#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the include_unsafe option of the funding RPCs.

By default the funding RPCs only spend safe coins, that is confirmed coins
and unconfirmed coins that were received by the wallet itself. With
`include_unsafe` the wallet is also allowed to spend unsafe coins, for
example an unconfirmed output that was received from another wallet.
"""
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
)


class IncludeUnsafeTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-mintxfee=0.00002"],
            ["-mintxfee=0.00002"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        connect_nodes_bi(self.nodes, 0, 1)

    def run_test(self):
        funder_node, wallet_node = self.nodes

        self.log.info("Mining blocks...")
        funder_node.generate(101)
        self.sync_blocks()

        # The wallet of the second node receives an unconfirmed output from the
        # first node. The output is not safe to spend, because the transaction
        # that created it was not created by the wallet itself.
        receive_address = wallet_node.getnewaddress()
        funder_node.sendtoaddress(receive_address, 1)
        self.sync_mempools()

        unspent = wallet_node.listunspent(0)
        assert_equal(len(unspent), 1)
        assert_equal(unspent[0]["address"], receive_address)
        assert_equal(unspent[0]["safe"], False)
        assert_equal(unspent[0]["confirmations"], 0)

        dest_address = wallet_node.getnewaddress()
        rawtx = wallet_node.createrawtransaction([], {dest_address: 0.5})

        # Without include_unsafe the unsafe output is not selected.
        assert_raises_rpc_error(
            -4, "Insufficient funds", wallet_node.fundrawtransaction, rawtx
        )
        assert_raises_rpc_error(
            -4,
            "Insufficient funds",
            wallet_node.fundrawtransaction,
            rawtx,
            {"include_unsafe": False},
        )
        assert_raises_rpc_error(
            -4,
            "Insufficient funds",
            wallet_node.walletcreatefundedpsbt,
            [],
            {dest_address: 0.5},
        )

        # With include_unsafe the unsafe output is selected.
        funded = wallet_node.fundrawtransaction(rawtx, {"include_unsafe": True})
        assert funded["fee"] > 0
        funded_tx = wallet_node.decoderawtransaction(funded["hex"])
        assert_equal(len(funded_tx["vin"]), 1)
        assert_equal(funded_tx["vin"][0]["txid"], unspent[0]["txid"])

        psbt = wallet_node.walletcreatefundedpsbt(
            [], {dest_address: 0.5}, 0, {"include_unsafe": True}
        )["psbt"]
        decoded = wallet_node.decodepsbt(psbt)
        assert_equal(len(decoded["tx"]["vin"]), 1)
        assert_equal(decoded["tx"]["vin"][0]["txid"], unspent[0]["txid"])

        # Once the output is confirmed it is safe and selected by default.
        funder_node.generate(1)
        self.sync_blocks()
        unspent = wallet_node.listunspent()
        assert_equal(len(unspent), 1)
        assert_equal(unspent[0]["safe"], True)
        funded = wallet_node.fundrawtransaction(rawtx)
        assert funded["fee"] > 0

        self.log.info("Success")


if __name__ == "__main__":
    IncludeUnsafeTest().main()
