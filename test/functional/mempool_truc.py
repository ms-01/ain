#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP 431 TRUC (version=3) transaction policy and 1P1C package relay."""

from decimal import Decimal
from io import BytesIO

from test_framework.messages import (
    COIN,
    CTransaction,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_RETURN,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    hex_str_to_bytes,
)

TRUC_VERSION = 3


class MempoolTrucTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txindex"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def make_signed_tx(self, inputs, outputs, version=TRUC_VERSION, extra_vout=None, prevtxs=None):
        """Create and sign a transaction with the requested nVersion."""
        node = self.nodes[0]
        raw_hex = node.createrawtransaction(inputs, outputs)
        tx = CTransaction()
        tx.deserialize(BytesIO(hex_str_to_bytes(raw_hex)))
        tx.nVersion = version
        if extra_vout:
            tx.vout.extend(extra_vout)
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

    def run_test(self):
        self.test_truc_size_and_topology()
        self.test_truc_version_isolation()
        self.test_truc_sibling_eviction()
        self.test_truc_1p1c_package_cpfp()

    def test_truc_size_and_topology(self):
        self.log.info("Testing TRUC v3 size limits and 1-parent-1-child topology...")
        node = self.nodes[0]
        utxo = self.get_utxos(1)[0]
        addr = node.getnewaddress()
        amount = Decimal(utxo["amount"])

        # 1. Oversized v3 tx (> 10,000 vB) is rejected with TRUC-violation.
        # Use standard P2WPKH outputs (each ~31 bytes) to exceed 10,000 vB without tripping multi-op-return.
        pad_spk = bytes.fromhex(node.getaddressinfo(addr)["scriptPubKey"])
        big_outputs = [CTxOut(10000, pad_spk) for _ in range(340)]
        big_hex, _ = self.make_signed_tx(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: amount - Decimal("0.05")},
            version=TRUC_VERSION,
            extra_vout=big_outputs,
        )
        res = node.testmempoolaccept([big_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "TRUC-violation")

        # 2. Normal v3 parent accepted.
        parent_hex, parent_tx = self.make_signed_tx(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: amount - Decimal("0.001")},
            version=TRUC_VERSION,
        )
        parent_txid = node.sendrawtransaction(parent_hex)
        assert parent_txid in node.getrawmempool()

        # 3. Oversized v3 child (> 1,000 vB) is rejected with TRUC-violation.
        child_big_outputs = [CTxOut(10000, pad_spk) for _ in range(35)]
        child_big_hex, _ = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 0}],
            {addr: amount - Decimal("0.01")},
            version=TRUC_VERSION,
            extra_vout=child_big_outputs,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 0,
                "scriptPubKey": parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amount - Decimal("0.001"),
            }],
        )
        res = node.testmempoolaccept([child_big_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "TRUC-violation")

        # 4. Normal v3 child accepted.
        child_hex, child_tx = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 0}],
            {addr: amount - Decimal("0.002")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 0,
                "scriptPubKey": parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amount - Decimal("0.001"),
            }],
        )
        child_txid = node.sendrawtransaction(child_hex)
        assert child_txid in node.getrawmempool()

        # 5. v3 grandchild (ancestor count = 3 > 2) rejected with TRUC-violation.
        grandchild_hex, _ = self.make_signed_tx(
            [{"txid": child_txid, "vout": 0}],
            {addr: amount - Decimal("0.003")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": child_txid,
                "vout": 0,
                "scriptPubKey": child_tx.vout[0].scriptPubKey.hex(),
                "amount": amount - Decimal("0.002"),
            }],
        )
        res = node.testmempoolaccept([grandchild_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "TRUC-violation")

        node.generate(1)

    def test_truc_version_isolation(self):
        self.log.info("Testing TRUC v3 / non-v3 version inheritance isolation...")
        node = self.nodes[0]
        utxo_a, utxo_b = self.get_utxos(2)
        addr = node.getnewaddress()

        # v3 parent in mempool -> v4 child must be rejected.
        amt_a = Decimal(utxo_a["amount"])
        v3_parent_hex, v3_parent_tx = self.make_signed_tx(
            [{"txid": utxo_a["txid"], "vout": utxo_a["vout"]}],
            {addr: amt_a - Decimal("0.001")},
            version=TRUC_VERSION,
        )
        v3_parent_txid = node.sendrawtransaction(v3_parent_hex)

        v4_child_hex, _ = self.make_signed_tx(
            [{"txid": v3_parent_txid, "vout": 0}],
            {addr: amt_a - Decimal("0.002")},
            version=4,
            prevtxs=[{
                "txid": v3_parent_txid,
                "vout": 0,
                "scriptPubKey": v3_parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amt_a - Decimal("0.001"),
            }],
        )
        res = node.testmempoolaccept([v4_child_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "TRUC-violation")

        # v4 parent in mempool -> v3 child must be rejected.
        amt_b = Decimal(utxo_b["amount"])
        v4_parent_hex, v4_parent_tx = self.make_signed_tx(
            [{"txid": utxo_b["txid"], "vout": utxo_b["vout"]}],
            {addr: amt_b - Decimal("0.001")},
            version=4,
        )
        v4_parent_txid = node.sendrawtransaction(v4_parent_hex)

        v3_child_hex, _ = self.make_signed_tx(
            [{"txid": v4_parent_txid, "vout": 0}],
            {addr: amt_b - Decimal("0.002")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": v4_parent_txid,
                "vout": 0,
                "scriptPubKey": v4_parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amt_b - Decimal("0.001"),
            }],
        )
        res = node.testmempoolaccept([v3_child_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "TRUC-violation")

        node.generate(1)

    def test_truc_sibling_eviction(self):
        self.log.info("Testing TRUC v3 sibling eviction...")
        node = self.nodes[0]
        utxo = self.get_utxos(1)[0]
        addr1 = node.getnewaddress()
        addr2 = node.getnewaddress()
        amt = Decimal(utxo["amount"])
        half = (amt - Decimal("0.001")) / 2

        parent_hex, parent_tx = self.make_signed_tx(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr1: half, addr2: half},
            version=TRUC_VERSION,
        )
        parent_txid = node.sendrawtransaction(parent_hex)

        # Child 1 spends output 0 with fee 0.001.
        child1_hex, _ = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 0}],
            {addr1: half - Decimal("0.001")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 0,
                "scriptPubKey": parent_tx.vout[0].scriptPubKey.hex(),
                "amount": Decimal(parent_tx.vout[0].nValue) / COIN,
            }],
        )
        child1_txid = node.sendrawtransaction(child1_hex)
        assert child1_txid in node.getrawmempool()

        # Sibling child 2 spends output 1 with lower fee (0.0005) -> rejected.
        child2_low_hex, _ = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 1}],
            {addr2: half - Decimal("0.0005")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 1,
                "scriptPubKey": parent_tx.vout[1].scriptPubKey.hex(),
                "amount": Decimal(parent_tx.vout[1].nValue) / COIN,
            }],
        )
        res = node.testmempoolaccept([child2_low_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "insufficient fee")

        # Sibling child 2 spends output 1 with higher fee (0.005) -> evicts child 1.
        child2_high_hex, _ = self.make_signed_tx(
            [{"txid": parent_txid, "vout": 1}],
            {addr2: half - Decimal("0.005")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": parent_txid,
                "vout": 1,
                "scriptPubKey": parent_tx.vout[1].scriptPubKey.hex(),
                "amount": Decimal(parent_tx.vout[1].nValue) / COIN,
            }],
        )
        child2_txid = node.sendrawtransaction(child2_high_hex)
        mempool = node.getrawmempool()
        assert parent_txid in mempool
        assert child2_txid in mempool
        assert child1_txid not in mempool

        node.generate(1)

    def test_truc_1p1c_package_cpfp(self):
        self.log.info("Testing 1P1C TRUC package CPFP via submitpackage...")
        node = self.nodes[0]
        utxo = self.get_utxos(1)[0]
        addr = node.getnewaddress()
        amt = Decimal(utxo["amount"])

        # 0-fee v3 parent: rejected on its own.
        parent_hex, parent_tx = self.make_signed_tx(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: amt},
            version=TRUC_VERSION,
        )
        res = node.testmempoolaccept([parent_hex])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "min relay fee not met")

        # v4 child spending 0-fee v3 parent in a package -> rejected with TRUC-violation.
        v4_child_hex, _ = self.make_signed_tx(
            [{"txid": parent_tx.hash, "vout": 0}],
            {addr: amt - Decimal("0.002")},
            version=4,
            prevtxs=[{
                "txid": parent_tx.hash,
                "vout": 0,
                "scriptPubKey": parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amt,
            }],
        )
        assert_raises_rpc_error(-26, "TRUC-violation", node.submitpackage, [parent_hex, v4_child_hex])

        # High-fee v3 child funding the 0-fee v3 parent via submitpackage -> accepted!
        v3_child_hex, v3_child_tx = self.make_signed_tx(
            [{"txid": parent_tx.hash, "vout": 0}],
            {addr: amt - Decimal("0.002")},
            version=TRUC_VERSION,
            prevtxs=[{
                "txid": parent_tx.hash,
                "vout": 0,
                "scriptPubKey": parent_tx.vout[0].scriptPubKey.hex(),
                "amount": amt,
            }],
        )
        pkg_res = node.submitpackage([parent_hex, v3_child_hex])
        assert_equal(len(pkg_res["tx-results"]), 2)
        assert_equal(pkg_res["tx-results"][0]["allowed"], True)
        assert_equal(pkg_res["tx-results"][1]["allowed"], True)
        mempool = node.getrawmempool()
        assert parent_tx.hash in mempool
        assert v3_child_tx.hash in mempool

        node.generate(1)


if __name__ == "__main__":
    MempoolTrucTest().main()
