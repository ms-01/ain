#!/usr/bin/env python3
# Copyright (c) 2024-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.
"""Test the initial block download GETHEADERS guardrails and the masternode version gate.

Two dead ends of the peer to peer sync are covered:

* While the node is in the initial block download it must not silently drop the
  GETHEADERS requests of its peers. If the active chain has at least the minimum chain
  work, the headers are served like upstream Bitcoin Core does it. If the active chain has
  less than the minimum chain work, an empty headers message is sent so that the peer does
  not consider the node unresponsive.

* A header that is rejected only because it does not signal the masternode version gate
  (VERSION_BIT_BALANCE_OVERFLOW) is a version skew of the peer software and must neither
  lead to a ban nor to a disconnect. A header that is invalid for any other reason keeps
  its ban worthy classification.
"""

import os
import time
from io import BytesIO

from test_framework.messages import (
    CBlockHeader,
    msg_getheaders,
    msg_headers,
)
from test_framework.mininode import (
    P2PInterface,
    mininode_lock,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# Version bit that signals the balance overflow fix. Headers that do not signal it are
# rejected by the masternode version gate.
VERSION_BIT_BALANCE_OVERFLOW = 1 << 24

# Chain work that is not reached by any chain that is built in this test.
HUGE_CHAIN_WORK = "0x" + "f" * 64

# Age of the tip of the node in seconds. It is larger than the maximum tip age of the node
# so that the node stays in the initial block download.
OLD_TIP_AGE = 20 * 60 * 60


class HeadersPeer(P2PInterface):
    """A peer that records the headers messages that it receives."""

    def __init__(self):
        super().__init__()
        self.received_headers = []

    def on_headers(self, message):
        self.received_headers.append(message)


class P2PGetHeadersGuardrailsTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.extra_args = [
            # Node 0 serves the headers while it is in the initial block download.
            ["-maxtipage=1"],
            # Node 1 has less chain work than the minimum chain work.
            ["-minimumchainwork=" + HUGE_CHAIN_WORK],
            # Node 2 enforces the masternode version gate from height 1 on. The proof of
            # stake checks are enabled so that the stake modifier of a header is validated.
            ["-mnversiongateheight=1", "-dummypos=0"],
            # Node 3 mines blocks that do not signal the version gate. Its version gate
            # starts at height 2 so that it can mine a block at height 1.
            ["-mnversiongateheight=2", "-blockversion=4", "-dummypos=0"],
        ]

    def setup_network(self):
        # The nodes must not be connected to each other: every test case works on its own
        # chain and the blocks of the nodes would otherwise be exchanged.
        self.setup_nodes()

    def run_test(self):
        self.test_headers_served_during_ibd()
        self.test_empty_headers_during_ibd_with_low_chain_work()
        self.test_version_skew_is_not_punished()
        self.test_invalid_header_is_banned()

    # Helpers

    def debug_log(self, node):
        log_path = os.path.join(node.datadir, node.chain, "debug.log")
        with open(log_path, encoding="utf-8") as log_file:
            return log_file.read()

    def wait_for_log(self, node, message):
        """Wait until the given message appears in the debug log of the node."""
        wait_until(lambda: message in self.debug_log(node), timeout=60)

    def request_headers(self, peer, locator):
        """Send a getheaders request and return the headers message of the node."""
        request = msg_getheaders()
        request.locator.vHave = locator
        peer.send_message(request)
        wait_until(lambda: peer.received_headers, timeout=60, lock=mininode_lock)
        return peer.received_headers[-1]

    def block_header(self, node, block_hash):
        """Return the header of the given block as it is sent over the wire."""
        header = CBlockHeader()
        header.deserialize(
            BytesIO(bytes.fromhex(node.getblockheader(block_hash, False)))
        )
        return header

    def send_headers(self, peer, header):
        """Send a headers message with a single header to the node."""
        message = msg_headers()
        message.headers = [header]
        peer.send_message(message)

    # Test cases

    def test_headers_served_during_ibd(self):
        self.log.info(
            "Test that the headers are served during the initial block download"
        )
        node = self.nodes[0]

        # Mine two blocks with a timestamp that is old enough to leave the node in the
        # initial block download after the restart below.
        old_time = int(time.time()) - OLD_TIP_AGE
        node.set_mocktime(old_time)
        node.setmocktime(old_time)
        node.generate(2)
        genesis = node.getblockhash(0)
        tip = node.getbestblockhash()

        # Restart the node with the current time. The tip of the node is now older than the
        # maximum tip age of the node, so it stays in the initial block download.
        node.set_mocktime(old_time + OLD_TIP_AGE)
        self.restart_node(0)

        peer = HeadersPeer()
        node.add_p2p_connection(peer)
        headers = self.request_headers(peer, [int(genesis, 16)])

        # The headers of the two mined blocks are served, the request is not dropped.
        assert_equal(len(headers.headers), 2)
        assert_equal(headers.headers[-1].hash, tip)
        self.wait_for_log(node, "served during the initial block download")

    def test_empty_headers_during_ibd_with_low_chain_work(self):
        self.log.info(
            "Test that an empty headers message is sent if the chain work is too low"
        )
        node = self.nodes[1]

        peer = HeadersPeer()
        node.add_p2p_connection(peer)
        headers = self.request_headers(peer, [int(node.getblockhash(0), 16)])

        # The active chain has less than the minimum chain work, so an empty headers
        # message is sent instead of dropping the request.
        assert_equal(len(headers.headers), 0)
        self.wait_for_log(node, "answered with an empty headers message")

    def test_version_skew_is_not_punished(self):
        self.log.info("Test that a header that misses the version gate is not punished")
        node = self.nodes[2]
        old_node = self.nodes[3]

        # Both nodes mine a block at the same height. The block of the old node does not
        # signal the version gate, the block of the node does.
        node.generate(1)
        old_node.generate(1)
        header = self.block_header(old_node, old_node.getbestblockhash())
        assert_equal(header.nVersion & VERSION_BIT_BALANCE_OVERFLOW, 0)

        peer = HeadersPeer()
        node.add_p2p_connection(peer)
        self.send_headers(peer, header)
        self.wait_for_log(node, "header rejected by the masternode version gate")

        # The peer is running software that is too old, it is not misbehaving. It must
        # neither be banned nor disconnected.
        assert "BAN THRESHOLD EXCEEDED" not in self.debug_log(node)
        assert_equal(node.listbanned(), [])
        assert peer.is_connected

    def test_invalid_header_is_banned(self):
        self.log.info("Test that a header that is invalid for another reason is banned")
        node = self.nodes[2]
        old_node = self.nodes[3]

        # A header with invalid diff bits is invalid for another reason than the missing
        # version bit and must keep its ban worthy classification.
        header = self.block_header(old_node, old_node.getbestblockhash())
        header.nBits ^= 1
        peer = HeadersPeer()
        node.add_p2p_connection(peer)
        self.send_headers(peer, header)
        self.wait_for_log(node, "BAN THRESHOLD EXCEEDED")
        peer.wait_for_disconnect()

        # A header with an invalid stake modifier is rejected after the version gate check
        # and must be punished as well.
        node.clearbanned()
        header = self.block_header(old_node, old_node.getbestblockhash())
        header.stakeModifier ^= 1
        peer = HeadersPeer()
        node.add_p2p_connection(peer)
        self.send_headers(peer, header)
        self.wait_for_log(node, "bad PoS stake modifier")
        self.wait_for_log(node, "BAN THRESHOLD EXCEEDED")
        peer.wait_for_disconnect()


if __name__ == "__main__":
    P2PGetHeadersGuardrailsTest().main()
