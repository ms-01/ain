#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test opportunistic 1-parent-1-child (1P1C) package relay over P2P."""

from decimal import Decimal
from io import BytesIO
import time

from test_framework.messages import (
    CTransaction,
    msg_tx,
)
from test_framework.mininode import P2PDataStore
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    hex_str_to_bytes,
    wait_until,
)

TRUC_VERSION = 3


class P2POpportunistic1P1CTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            "-debug=mempool",
            "-debug=mempoolrej",
            "-minrelaytxfee=0.0001",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def make_signed_tx(self, inputs, outputs, version=2, prevtxs=None):
        """Create and sign a transaction with the requested nVersion."""
        node = self.nodes[0]
        raw_hex = node.createrawtransaction(inputs, outputs)
        tx = CTransaction()
        tx.deserialize(BytesIO(hex_str_to_bytes(raw_hex)))
        tx.nVersion = version
        tx.rehash()
        signed = node.signrawtransactionwithwallet(tx.serialize().hex(), prevtxs)
        assert signed["complete"]
        signed_tx = CTransaction()
        signed_tx.deserialize(BytesIO(hex_str_to_bytes(signed["hex"])))
        signed_tx.rehash()
        return signed["hex"], signed_tx

    def get_utxos(self, count):
        node = self.nodes[0]
        utxos = [u for u in node.listunspent() if Decimal(u["amount"]) >= Decimal("1.0")]
        while len(utxos) < count:
            node.generate(1)
            utxos = [u for u in node.listunspent() if Decimal(u["amount"]) >= Decimal("1.0")]
        return utxos[:count]

    def make_1p1c(self, utxo, parent_fee=Decimal("0"), child_fee=Decimal("0.002"), version=2):
        """Build a signed (parent, child) pair spending utxo."""
        node = self.nodes[0]
        addr = node.getnewaddress("", "legacy")
        spk = node.getaddressinfo(addr)["scriptPubKey"]
        amount = Decimal(utxo["amount"])
        parent_out = amount - parent_fee
        _, parent_tx = self.make_signed_tx(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: parent_out},
            version=version,
        )
        parent_txid = parent_tx.hash
        child_out = parent_out - child_fee
        _, child_tx = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 0}],
            {addr: child_out},
            version=version,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 0,
                "scriptPubKey": spk,
                "amount": parent_out,
            }],
        )
        return parent_tx, child_tx

    def run_test(self):
        node = self.nodes[0]
        # Ensure we have plenty of mature UTXOs.
        utxos = self.get_utxos(8)

        self.test_parent_first_then_child(utxos[0])
        self.test_child_first_then_parent(utxos[1])
        self.test_rejected_package_cached(utxos[2])
        self.test_two_reconsiderable_parents_dropped(utxos[3], utxos[4])
        self.test_same_peer_isolation(utxos[5])
        self.test_truc_v3_1p1c_p2p(utxos[6])

    def test_parent_first_then_child(self, utxo):
        self.log.info("1. Parent first (0-fee -> reconsiderable), then high-fee child -> 1P1C accepted")
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        parent_tx, child_tx = self.make_1p1c(utxo, parent_fee=Decimal("0"), child_fee=Decimal("0.002"))

        # Send 0-fee parent first; it should be rejected individually (min relay fee not met).
        peer.send_and_ping(msg_tx(parent_tx))
        assert parent_tx.hash not in node.getrawmempool()

        # Now send high-fee child as orphan; node should re-request parent from peer.
        peer.tx_store[parent_tx.sha256] = parent_tx
        peer.send_and_ping(msg_tx(child_tx))
        # Advance mocktime past TXID_RELAY_DELAY / NONPREF_PEER_TX_DELAY so GETDATA is sent.
        node.setmocktime(int(time.time()) + 600)

        wait_until(
            lambda: parent_tx.hash in node.getrawmempool() and child_tx.hash in node.getrawmempool(),
            timeout=10,
        )
        node.disconnect_p2ps()

    def test_child_first_then_parent(self, utxo):
        self.log.info("2. Child first (orphan), then 0-fee parent -> 1P1C accepted immediately")
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        parent_tx, child_tx = self.make_1p1c(utxo, parent_fee=Decimal("0"), child_fee=Decimal("0.002"))

        # Send child first (becomes orphan).
        peer.send_and_ping(msg_tx(child_tx))
        assert child_tx.hash not in node.getrawmempool()

        # Send 0-fee parent; node pairs it with the waiting orphan child and accepts both.
        peer.send_and_ping(msg_tx(parent_tx))
        mempool = node.getrawmempool()
        assert parent_tx.hash in mempool
        assert child_tx.hash in mempool
        node.disconnect_p2ps()

    def test_rejected_package_cached(self, utxo):
        self.log.info("3. Low-fee 1P1C package rejected atomically and cached by package hash")
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        # Both parent and child pay 0 fee -> combined feerate still below minrelaytxfee.
        parent_tx, child_tx = self.make_1p1c(utxo, parent_fee=Decimal("0"), child_fee=Decimal("0"))

        peer.send_and_ping(msg_tx(child_tx))
        peer.send_and_ping(msg_tx(parent_tx))

        mempool = node.getrawmempool()
        assert parent_tx.hash not in mempool
        assert child_tx.hash not in mempool

        # Resending the parent does not accept the rejected pair and does not disconnect the peer.
        peer.send_and_ping(msg_tx(parent_tx))
        assert parent_tx.hash not in node.getrawmempool()
        assert child_tx.hash not in node.getrawmempool()
        assert_equal(len(node.getpeerinfo()), 1)
        node.disconnect_p2ps()

    def test_two_reconsiderable_parents_dropped(self, utxo1, utxo2):
        self.log.info("4. Orphan with two reconsiderable parents is dropped")
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        addr = node.getnewaddress("", "legacy")
        spk = node.getaddressinfo(addr)["scriptPubKey"]

        # Create two 0-fee parents (non-segwit or segwit; both rejected with TX_RECONSIDERABLE).
        _, p1 = self.make_signed_tx(
            [{"txid": utxo1["txid"], "vout": utxo1["vout"]}],
            {addr: Decimal(utxo1["amount"])},
        )
        _, p2 = self.make_signed_tx(
            [{"txid": utxo2["txid"], "vout": utxo2["vout"]}],
            {addr: Decimal(utxo2["amount"])},
        )
        peer.send_and_ping(msg_tx(p1))
        peer.send_and_ping(msg_tx(p2))
        assert p1.hash not in node.getrawmempool()
        assert p2.hash not in node.getrawmempool()

        # High-fee child spending both parents cannot be rescued by 1P1C.
        total = Decimal(utxo1["amount"]) + Decimal(utxo2["amount"]) - Decimal("0.005")
        _, child = self.make_signed_tx(
            [{"txid": p1.hash, "vout": 0}, {"txid": p2.hash, "vout": 0}],
            {addr: total},
            prevtxs=[
                {"txid": p1.hash, "vout": 0, "scriptPubKey": spk, "amount": Decimal(utxo1["amount"])},
                {"txid": p2.hash, "vout": 0, "scriptPubKey": spk, "amount": Decimal(utxo2["amount"])},
            ],
        )
        peer.send_and_ping(msg_tx(child))
        mempool = node.getrawmempool()
        assert p1.hash not in mempool
        assert p2.hash not in mempool
        assert child.hash not in mempool
        node.disconnect_p2ps()

    def test_same_peer_isolation(self, utxo):
        self.log.info("5. Same-peer child pairing prevents cross-peer interference")
        node = self.nodes[0]
        peer_bad = node.add_p2p_connection(P2PDataStore())
        peer_good = node.add_p2p_connection(P2PDataStore())

        parent_tx, good_child = self.make_1p1c(utxo, parent_fee=Decimal("0"), child_fee=Decimal("0.002"))
        # Build a 0-fee competing child from peer_bad spending the same parent.
        addr = node.getnewaddress("", "legacy")
        spk = node.getaddressinfo(addr)["scriptPubKey"]
        parent_out = Decimal(utxo["amount"])
        _, bad_child = self.make_signed_tx(
            [{"txid": parent_tx.hash, "vout": 0}],
            {addr: parent_out},
            prevtxs=[{
                "txid": parent_tx.hash,
                "vout": 0,
                "scriptPubKey": spk,
                "amount": parent_out,
            }],
        )

        # peer_bad sends its 0-fee child first, then peer_good sends its high-fee child and parent.
        peer_bad.send_and_ping(msg_tx(bad_child))
        peer_good.send_and_ping(msg_tx(good_child))
        peer_good.send_and_ping(msg_tx(parent_tx))

        mempool = node.getrawmempool()
        assert parent_tx.hash in mempool
        assert good_child.hash in mempool
        assert bad_child.hash not in mempool
        node.disconnect_p2ps()

    def test_truc_v3_1p1c_p2p(self, utxo):
        self.log.info("6. TRUC v3 1P1C package relayed over P2P")
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        parent_tx, child_tx = self.make_1p1c(
            utxo,
            parent_fee=Decimal("0"),
            child_fee=Decimal("0.002"),
            version=TRUC_VERSION,
        )

        peer.send_and_ping(msg_tx(parent_tx))
        assert parent_tx.hash not in node.getrawmempool()

        peer.tx_store[parent_tx.sha256] = parent_tx
        peer.send_and_ping(msg_tx(child_tx))
        node.setmocktime(int(time.time()) + 1200)

        wait_until(
            lambda: parent_tx.hash in node.getrawmempool() and child_tx.hash in node.getrawmempool(),
            timeout=10,
        )
        node.disconnect_p2ps()


if __name__ == "__main__":
    P2POpportunistic1P1CTest().main()
