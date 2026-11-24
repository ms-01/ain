#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP324 v2 transport negotiation, block relay, and v1 fallback."""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    wait_until,
)


class V2TransportTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-v2transport=1"],
            ["-v2transport=1"],
            ["-v2transport=0"],
        ]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        node0, node1, node2 = self.nodes

        self.log.info("1. Test v2 <-> v2 connection between node0 and node1")
        connect_nodes(node0, 1)
        wait_until(lambda: len(node0.getpeerinfo()) == 1 and len(node1.getpeerinfo()) == 1, timeout=15)

        p0 = node0.getpeerinfo()[0]
        p1 = node1.getpeerinfo()[0]
        assert_equal(p0["transport_protocol_type"], "v2")
        assert_equal(p1["transport_protocol_type"], "v2")
        assert_equal(len(p0["session_id"]), 64)
        assert_equal(p0["session_id"], p1["session_id"])

        # Mine a block on node0 and verify relay over v2 transport
        node0.generate(1)
        self.sync_blocks([node0, node1])
        assert_equal(node1.getblockcount(), 1)

        self.log.info("2. Test v1 (node2) -> v2 (node0) transparent responder fallback")
        connect_nodes(node2, 0)
        wait_until(lambda: len(node2.getpeerinfo()) == 1 and len(node0.getpeerinfo()) == 2, timeout=15)
        p2 = node2.getpeerinfo()[0]
        assert_equal(p2["transport_protocol_type"], "v1")
        assert_equal(p2["session_id"], "")
        self.sync_blocks([node0, node1, node2])
        assert_equal(node2.getblockcount(), 1)

        self.log.info("3. Test v2 (node1) -> v1 (node2) initiator reconnect fallback")
        connect_nodes(node1, 2)
        wait_until(
            lambda: any(p["transport_protocol_type"] == "v1" for p in node1.getpeerinfo()),
            timeout=20,
        )
        node1.generate(1)
        self.sync_blocks([node0, node1, node2])
        assert_equal(node2.getblockcount(), 2)


if __name__ == "__main__":
    V2TransportTest().main()
