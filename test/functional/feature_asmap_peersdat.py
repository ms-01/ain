#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the handling of the peers.dat file when an ASMap is used.

With an ASMap the address manager is written in the format 3, which stores the
fingerprint of the bucketing method after the bucket section. A change of the
ASMap (or the removal of it) re-buckets the entries of the address manager.

A file in a format that the node does not understand (e.g. a file that was
written by a newer version of the node) is backed up and the node continues
with an empty address manager.
"""

import os

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
ASMAP_FILE = os.path.join(DATA_DIR, "asmap.raw")
ASMAP2_FILE = os.path.join(DATA_DIR, "asmap2.raw")

# The format byte of the address manager (the first byte of the addrman data,
# which follows the 4 byte network magic of the file).
FORMAT_LEGACY = 1
FORMAT_ADDRV2 = 2
FORMAT_ADDRV2_ASMAP = 3
# A format byte that no version of the node knows.
FORMAT_UNKNOWN = 4


class AsmapPeersDatTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def peers_dat(self, node=0):
        return os.path.join(self.nodes[node].datadir, "regtest", "peers.dat")

    def peers_dat_format(self, node=0):
        """The format byte of the peers.dat file of a node."""
        with open(self.peers_dat(node), "rb") as f:
            data = f.read()
        assert len(data) > 4
        return data[4]

    def run_test(self):
        # The node is already running (the test framework starts it).
        self.stop_node(0)

        self.log.info("With an ASMap the peers.dat is written in the format 3")
        self.start_node(0, extra_args=["-asmap={}".format(ASMAP_FILE)])
        self.stop_node(0)
        assert_equal(self.peers_dat_format(), FORMAT_ADDRV2_ASMAP)

        self.log.info("The peer cache survives a restart with the same ASMap")
        self.start_node(0, extra_args=["-asmap={}".format(ASMAP_FILE)])
        self.stop_node(0)
        assert_equal(self.peers_dat_format(), FORMAT_ADDRV2_ASMAP)

        self.log.info("The peer cache survives a restart with a different ASMap")
        self.start_node(0, extra_args=["-asmap={}".format(ASMAP2_FILE)])
        self.stop_node(0)
        assert_equal(self.peers_dat_format(), FORMAT_ADDRV2_ASMAP)

        self.log.info("The peer cache survives a restart without an ASMap")
        self.start_node(0)
        self.stop_node(0)
        assert self.peers_dat_format() in (FORMAT_LEGACY, FORMAT_ADDRV2)

        self.log.info("A peers.dat in an unknown format is backed up")
        # Write a file in the format 3 and patch its format byte, so that the
        # node does not understand it (this is the situation of a downgrade to
        # a node that does not know the format).
        self.start_node(0, extra_args=["-asmap={}".format(ASMAP_FILE)])
        self.stop_node(0)
        assert_equal(self.peers_dat_format(), FORMAT_ADDRV2_ASMAP)
        with open(self.peers_dat(0), "rb") as f:
            data = bytearray(f.read())
        data[4] = FORMAT_UNKNOWN
        with open(self.peers_dat(0), "wb") as f:
            f.write(data)

        with self.nodes[0].assert_debug_log(
            expected_msgs=["Unsupported version of peers.dat"]
        ):
            self.start_node(0)
        self.stop_node(0)
        assert os.path.exists(self.peers_dat(0) + ".bak")
        # The node continues with an empty address manager and writes the file
        # in one of the classic formats again.
        assert self.peers_dat_format() in (FORMAT_LEGACY, FORMAT_ADDRV2)


if __name__ == "__main__":
    AsmapPeersDatTest().main()
