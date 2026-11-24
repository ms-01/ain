#!/usr/bin/env python3
# Copyright (c) 2026-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.
"""Test PoS anchored header-tip selection (-headertipmode=anchored|chainwork).

Covers:
  (a) Fork at the validated tip H (equal anchored height): forged chain temporarily
      wins tie-break, then snaps back to the honest chain as soon as honest block H+1
      validates.
  (b) Honest reorg: competing honest chain with more work completes reorg cleanly
      once its blocks are downloaded and validated.
  (c) Fork below the validated tip H: in default anchored mode, pindexBestHeader stays
      on the honest chain and the forged side-chain is bounded by the unanchored header
      budget without banning the peer.
  (d) -headertipmode=chainwork negative control: historical forged fork below H does
      hijack pindexBestHeader in legacy chainwork mode.
  (e) Restart / -reindex and rollback (invalidateblock / reconsiderblock): header tip
      is reconstructed accurately and follows the surviving chain.
  (f) Regression: honest headers-first IBD completes without regression.
  (g) Post-restart reconstruction window: unvalidated header chain > 64 blocks above
      the validated tip persisted across a restart (pins the documented residual window).
"""

from io import BytesIO

from test_framework.messages import (
    CBlockHeader,
    msg_headers,
)
from test_framework.mininode import P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    disconnect_nodes,
    sync_blocks,
    wait_until,
)

PEER_BUDGET = 40


class HeadersPeer(P2PInterface):
    def send_headers(self, headers):
        message = msg_headers()
        message.headers = headers
        self.send_message(message)


