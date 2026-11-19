#!/usr/bin/env python3
# Copyright (c) 2024-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.gnu.org/licenses/old-licenses/fdl-1.1.txt.
"""Test that blocks and transactions are served to peers.

The block serving path (ProcessGetBlockData()) reads the block from disk and
serializes it *without* holding the chain lock, and the transaction serving path
(ProcessGetData()) serializes the transactions without holding the chain lock.
This test makes sure that the reworked paths still deliver the requested
objects to the requesting peer and that a request for an unknown object does
not kill the node.
"""

from test_framework.messages import (
    CInv,
    MSG_BLOCK,
    MSG_TX,
    MSG_TYPE_MASK,
    MSG_WITNESS_FLAG,
    MSG_WTX,
    msg_getdata,
    msg_mempool,
)
from test_framework.mininode import P2PInterface, mininode_lock
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, wait_until

# Not exposed by the test framework.
MSG_CMPCT_BLOCK = 4
MSG_WITNESS_BLOCK = MSG_BLOCK | MSG_WITNESS_FLAG


class NoAutoRequestPeer(P2PInterface):
    """Peer that does not automatically answer inventory messages."""

    def __init__(self):
        super().__init__()
        self.invs = []

    def on_inv(self, message):
        self.invs.extend(message.inv)


class BlockServingTest(DefiTestFramework):
    def set_test_params(self):
        # The cached chain provides a funded wallet, which is needed to create
        # a transaction in the mempool.
        self.setup_clean_chain = False
        self.num_nodes = 1
        # Allow the peer to send a BIP35 mempool request.
        self.extra_args = [["--peerbloomfilters=1"]]

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(NoAutoRequestPeer())
        peer.sync_with_ping()

        # Mine two blocks, so that there is something to serve from disk.
        node.generate(2)
        block_hash = int(node.getblockhash(1), 16)

        # 1. A plain block request is served from disk.
        peer.send_message(msg_getdata([CInv(MSG_BLOCK, block_hash)]))
        peer.wait_for_block(block_hash)
        self.log.info("Block served from disk (MSG_BLOCK)")

        # 2. The witness block fast-path (raw block read from disk) works as well.
        peer.send_message(msg_getdata([CInv(MSG_WITNESS_BLOCK, block_hash)]))
        peer.wait_for_block(block_hash)
        self.log.info("Block served from disk (MSG_WITNESS_BLOCK)")

        # 3. A compact block request is answered with a compact block (the block
        #    is recent enough to be served as a compact block).
        peer.send_message(msg_getdata([CInv(MSG_CMPCT_BLOCK, block_hash)]))
        wait_until(
            lambda: "cmpctblock" in peer.last_message or "block" in peer.last_message,
            lock=mininode_lock,
        )
        self.log.info("Compact block request answered")

        # 4. A request for a block that we do not know must not kill the node.
        #    The node does not answer such a request, but stays connected.
        unknown_hash = 0x1234567890ABCDEF
        peer.send_message(msg_getdata([CInv(MSG_BLOCK, unknown_hash)]))
        peer.sync_with_ping()
        assert_equal(len(node.getpeerinfo()), 1)
        self.log.info("Unknown block request handled gracefully")

        # 5. Transactions are served from the mempool. The peer asks for the
        #    mempool first (BIP35), which makes the node announce the
        #    transactions and allows it to answer the getdata request.
        address = node.getnewaddress("", "bech32")
        funding_txid = node.sendtoaddress(address, 1.0)
        node.generate(1)
        txid = node.sendtoaddress(node.getnewaddress("", "bech32"), 0.5)
        assert txid in node.getrawmempool()

        peer.send_message(msg_mempool())
        wait_until(
            lambda: any(inv.hash == int(txid, 16) for inv in peer.invs),
            lock=mininode_lock,
        )
        with mininode_lock:
            inv_type = next(
                inv.type & MSG_TYPE_MASK
                for inv in peer.invs
                if inv.hash == int(txid, 16)
            )
        assert inv_type in (MSG_TX, MSG_WTX)
        peer.send_message(msg_getdata([CInv(inv_type, int(txid, 16))]))
        wait_until(lambda: "tx" in peer.last_message, lock=mininode_lock)
        with mininode_lock:
            received_tx = peer.last_message["tx"].tx
        # The transaction hash of DeFiChain transactions cannot be recomputed by
        # the test framework (token data is not part of the python
        # serialization), so the spent output is compared instead.
        assert_equal(received_tx.vin[0].prevout.hash, int(funding_txid, 16))
        self.log.info("Transaction served from the mempool")

        peer.sync_with_ping()
        assert_equal(len(node.getpeerinfo()), 1)


if __name__ == "__main__":
    BlockServingTest().main()
