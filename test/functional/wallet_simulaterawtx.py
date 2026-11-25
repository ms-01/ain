#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the modern `simulaterawtransaction` wallet RPC.

`simulaterawtransaction` calculates the balance change of one or more raw
transactions without touching the wallet. Only the DFI UTXO balance is taken
into account, token amounts are excluded.
"""
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
)


class SimulateRawTxTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-mintxfee=0.00001"],
            ["-mintxfee=0.00001"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        connect_nodes_bi(self.nodes, 0, 1)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mining blocks...")
        node.generate(101)
        self.sync_blocks()

        balance = node.getbalance()
        assert balance > 0

        dest = self.nodes[1].getnewaddress()
        unspent = node.listunspent()[0]

        self.log.info("simulaterawtransaction of a spending transaction")
        rawtx = node.createrawtransaction(
            [{"txid": unspent["txid"], "vout": unspent["vout"]}], {dest: 1}
        )
        res = node.simulaterawtransaction([rawtx])
        # The input is ours (negative), the output belongs to another wallet.
        assert_equal(res["balance_change"], -unspent["amount"])
        # The wallet was not modified.
        assert_equal(node.getbalance(), balance)

        self.log.info("simulaterawtransaction of a receiving transaction")
        own_address = node.getnewaddress()
        rawtx = node.createrawtransaction([], {own_address: 1})
        res = node.simulaterawtransaction([rawtx])
        assert_equal(res["balance_change"], Decimal("1.00000000"))
        assert_equal(node.getbalance(), balance)

        self.log.info("simulaterawtransaction of chained transactions")
        rawtx1 = node.createrawtransaction(
            [{"txid": unspent["txid"], "vout": unspent["vout"]}], {own_address: 1}
        )
        decoded = node.decoderawtransaction(rawtx1)
        # Spend the output of the first transaction in a second transaction.
        fake_txid = decoded["txid"]
        rawtx2 = node.createrawtransaction([{"txid": fake_txid, "vout": 0}], {dest: 1})
        res = node.simulaterawtransaction([rawtx1, rawtx2])
        # The first transaction spends our coin and receives 1, the second
        # transaction spends the received coin again, so the net effect is the
        # loss of the coinbase output.
        assert_equal(res["balance_change"], -unspent["amount"])

        self.log.info("simulaterawtransaction rejects double spends")
        assert_raises_rpc_error(
            -8,
            "spending the same output more than once",
            node.simulaterawtransaction,
            [rawtx1, rawtx1],
        )

        self.log.info("simulaterawtransaction rejects unknown inputs")
        rawtx = node.createrawtransaction([{"txid": "00" * 32, "vout": 0}], {dest: 1})
        assert_raises_rpc_error(
            -8,
            "missing or have been spent already",
            node.simulaterawtransaction,
            [rawtx],
        )


if __name__ == "__main__":
    SimulateRawTxTest().main()
