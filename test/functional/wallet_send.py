#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the modern `send` wallet RPC.

The `send` RPC is a unified create/sign/broadcast RPC that supports explicit
fee rates, explicit inputs, PSBT output and the `add_to_wallet` switch. Only
the DFI UTXO amount is supported, token amounts are rejected.
"""
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
)

# 0.0001 DFI/vB (10 sat/vB), above the minimum fee rate of the test setup.
FEE_RATE = 0.0001


class SendTest(DefiTestFramework):
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

        self.log.info("send with add_to_wallet=false does not touch the wallet")
        res = node.send(
            outputs={dest: 1},
            conf_target=None,
            estimate_mode="unset",
            fee_rate=FEE_RATE,
            options={"add_to_wallet": False},
        )
        assert_equal(res["complete"], True)
        assert "txid" in res
        assert "hex" in res
        assert "psbt" in res
        # The transaction was neither committed to the wallet nor broadcast.
        assert_equal(node.getbalance(), balance)
        assert_equal(res["txid"] not in node.getrawmempool(), True)
        assert_raises_rpc_error(
            -5, "Invalid or non-wallet transaction id", node.gettransaction, res["txid"]
        )

        self.log.info("send broadcasts and commits the transaction")
        res = node.send(outputs={dest: 1}, fee_rate=FEE_RATE)
        assert_equal(res["complete"], True)
        assert "txid" in res
        assert "hex" not in res
        txid = res["txid"]
        assert txid in node.getrawmempool()
        assert node.getbalance() < balance

        self.log.info("send with psbt=true returns a base64 encoded PSBT")
        res = node.send(outputs={dest: 1}, fee_rate=FEE_RATE, options={"psbt": True})
        assert_equal(res["complete"], True)
        assert "psbt" in res
        assert "txid" in res
        # A PSBT is not committed to the wallet.
        assert res["txid"] not in node.getrawmempool()

        self.log.info("send with explicit inputs")
        node.generate(1)
        self.sync_blocks()
        utxos = node.listunspent()
        assert len(utxos) > 0
        unspent = utxos[0]
        res = node.send(
            outputs={dest: 1},
            fee_rate=FEE_RATE,
            options={
                "add_to_wallet": False,
                "inputs": [{"txid": unspent["txid"], "vout": unspent["vout"]}],
            },
        )
        assert_equal(res["complete"], True)
        assert "hex" in res
        raw = node.decoderawtransaction(res["hex"])
        assert_equal(len(raw["vin"]), 1)
        assert_equal(raw["vin"][0]["txid"], unspent["txid"])
        assert_equal(raw["vin"][0]["vout"], unspent["vout"])

        self.log.info("send with an explicit locktime")
        res = node.send(
            outputs={dest: 1},
            fee_rate=FEE_RATE,
            options={"add_to_wallet": False, "locktime": 500},
        )
        assert_equal(res["complete"], True)
        raw = node.decoderawtransaction(res["hex"])
        assert_equal(raw["locktime"], 500)

        self.log.info("send with a data output")
        res = node.send(
            outputs={dest: 1, "data": "00112233"},
            fee_rate=FEE_RATE,
            options={"add_to_wallet": False},
        )
        assert_equal(res["complete"], True)
        raw = node.decoderawtransaction(res["hex"])
        asm = [vout["scriptPubKey"]["asm"] for vout in raw["vout"]]
        assert any(a.startswith("OP_RETURN") for a in asm)

        self.log.info("send rejects token amounts")
        assert_raises_rpc_error(
            -8,
            "Token amounts are not supported by this RPC",
            node.send,
            {dest: "1@2"},
            None,
            "unset",
            FEE_RATE,
        )

        self.log.info("send rejects invalid addresses")
        assert_raises_rpc_error(
            -5,
            "Invalid address",
            node.send,
            {"not-an-address": 1},
            None,
            "unset",
            FEE_RATE,
        )

        self.log.info("send rejects duplicated addresses")
        assert_raises_rpc_error(
            -8,
            "duplicated address",
            node.send,
            [{dest: 1}, {dest: 2}],
            None,
            "unset",
            FEE_RATE,
        )

        self.log.info("send rejects conflicting fee arguments")
        assert_raises_rpc_error(
            -8,
            "Cannot specify both conf_target and fee_rate",
            node.send,
            {dest: 1},
            6,
            "unset",
            FEE_RATE,
        )

        self.log.info("send rejects the outdated option names")
        assert_raises_rpc_error(
            -8,
            "Use fee_rate instead of feeRate",
            node.send,
            {dest: 1},
            None,
            "unset",
            None,
            {"feeRate": FEE_RATE},
        )


if __name__ == "__main__":
    SendTest().main()
