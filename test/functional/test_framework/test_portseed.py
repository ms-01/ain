#!/usr/bin/env python3
# Copyright (c) 2026 The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for portseed collision avoidance in test_runner and util."""

import argparse
import os
import socket
import sys
import unittest
from unittest import mock

FUNCTIONAL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if FUNCTIONAL_DIR not in sys.path:
    sys.path.insert(0, FUNCTIONAL_DIR)

import test_runner  # noqa: E402
from test_framework import util  # noqa: E402


class PortseedAllocationTest(unittest.TestCase):
    def _make_handler(self, portseed_offset=0):
        return test_runner.TestHandler(
            num_tests_parallel=4,
            tests_dir=FUNCTIONAL_DIR + "/",
            tmpdir="/tmp",
            test_list=[],
            flags=[],
            timeout_duration=60,
            use_term_control=False,
            portseed_offset=portseed_offset,
        )

    def test_ports_for_portseed_matches_rpc_port_without_mutating_global(self):
        prev_seed = util.PortSeed.n
        try:
            util.PortSeed.n = 42
            ports = util.ports_for_portseed(151, max_nodes=util.MAX_NODES)
            self.assertEqual(util.PortSeed.n, 42)
            # 12 nodes * (5 base ports + 1 extra gRPC port) = 72 distinct ports
            self.assertEqual(len(ports), util.MAX_NODES * 6)
            # Node 8, wsport (i=4) for portseed 151 lands on 19551
            self.assertIn(19551, ports)
            # Verify gRPC + 1 is included
            grpc_node0 = util._rpc_port_for_seed(0, 2, 151)
            self.assertIn(grpc_node0, ports)
            self.assertIn(grpc_node0 + 1, ports)
        finally:
            util.PortSeed.n = prev_seed

    def test_is_port_available_detects_bound_listening_socket(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            bound_port = srv.getsockname()[1]
            self.assertFalse(util.is_port_available(bound_port))
        self.assertTrue(util.is_port_available(bound_port))

    def test_allocate_portseed_skips_bound_port_in_occupied_range(self):
        # Bind the exact port (19551) that portseed=151 would use for node 8 wsport
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                srv.bind(("127.0.0.1", 19551))
            except OSError:
                self.skipTest("Port 19551 is already in use on this host")
            srv.listen(1)

            handler = self._make_handler(portseed_offset=151)
            seed, ports = handler._allocate_portseed()
            self.assertNotEqual(seed, 151)
            self.assertNotIn(19551, ports)
            self.assertTrue(ports.issubset(handler.active_ports))

    def test_allocate_portseed_avoids_active_job_overlap(self):
        handler = self._make_handler(portseed_offset=0)
        with mock.patch.object(test_runner, "is_port_available", return_value=True):
            seed1, ports1 = handler._allocate_portseed()
            seed2, ports2 = handler._allocate_portseed()
            self.assertEqual(seed1, 0)
            # seed=1 overlaps seed=0 on gRPC's second port (grpcport + 1), so seed=2 is chosen
            self.assertEqual(seed2, 2)
            self.assertTrue(ports1.isdisjoint(ports2))
            self.assertEqual(handler.active_ports, ports1 | ports2)

            # Releasing seed1's ports frees them cleanly
            handler.active_ports.difference_update(ports1)
            self.assertEqual(handler.active_ports, ports2)

    def test_allocate_portseed_raises_when_all_seeds_exhausted(self):
        handler = self._make_handler(portseed_offset=0)
        with mock.patch.object(test_runner, "is_port_available", return_value=False):
            with self.assertRaises(RuntimeError):
                handler._allocate_portseed()

    def test_parse_portseed_offset_validation(self):
        self.assertEqual(test_runner._parse_portseed_offset("0"), 0)
        self.assertEqual(test_runner._parse_portseed_offset("500"), 500)
        with self.assertRaises(argparse.ArgumentTypeError):
            test_runner._parse_portseed_offset("-1")
        with self.assertRaises(argparse.ArgumentTypeError):
            test_runner._parse_portseed_offset("abc")

    def test_get_port_min_validation(self):
        with mock.patch.dict(os.environ, {"TEST_RUNNER_PORT_MIN": "20000"}):
            self.assertEqual(util._get_port_min(), 20000)
        with mock.patch.dict(os.environ, {"TEST_RUNNER_PORT_MIN": "0"}):
            with self.assertRaises(ValueError):
                util._get_port_min()
        with mock.patch.dict(os.environ, {"TEST_RUNNER_PORT_MIN": "65000"}):
            with self.assertRaises(ValueError):
                util._get_port_min()
        with mock.patch.dict(os.environ, {"TEST_RUNNER_PORT_MIN": "notanint"}):
            with self.assertRaises(ValueError):
                util._get_port_min()


if __name__ == "__main__":
    unittest.main()
