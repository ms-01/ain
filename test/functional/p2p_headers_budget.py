#!/usr/bin/env python3
# Copyright (c) 2026-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.
"""Test the budget for the headers that do not continue the best header chain.

On a proof of stake chain a peer can produce an arbitrary number of headers that
pass all header level checks: a key pair is enough. Every header that is added to
the block index costs memory (the index entry) and disk space (the block index
database), so the node bounds the number of headers that a peer may add that do
not continue the best header chain.

The test covers:

* A peer that sends a long chain of headers that forks off the best header chain
  only gets a bounded amount of them accepted. The peer is punished with a small
  misbehaviour score only: it is neither disconnected nor banned.
* Headers that continue the best header chain are not affected by the budget, so
  the headers first initial block download keeps working.
"""

import os
import time
from io import BytesIO

from test_framework.messages import (
    CBlockHeader,
    msg_headers,
)
from test_framework.mininode import (
    P2PInterface,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# Number of side chain headers that a peer may add to the block index.
PEER_BUDGET = 100
# Number of headers that the peer sends in the test.
HEADERS = 150


class HeadersPeer(P2PInterface):
    """A peer that sends headers to the node."""

    def send_headers(self, headers):
        message = msg_headers()
        message.headers = headers
        self.send_message(message)


class P2PHeadersBudgetTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [
            [
                "-maxunanchoredheadersperpeer=%d" % PEER_BUDGET,
                "-maxunanchoredheaders=1000",
            ],
            [
                "-maxunanchoredheadersperpeer=%d" % PEER_BUDGET,
                "-maxunanchoredheaders=1000",
            ],
        ]

    def setup_network(self):
        # The nodes must not be connected to each other: every test case works on
        # its own chain.
        self.setup_nodes()

    def run_test(self):
        self.test_side_chain_headers_are_bounded()
        self.test_best_chain_headers_are_not_bounded()

    # Helpers

    def debug_log(self, node):
        log_path = os.path.join(node.datadir, node.chain, "debug.log")
        with open(log_path, encoding="utf-8") as log_file:
            return log_file.read()

    def block_header(self, node, block_hash):
        """Return the header of the given block as it is sent over the wire."""
        header = CBlockHeader()
        header.deserialize(
            BytesIO(bytes.fromhex(node.getblockheader(block_hash, False)))
        )
        header.calc_sha256()
        return header

    def build_headers(self, node, base_hash, count, start_time):
        """Build a chain of headers that continues the given block."""
        base = self.block_header(node, base_hash)
        headers = []
        previous = base
        for i in range(count):
            header = CBlockHeader()
            header.nVersion = base.nVersion
            header.hashPrevBlock = int(previous.hash, 16)
            header.hashMerkleRoot = 0
            header.nTime = start_time + i
            header.nBits = base.nBits
            header.stakeModifier = 0
            header.nHeight = base.nHeight + i + 1
            header.nMintedBlocks = 0
            header.sig = b"0" * 65
            header.calc_sha256()
            headers.append(header)
            previous = header
        return headers

    def header_known(self, node, header_hash):
        """Return whether the node knows the header with the given hash."""
        try:
            node.getblockheader(header_hash)
        except Exception:
            return False
        return True

    # Test cases

    def test_side_chain_headers_are_bounded(self):
        self.log.info("Test that the side chain headers of a peer are bounded")
        node = self.nodes[0]
        node.generate(2)
        genesis = node.getblockhash(0)

        peer = HeadersPeer()
        node.add_p2p_connection(peer)

        # A chain of headers that forks off at the genesis block. It does not
        # continue the best header chain of the node, so all of its headers are
        # side chain material.
        headers = self.build_headers(node, genesis, HEADERS, int(time.time()) + 10)
        peer.send_headers(headers)

        # Only the headers that are within the budget of the peer are added to the
        # block index.
        wait_until(
            lambda: self.header_known(node, headers[PEER_BUDGET - 1].hash),
            timeout=60,
        )
        assert not self.header_known(node, headers[PEER_BUDGET].hash)

        # The peer exceeded its budget: it is punished with a small misbehaviour
        # score only. It is neither disconnected nor banned.
        wait_until(
            lambda: "the side chain header budget is exhausted" in self.debug_log(node),
            timeout=60,
        )
        assert "BAN THRESHOLD EXCEEDED" not in self.debug_log(node)
        assert_equal(node.listbanned(), [])
        assert peer.is_connected

        # The node is still able to serve its own chain.
        assert_equal(node.getbestblockhash(), node.getblockhash(2))

    def test_best_chain_headers_are_not_bounded(self):
        self.log.info(
            "Test that headers that continue the best header chain are not bounded"
        )
        node = self.nodes[1]
        node.generate(2)
        tip = node.getbestblockhash()

        peer = HeadersPeer()
        node.add_p2p_connection(peer)

        # A chain of headers that continues the best header chain of the node.
        headers = self.build_headers(node, tip, HEADERS, int(time.time()) + 10)
        peer.send_headers(headers)

        # All headers are added to the block index, even though there are more of
        # them than the peer is allowed to add as side chain headers.
        wait_until(
            lambda: self.header_known(node, headers[-1].hash),
            timeout=60,
        )
        assert "the side chain header budget is exhausted" not in self.debug_log(node)
        assert peer.is_connected


if __name__ == "__main__":
    P2PHeadersBudgetTest().main()
