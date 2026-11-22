#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that the orphan pool evicts per peer.

A peer that floods the node with orphan transactions must not be able to evict the
orphans of other peers: only the announcements of the offending peer are evicted.

The test also covers the multi-announcer semantics of the orphan pool: when a peer
disconnects, only its own announcement is removed, so that an orphan that was announced
by another peer as well survives the disconnect.
"""
import random

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.mininode import P2PDataStore
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# A script that anyone can spend, but that is not the trivial "OP_TRUE" script.
SCRIPT_PUB_KEY_OP_TRUE = b"\x51\x75" * 15 + b"\x51"


class OrphanPerPeerEvictionTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # A small orphan limit, so that the flooding peer exceeds it quickly.
        self.extra_args = [
            [
                "-acceptnonstdtxn=1",
                "-dummypos=1",
                "-maxorphantx=10",
                "-debug=mempool",
            ]
        ]
        self.setup_clean_chain = True

    def create_orphan(self, parent_hash, parent_index=0):
        """Create a transaction that spends an output that does not exist (yet)."""
        tx = CTransaction()
        tx.vin.append(CTxIn(outpoint=COutPoint(parent_hash, parent_index)))
        tx.vout.append(
            CTxOut(nValue=10 * COIN - 12000, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE)
        )
        tx.calc_sha256()
        return tx

    def create_parent(self, coinbase_hash):
        """Create a transaction that spends the coinbase of the given block.

        The transaction has two outputs, so that two independent chains of transactions can
        spend them.
        """
        tx = CTransaction()
        tx.vin.append(CTxIn(outpoint=COutPoint(coinbase_hash, 0)))
        tx.vout.append(
            CTxOut(nValue=25 * COIN - 12000, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE)
        )
        tx.vout.append(
            CTxOut(nValue=25 * COIN - 12000, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE)
        )
        tx.calc_sha256()
        return tx

    def create_spending(self, parent_tx, parent_index, value_in, value_out):
        """Create a transaction that spends an output of the given transaction."""
        tx = CTransaction()
        tx.vin.append(CTxIn(outpoint=COutPoint(parent_tx.sha256, parent_index)))
        tx.vout.append(CTxOut(nValue=value_out, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE))
        tx.calc_sha256()
        assert value_out < value_in
        return tx

    def run_test(self):
        node = self.nodes[0]

        peer_honest = node.add_p2p_connection(P2PDataStore())
        peer_flood = node.add_p2p_connection(P2PDataStore())

        best_block = node.getbestblockhash()
        tip = int(best_block, 16)
        best_block_time = node.getblock(best_block)["time"]
        block_time = best_block_time + 1

        self.log.info("Create a new block with an anyone-can-spend coinbase.")
        height = 1
        block = create_block(tip, create_coinbase(height), block_time)
        block.solve()
        block1 = block
        tip = block.sha256
        node.p2p.send_blocks_and_test([block], node, success=True)

        self.log.info("Mature the block.")
        node.generate(100)

        self.log.info(
            "Create the parent transaction and the orphan of the honest peer."
        )
        tx_parent = self.create_parent(block1.vtx[0].sha256)
        tx_orphan_honest = self.create_orphan(tx_parent.sha256)

        self.log.info("The honest peer announces its orphan.")
        peer_honest.send_txs_and_test([tx_orphan_honest], node, success=False)
        assert_equal(0, len(node.getrawmempool()))

        self.log.info("The flooding peer announces more orphans than the limit allows.")
        flood_txs = []
        for _ in range(20):
            random_hash = random.randrange(2**256)
            flood_txs.append(self.create_orphan(random_hash))
        with node.assert_debug_log(expected_msgs=["orphanage overflow"]):
            peer_flood.send_txs_and_test(flood_txs, node, success=False)

        self.log.info("The honest peer announces the missing parent.")
        peer_honest.send_txs_and_test([tx_parent], node, success=True)

        self.log.info(
            "The orphan of the honest peer survived the flood and is accepted."
        )
        wait_until(lambda: tx_orphan_honest.hash in node.getrawmempool(), timeout=30)
        assert tx_orphan_honest.hash in node.getrawmempool()
        assert tx_parent.hash in node.getrawmempool()

        self.log.info("The orphans of the flooding peer did not enter the mempool.")
        for tx in flood_txs:
            assert tx.hash not in node.getrawmempool()

        self.log.info("Both peers announce the same orphan.")
        tx_parent_shared = self.create_spending(
            tx_parent, 1, 25 * COIN - 12000, 20 * COIN - 12000
        )
        tx_orphan_shared = self.create_orphan(tx_parent_shared.sha256)
        peer_flood.send_txs_and_test([tx_orphan_shared], node, success=False)
        peer_honest.send_txs_and_test([tx_orphan_shared], node, success=False)

        self.log.info("Disconnecting the honest peer keeps the shared orphan alive.")
        peer_honest.peer_disconnect()
        wait_until(lambda: len(node.getpeerinfo()) == 1, timeout=30)

        self.log.info("The orphan is still processed after the disconnect.")
        peer_flood.send_txs_and_test([tx_parent_shared], node, success=True)
        wait_until(lambda: tx_orphan_shared.hash in node.getrawmempool(), timeout=30)
        assert tx_orphan_shared.hash in node.getrawmempool()

        node.disconnect_p2ps()


if __name__ == "__main__":
    OrphanPerPeerEvictionTest().main()
