#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the ASN based address bucketing (-asmap).

When an ASMap file is configured (-asmap), the node buckets the addresses of
its peers by the autonomous system they belong to instead of by the /16 prefix
of the address. Without the option the classic grouping is used and the node
behaves exactly as before.
"""

import hashlib
import os

from test_framework.test_framework import DefiTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
ASMAP_FILE = os.path.join(DATA_DIR, "asmap.raw")
# A file that is not a valid ASMap.
INVALID_ASMAP_FILE = os.path.join(DATA_DIR, "asmap_invalid.raw")


class AsmapTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        # Two nodes, so that the first one has a peer (on the loopback
        # interface) whose mapped AS can be checked.
        self.num_nodes = 2

    def asmap_version(self, path):
        """The version of an ASMap file: the SHA256 of its bytes."""
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    def run_test(self):
        self.log.info("A node without -asmap does not report an ASMap version")
        # The nodes are already running (the test framework starts them).
        info = self.nodes[0].getnetworkinfo()
        assert "asmap_version" not in info
        # The peers of a regtest node are on the loopback interface, which is
        # not routable and therefore never mapped to an AS.
        peers = self.nodes[0].getpeerinfo()
        assert len(peers) > 0
        for peer in peers:
            assert_equal(peer["mapped_as"], 0)
        self.stop_node(0)

        self.log.info("A node with -asmap reports the version of the ASMap")
        self.start_node(0, extra_args=["-asmap={}".format(ASMAP_FILE)])
        info = self.nodes[0].getnetworkinfo()
        assert_equal(info["asmap_version"], self.asmap_version(ASMAP_FILE))
        # The loopback peers are not routable, so they are not mapped.
        for peer in self.nodes[0].getpeerinfo():
            assert_equal(peer["mapped_as"], 0)
        self.stop_node(0)

        self.log.info("A node with a malformed ASMap refuses to start")
        self.nodes[0].assert_start_raises_init_error(
            extra_args=["-asmap={}".format(INVALID_ASMAP_FILE)],
            expected_msg="Failed to load the ASMap file",
            match=ErrorMatch.PARTIAL_REGEX,
        )

        self.log.info("A node with a missing ASMap refuses to start")
        self.nodes[0].assert_start_raises_init_error(
            extra_args=[
                "-asmap={}".format(os.path.join(DATA_DIR, "does_not_exist.raw"))
            ],
            expected_msg="Failed to load the ASMap file",
            match=ErrorMatch.PARTIAL_REGEX,
        )


if __name__ == "__main__":
    AsmapTest().main()