class P2PHeaderTipSelectionTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [
            [
                "-rpccache=0",
                "-maxunanchoredheadersperpeer=%d" % PEER_BUDGET,
                "-maxunanchoredheaders=200",
            ],
            [
                "-rpccache=0",
                "-headertipmode=chainwork",
                "-maxunanchoredheadersperpeer=%d" % PEER_BUDGET,
                "-maxunanchoredheaders=200",
            ],
            [
                "-rpccache=0",
            ],
        ]

    def setup_network(self):
        self.setup_nodes()

    def block_header(self, node, block_hash):
        header = CBlockHeader()
        header.deserialize(
            BytesIO(bytes.fromhex(node.getblockheader(block_hash, False)))
        )
        header.calc_sha256()
        return header

    def build_forged_headers(self, node, base_hash, count, start_time):
        """Build a forged chain of headers extending base_hash."""
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
        try:
            node.getblockheader(header_hash)
            return True
        except Exception:
            return False

    def run_test(self):
        self.test_below_tip_fork_anchored_and_budget()
        self.test_below_tip_fork_chainwork_negative_control()
        self.test_at_tip_fork_and_snap_back()
        self.test_restart_reindex_and_rollback()
        self.test_honest_ibd_and_reorg()
        self.test_post_restart_long_unvalidated_chain()

    def test_below_tip_fork_anchored_and_budget(self):
        self.log.info("(c) Fork below validated tip in anchored mode stays on honest chain and obeys budget")
        node = self.nodes[0]
        node.generate(20)
        assert_equal(node.getblockchaininfo()["blocks"], 20)
        assert_equal(node.getblockchaininfo()["headers"], 20)

        # Fork from block 5 with 60 forged headers (height 65 > 20, higher nChainWork).
        fork_hash = node.getblockhash(5)
        start_time = node.getblockheader(fork_hash)["time"] + 1
        forged = self.build_forged_headers(node, fork_hash, 60, start_time)

        peer = node.add_p2p_connection(HeadersPeer())
        peer.send_headers(forged)
        peer.sync_with_ping()

        # pindexBestHeader must stay on the honest anchored chain (height 20).
        assert_equal(node.getblockchaininfo()["headers"], 20)
        # Side-chain budget (PEER_BUDGET=40) must cap the forged chain.
        assert self.header_known(node, forged[PEER_BUDGET - 1].hash)
        assert not self.header_known(node, forged[PEER_BUDGET].hash)
        # No peer ban storm: peer remains connected.
        assert peer.is_connected
        node.disconnect_p2ps()

    def test_below_tip_fork_chainwork_negative_control(self):
        self.log.info("(d) Negative control: -headertipmode=chainwork allows historical forged fork to hijack header tip")
        node = self.nodes[1]
        node.generate(20)
        assert_equal(node.getblockchaininfo()["headers"], 20)

        fork_hash = node.getblockhash(5)
        start_time = node.getblockheader(fork_hash)["time"] + 1
        forged = self.build_forged_headers(node, fork_hash, 30, start_time)

        peer = node.add_p2p_connection(HeadersPeer())
        peer.send_headers(forged)
        peer.sync_with_ping()

        # In legacy chainwork mode, the forged chain (height 35) hijacks pindexBestHeader.
        assert_equal(node.getblockchaininfo()["headers"], 35)
        node.disconnect_p2ps()

    def test_at_tip_fork_and_snap_back(self):
        self.log.info("(a) Fork at validated tip H wins tie-break temporarily, then snaps back on honest block H+1")
        node = self.nodes[0]
        tip_height = node.getblockchaininfo()["blocks"]
        tip_hash = node.getblockhash(tip_height)
        start_time = node.getblockheader(tip_hash)["time"] + 1

        # Fork directly at tip_height (equal anchored height) with 10 forged headers.
        forged = self.build_forged_headers(node, tip_hash, 10, start_time)
        peer = node.add_p2p_connection(HeadersPeer())
        peer.send_headers(forged)
        peer.sync_with_ping()

        # Equal anchored height -> forged chain temporarily wins nChainWork tie-break.
        assert_equal(node.getblockchaininfo()["headers"], tip_height + 10)

        # Mine 1 honest block (H+1): anchored height advances to H+1, snapping pindexBestHeader back.
        node.generate(1)
        assert_equal(node.getblockchaininfo()["blocks"], tip_height + 1)
        assert_equal(node.getblockchaininfo()["headers"], tip_height + 1)
        node.disconnect_p2ps()

    def test_restart_reindex_and_rollback(self):
        self.log.info("(e) Restart, -reindex, and invalidateblock/reconsiderblock preserve anchored header tip")
        node = self.nodes[0]
        expected_height = node.getblockchaininfo()["blocks"]

        # Restart and verify header tip is reconstructed identically.
        self.restart_node(0, extra_args=self.extra_args[0])
        assert_equal(node.getblockchaininfo()["blocks"], expected_height)
        assert_equal(node.getblockchaininfo()["headers"], expected_height)

        # Invalidate the top block (21): active tip drops to 20, and RecomputeBestHeader()
        # selects the surviving forged chain anchored at 20 (height 30).
        top_hash = node.getblockhash(expected_height)
        prev_hash = node.getblockhash(expected_height - 1)
        node.invalidateblock(top_hash)
        assert_equal(node.getblockchaininfo()["blocks"], expected_height - 1)
        assert_equal(node.getblockchaininfo()["headers"], expected_height - 1 + 10)

        # Invalidate block 20 as well: the forged chain anchored at 20 is now BLOCK_FAILED_CHILD,
        # so RecomputeBestHeader() moves the header tip cleanly to the surviving honest chain (19).
        node.invalidateblock(prev_hash)
        assert_equal(node.getblockchaininfo()["blocks"], expected_height - 2)
        assert_equal(node.getblockchaininfo()["headers"], expected_height - 2)

        # Reconsider and verify full recovery.
        node.reconsiderblock(prev_hash)
        node.reconsiderblock(top_hash)
        assert_equal(node.getblockchaininfo()["blocks"], expected_height)
        assert_equal(node.getblockchaininfo()["headers"], expected_height)

        # Reindex and verify reconstruction.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        assert_equal(node.getblockchaininfo()["blocks"], expected_height)
        assert_equal(node.getblockchaininfo()["headers"], expected_height)

    def test_honest_ibd_and_reorg(self):
        self.log.info("(b, f) Honest headers-first IBD and honest reorg succeed without regression")
        node0 = self.nodes[0]
        node2 = self.nodes[2]

        # Honest headers-first IBD: node2 (at height 0) syncs from node0.
        connect_nodes(node0, 2)
        sync_blocks([node0, node2])
        assert_equal(node2.getblockchaininfo()["blocks"], node0.getblockchaininfo()["blocks"])
        assert_equal(node2.getblockchaininfo()["headers"], node0.getblockchaininfo()["headers"])

        # Honest reorg: disconnect, mine 3 blocks on node0 and 6 blocks on node2, then reconnect.
        disconnect_nodes(node0, 2)
        node0.generate(3)
        node2.generate(6)
        target_height = node2.getblockchaininfo()["blocks"]

        connect_nodes(node0, 2)
        wait_until(lambda: node0.getblockchaininfo()["blocks"] == target_height, timeout=30)
        assert_equal(node0.getblockchaininfo()["headers"], target_height)

    def test_post_restart_long_unvalidated_chain(self):
        self.log.info("(g) Post-restart window: >64 unvalidated headers above validated tip persisted across restart")
        node = self.nodes[0]
        disconnect_nodes(node, 2)
        tip_height = node.getblockchaininfo()["blocks"]
        tip_hash = node.getblockhash(tip_height)
        start_time = node.getblockheader(tip_hash)["time"] + 1

        # Deliver 70 unvalidated headers (> MAX_ANCHORED_WALK_DEPTH=64) extending the validated tip.
        forged_len = 70
        forged = self.build_forged_headers(node, tip_hash, forged_len, start_time)
        peer = node.add_p2p_connection(HeadersPeer())
        peer.send_headers(forged)
        peer.sync_with_ping()
        assert_equal(node.getblockchaininfo()["headers"], tip_height + forged_len)
        node.disconnect_p2ps()

        # Restart before any new honest block validates: pins the documented post-restart window
        # where LoadBlockIndex() runs without active_tip and the persisted >64-header unvalidated
        # chain remains pindexBestHeader across the restart, then snaps back once honest block
        # H+1 validates via ConnectTip().
        self.restart_node(0, extra_args=self.extra_args[0])
        assert_equal(node.getblockchaininfo()["blocks"], tip_height)
        assert_equal(node.getblockchaininfo()["headers"], tip_height + forged_len)

        node.generate(1)
        assert_equal(node.getblockchaininfo()["blocks"], tip_height + 1)
        assert_equal(node.getblockchaininfo()["headers"], tip_height + 1)


if __name__ == "__main__":
    P2PHeaderTipSelectionTest().main()
