#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the package aware mempool RPCs.

Covers testmempoolaccept with more than one transaction (package validation) and
the submitpackage RPC.
"""

from decimal import Decimal

from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_1,
    OP_RETURN,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# The DFI custom transaction marker, see src/consensus/tx_check.cpp.
DFI_TX_MARKER = b"DfTx"


def make_dummy_tx(num_inputs=1, seed=0x11):
    """A transaction that is not valid, it is only used for the context free package checks."""
    tx = CTransaction()
    tx.vin = [
        CTxIn(COutPoint(int(f"{seed:02x}" * 32, 16), i), CScript([OP_0]))
        for i in range(num_inputs)
    ]
    tx.vout = [CTxOut(1000, CScript([OP_1]))]
    return tx


def make_custom_tx():
    """A transaction that is recognized as a DFI custom transaction."""
    tx = CTransaction()
    tx.vin = [CTxIn(COutPoint(int("22" * 32, 16), 0), CScript([OP_0]))]
    tx.vout = [CTxOut(0, CScript([OP_RETURN, DFI_TX_MARKER + b"U"]))]
    return tx


def make_evm_tx():
    """A transaction that is recognized as an EVM transaction."""
    null_outpoint = COutPoint(int("00" * 32, 16), 0xFFFFFFFF)
    tx = CTransaction()
    tx.vin = [
        CTxIn(null_outpoint, CScript([OP_0])),
        CTxIn(null_outpoint, CScript([OP_0])),
    ]
    tx.vout = [CTxOut(0, CScript([OP_RETURN, DFI_TX_MARKER + b"9"]))]
    return tx


class MempoolPackageRpcTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txindex"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def create_parent(self):
        """Create a parent transaction, spending a coin of the wallet."""
        node = self.nodes[0]
        coin = node.listunspent()[0]
        amount = Decimal(coin["amount"])

        parent_raw = node.createrawtransaction(
            [{"txid": coin["txid"], "vout": coin["vout"]}],
            {node.getnewaddress(): amount - Decimal("0.001")},
        )
        parent_signed = node.signrawtransactionwithwallet(parent_raw)
        assert_equal(parent_signed["complete"], True)
        return parent_signed["hex"]

    def create_child(self, parent, fee="0.002"):
        """Create a child transaction, spending the output of the given parent transaction."""
        node = self.nodes[0]
        decoded = node.decoderawtransaction(parent)
        parent_txid = decoded["txid"]
        parent_vout = decoded["vout"][0]
        amount = Decimal(parent_vout["value"])

        child_raw = node.createrawtransaction(
            [{"txid": parent_txid, "vout": 0}],
            {node.getnewaddress(): amount - Decimal(fee)},
        )
        # The output of the parent is not in the wallet yet, it has to be given as a previous
        # transaction, so that the wallet is able to sign for it.
        child_signed = node.signrawtransactionwithwallet(
            child_raw,
            [
                {
                    "txid": parent_txid,
                    "vout": 0,
                    "scriptPubKey": parent_vout["scriptPubKey"]["hex"],
                    "amount": parent_vout["value"],
                }
            ],
        )
        assert_equal(child_signed["complete"], True)
        return child_signed["hex"]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create a parent transaction and a child transaction")
        parent = self.create_parent()
        child = self.create_child(parent)
        parent_txid = node.decoderawtransaction(parent)["txid"]
        child_txid = node.decoderawtransaction(child)["txid"]

        self.log.info("A single transaction keeps the output of the old implementation")
        result = node.testmempoolaccept([parent])
        assert_equal(len(result), 1)
        assert_equal(result[0]["txid"], parent_txid)
        assert_equal(result[0]["allowed"], True)
        assert "reject-reason" not in result[0]

        self.log.info("A package of a parent and its child is accepted")
        result = node.testmempoolaccept([parent, child])
        assert_equal(len(result), 2)
        assert_equal(result[0]["txid"], parent_txid)
        assert_equal(result[0]["allowed"], True)
        assert_equal(result[1]["txid"], child_txid)
        assert_equal(result[1]["allowed"], True)

        self.log.info("testmempoolaccept does not have any side effects")
        assert_equal(node.getrawmempool(), [])

        self.log.info("A package that is not sorted is rejected")
        result = node.testmempoolaccept([child, parent])
        assert_equal(len(result), 3)
        assert_equal(result[0]["allowed"], False)
        assert_equal(result[1]["allowed"], False)
        assert_equal(result[2]["package-error"], "package-not-sorted")

        self.log.info("A package with conflicting transactions is rejected")
        conflict = self.create_child(parent, fee="0.003")
        result = node.testmempoolaccept([parent, child, conflict])
        assert_equal(len(result), 4)
        assert_equal(result[0]["allowed"], False)
        assert_equal(result[1]["allowed"], False)
        assert_equal(result[2]["allowed"], False)
        assert_equal(result[3]["package-error"], "conflict-in-package")

        self.log.info("A package with too many transactions is rejected")
        too_many = [make_dummy_tx().serialize().hex() for _ in range(26)]
        assert_raises_rpc_error(
            -8,
            "Array must contain at most 25 raw transactions",
            node.testmempoolaccept,
            too_many,
        )

        self.log.info("A package that is too large is rejected")
        too_large = [
            make_dummy_tx(1000, seed=0x10 + i).serialize().hex() for i in range(3)
        ]
        result = node.testmempoolaccept(too_large)
        assert_equal(result[-1]["package-error"], "package-too-large")

        self.log.info("A DFI custom transaction is not allowed in a package")
        result = node.testmempoolaccept([parent, make_custom_tx().serialize().hex()])
        assert_equal(result[-1]["package-error"], "custom-tx-not-supported-in-package")
        assert_equal(result[0]["allowed"], False)
        assert_equal(result[1]["allowed"], False)

        self.log.info("An EVM transaction is not allowed in a package")
        result = node.testmempoolaccept([parent, make_evm_tx().serialize().hex()])
        assert_equal(result[-1]["package-error"], "evm-tx-not-supported-in-package")
        assert_equal(result[0]["allowed"], False)
        assert_equal(result[1]["allowed"], False)

        self.log.info("testmempoolaccept still does not have any side effects")
        assert_equal(node.getrawmempool(), [])

        self.log.info("submitpackage submits the whole package to the mempool")
        result = node.submitpackage([parent, child])
        assert_equal(len(result["tx-results"]), 2)
        assert_equal(result["tx-results"][0]["txid"], parent_txid)
        assert_equal(result["tx-results"][0]["allowed"], True)
        assert_equal(result["tx-results"][1]["txid"], child_txid)
        assert_equal(result["tx-results"][1]["allowed"], True)
        mempool = node.getrawmempool()
        assert parent_txid in mempool
        assert child_txid in mempool

        self.log.info("The transactions of the package can be mined")
        node.generate(1)
        assert_equal(node.getrawmempool(), [])

        self.log.info("A failing submitpackage leaves the mempool unchanged")
        parent2 = self.create_parent()
        invalid = make_dummy_tx().serialize().hex()
        assert_raises_rpc_error(-26, "failed:", node.submitpackage, [parent2, invalid])
        assert_equal(node.getrawmempool(), [])

        self.log.info("A package that is rejected as a whole is not submitted")
        child2 = self.create_child(parent2)
        assert_raises_rpc_error(
            -8,
            "package-not-sorted",
            node.submitpackage,
            [child2, parent2],
        )
        assert_equal(node.getrawmempool(), [])


if __name__ == "__main__":
    MempoolPackageRpcTest().main()
