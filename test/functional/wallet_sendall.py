#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the modern `sendall` wallet RPC.

`sendall` sweeps all (or the explicitly selected) UTXOs of the wallet to one
or more recipients. The remaining value is split between the recipients that
were given without an explicit amount, no change output is created.
"""
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
)

# 0.0001 DFI/vB (10 sat/vB), above the minimum fee rate of the test setup.
FEE_RATE = 0.0001


class SendAllTest(DefiTestFramework):
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

        # Create more than one UTXO in the wallet of the first node.
        dest = self.nodes[1].getnewaddress()
        node.sendtoaddress(dest, 1)
        node.generate(1)
        self.sync_blocks()

        utxos = node.listunspent()
        assert len(utxos) > 1

        self.log.info("sendall with add_to_wallet=false does not touch the wallet")
        balance = node.getbalance()
        res = node.sendall(
            recipients=[dest],
            conf_target=None,
            estimate_mode="unset",
            fee_rate=FEE_RATE,
            options={"add_to_wallet": False},
        )
        assert_equal(res["complete"], True)
        assert "txid" in res
        assert "hex" in res
        assert "psbt" in res
        assert_equal(node.getbalance(), balance)
        assert res["txid"] not in node.getrawmempool()

        self.log.info("sendall requires at least one recipient without an amount")
        assert_raises_rpc_error(
            -8,
            "Must provide at least one address without a specified amount",
            node.sendall,
            [{dest: 1}],
            None,
            "unset",
            FEE_RATE,
        )

        self.log.info("sendall rejects send_max combined with explicit inputs")
        unspent = node.listunspent()[0]
        assert_raises_rpc_error(
            -8,
            "Cannot combine send_max with specific inputs",
            node.sendall,
            [dest],
            None,
            "unset",
            FEE_RATE,
            {
                "send_max": True,
                "inputs": [{"txid": unspent["txid"], "vout": unspent["vout"]}],
            },
        )

        self.log.info("sendall rejects a fee rate below the minimum fee rate")
        assert_raises_rpc_error(
            -8,
            "Fee rate",
            node.sendall,
            [dest],
            None,
            "unset",
            0.00000001,
        )

        self.log.info("sendall sweeps the whole wallet")
        res = node.sendall(recipients=[dest], fee_rate=FEE_RATE)
        assert_equal(res["complete"], True)
        txid = res["txid"]
        assert txid in node.getrawmempool()
        # No change output is left behind, only the fee is paid.
        assert node.getbalance() < balance
        assert node.getbalance() < 1

        self.log.info("sendall splits the value between two recipients")
        node.generate(101)
        self.sync_blocks()
        dest2 = self.nodes[1].getnewaddress()
        balance = node.getbalance()
        res = node.sendall(recipients=[dest, dest2], fee_rate=FEE_RATE)
        assert_equal(res["complete"], True)
        raw = node.decoderawtransaction(node.getrawtransaction(res["txid"]))
        assert_equal(len(raw["vout"]), 2)
        amounts = sorted([vout["value"] for vout in raw["vout"]])
        # Both recipients receive (almost) the same amount.
        assert abs(amounts[0] - amounts[1]) < Decimal("0.00000001")
        assert amounts[0] > 0
        assert node.getbalance() < balance


if __name__ == "__main__":
    SendAllTest().main()
