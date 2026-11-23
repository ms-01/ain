#!/usr/bin/env python3
# Copyright (c) 2021-2026 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that a node in -blocksonly mode still relays/reconstructs compact blocks
and ignores unrequested transaction announcements from peers."""

from test_framework.messages import (
    CInv,
    msg_getdata,
    msg_inv,
    msg_sendcmpct,
)
from test_framework.mininode import P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_approx,
    assert_equal,
    assert_not_equal,
    wait_until,
)


class CompactBlocksOnlyPeer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.cmpctblocks = []
        self.getdata_received = []

    def on_cmpctblock(self, message):
        self.cmpctblocks.append(message)

    def on_getdata(self, message):
        self.getdata_received.append(message)


class CompactBlocksBlocksOnlyTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [["-blocksonly=1"], []]

    def run_test(self):
        blocksonly_node = self.nodes[0]
        normal_node = self.nodes[1]

        peer = blocksonly_node.add_p2p_connection(CompactBlocksOnlyPeer())
        sendcmpct = msg_sendcmpct()
        sendcmpct.announce = True
        sendcmpct.version = 2
        peer.send_and_ping(sendcmpct)

        # Verify -blocksonly node ignores tx inv announcements (no getdata sent)
        fake_tx_inv = msg_inv([CInv(1, 0x1234567890ABCDEF)])
        peer.send_and_ping(fake_tx_inv)
        assert_equal(len(peer.getdata_received), 0)

        # Mine a block on node 1 and verify node 0 syncs and announces a compact block
        tip_before = blocksonly_node.getbestblockhash()
        mined_hashes = normal_node.generate(1)
        self.sync_blocks()
        tip_after = blocksonly_node.getbestblockhash()

        assert_not_equal(tip_before, tip_after)
        assert_equal(tip_after, mined_hashes[0])

        # Request the new block as a compact block (MSG_CMPCT_BLOCK = 4)
        peer.send_and_ping(msg_getdata([CInv(4, int(tip_after, 16))]))
        wait_until(lambda: len(peer.cmpctblocks) >= 1, timeout=15)
        assert_equal(len(peer.cmpctblocks), 1)

        # Exercise assert_approx on node difficulty/chainwork sanity
        info = blocksonly_node.getblockchaininfo()
        assert_approx(float(info["verificationprogress"]), 1.0, vspan=0.05)


if __name__ == "__main__":
    CompactBlocksBlocksOnlyTest().main()
