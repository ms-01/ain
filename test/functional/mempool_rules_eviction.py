#!/usr/bin/env python3
# Copyright (c) 2026 The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test mempool eviction, ancestor/descendant limits, and DF25 height-transition re-validation."""

from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    create_confirmed_utxos,
    create_lots_of_big_transactions,
    gen_return_txouts,
)


class MempoolRulesEvictionTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.df25_height = 220
        self.extra_args = [
            [
                "-acceptnonstdtxn=1",
                "-maxmempool=5",
                "-limitancestorcount=5",
                "-limitdescendantcount=5",
                "-spendzeroconfchange=0",
                f"-df25height={self.df25_height}",
            ]
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def test_ancestor_limit(self, node, utxo):
        self.log.info("Testing ancestor/descendant chain limit enforcement")
        addr = node.getnewaddress()
        fee = Decimal("0.0001")
        amount = utxo["amount"]
        prev_txid = utxo["txid"]
        prev_vout = utxo["vout"]

        chain_txids = []
        # limitancestorcount=5 allows 5 unconfirmed transactions in a chain
        for _ in range(5):
            amount -= fee
            raw = node.createrawtransaction(
                [{"txid": prev_txid, "vout": prev_vout}],
                {addr: amount},
            )
            signed = node.signrawtransactionwithwallet(raw)
            txid = node.sendrawtransaction(signed["hex"])
            chain_txids.append(txid)
            prev_txid = txid
            prev_vout = 0

        # The 6th chained transaction must be rejected for exceeding the mempool chain limit
        amount -= fee
        raw_overflow = node.createrawtransaction(
            [{"txid": prev_txid, "vout": prev_vout}],
            {addr: amount},
        )
        signed_overflow = node.signrawtransactionwithwallet(raw_overflow)
        assert_raises_rpc_error(
            -26,
            "too-long-mempool-chain",
            node.sendrawtransaction,
            signed_overflow["hex"],
        )

        # Mining a block confirms the chain and clears the mempool
        node.generate(1)
        assert_equal(len(node.getrawmempool()), 0)

    def test_height_transition_revalidation(self, node):
        self.log.info("Testing mempool re-validation across DF25Height boundary")
        current_height = node.getblockcount()
        assert_greater_than(self.df25_height, current_height + 2)

        # Advance to DF25Height - 2
        node.generate(self.df25_height - 2 - current_height)
        assert_equal(node.getblockcount(), self.df25_height - 2)

        # Submit a valid transaction before DF25Height
        addr = node.getnewaddress()
        txid_pre = node.sendtoaddress(addr, Decimal("1.0"))
        assert txid_pre in node.getrawmempool()

        # Mine 1 block to reach DF25Height - 1 (prospective height = DF25Height triggers
        # MempoolRulesChangedBetween() in CTxMemPool::removeForBlock)
        node.generate(1)
        assert_equal(node.getblockcount(), self.df25_height - 1)
        assert_equal(len(node.getrawmempool()), 0)

        # Submit a transaction at DF25Height - 1 and cross into DF25Height
        txid_cross = node.sendtoaddress(addr, Decimal("1.0"))
        assert txid_cross in node.getrawmempool()
        node.generate(1)
        assert_equal(node.getblockcount(), self.df25_height)
        assert_equal(len(node.getrawmempool()), 0)

        # Verify transaction acceptance continues cleanly after DF25Height
        txid_post = node.sendtoaddress(addr, Decimal("1.0"))
        assert txid_post in node.getrawmempool()
        node.generate(1)
        assert_equal(len(node.getrawmempool()), 0)

    def test_size_eviction(self, node):
        self.log.info("Testing mempool size eviction and mempoolminfee enforcement")
        txouts = gen_return_txouts()
        relayfee = node.getnetworkinfo()["relayfee"]

        utxos = create_confirmed_utxos(relayfee, node, 92)

        # First run ancestor limit check using one confirmed UTXO
        self.test_ancestor_limit(node, utxos.pop())

        # Create a low-fee transaction that should be evicted when mempool fills
        us0 = utxos.pop()
        raw = node.createrawtransaction(
            [{"txid": us0["txid"], "vout": us0["vout"]}],
            {node.getnewaddress(): Decimal("0.0001")},
        )
        node.settxfee(relayfee)
        funded = node.fundrawtransaction(raw)
        node.settxfee(0)
        signed = node.signrawtransactionwithwallet(funded["hex"])
        low_fee_txid = node.sendrawtransaction(signed["hex"])
        assert low_fee_txid in node.getrawmempool()

        base_fee = relayfee * 100
        for i in range(3):
            create_lots_of_big_transactions(
                node,
                txouts,
                utxos[30 * i : 30 * i + 30],
                30,
                (i + 1) * base_fee,
            )

        assert low_fee_txid not in node.getrawmempool()
        assert_greater_than(
            node.getmempoolinfo()["mempoolminfee"],
            node.getmempoolinfo()["minrelaytxfee"],
        )

        # Clear mempool before height transition test
        node.generate(10)

    def run_test(self):
        node = self.nodes[0]
        self.test_size_eviction(node)
        self.test_height_transition_revalidation(node)


if __name__ == "__main__":
    MempoolRulesEvictionTest().main()
