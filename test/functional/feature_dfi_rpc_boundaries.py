#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Verify DFI RPC boundary conditions, malformed inputs, and error-code contracts.

Covers deterministic negative-path and boundary validation across:
- rpc_vault.cpp (getvault, closevault, updatevault)
- rpc_loan.cpp (setcollateraltoken, setloantoken, getcollateraltoken, getloantoken)
- rpc_poolpair.cpp (getpoolpair, testpoolswap)
- rpc_proposals.cpp (createcfp, createvoc, getproposal)
- rpc_tokens.cpp (createtoken, updatetoken, gettoken)
- rpc_accounts.cpp (getaccount, accounttoaccount, utxostoaccount, listaccounts)
- rpc_oracles.cpp (appointoracle, getoracledata)
- rpc_customtx.cpp (getcustomtx)
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

NONEXISTENT_ID = "00" * 32


class DfiRpcBoundariesTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-txnotokens=0",
                "-amkheight=1",
                "-bayfrontheight=1",
                "-bayfrontgardensheight=1",
                "-clarkequayheight=1",
                "-dakotaheight=1",
                "-dakotacrescentheight=1",
                "-eunosheight=1",
                "-eunospayaheight=1",
                "-fortcanningheight=1",
                "-fortcanningmuseumheight=1",
                "-fortcanningparkheight=1",
                "-fortcanninghillheight=1",
                "-fortcanningroadheight=1",
                "-fortcanningcrunchheight=1",
                "-fortcanningspringheight=1",
                "-fortcanninggreatworldheight=1",
                "-fortcanningepilogueheight=1",
                "-grandcentralheight=1",
                "-grandcentralepilogueheight=1",
                "-metachainheight=105",
                "-df23height=105",
                "-df24height=105",
                "-subsidytest=1",
            ]
        ]

    def run_test(self):
        node = self.nodes[0]
        node.generate(110)

        owner = node.getnewaddress()
        # Seed a small account balance so we also have a valid CustomTx for getcustomtx
        txid_u2a = node.utxostoaccount({owner: "10@DFI"})
        node.generate(1)

        self.test_vault_rpc_boundaries(node, owner)
        self.test_loan_rpc_boundaries(node)
        self.test_poolpair_rpc_boundaries(node, owner)
        self.test_proposal_rpc_boundaries(node, owner)
        self.test_token_rpc_boundaries(node, owner)
        self.test_account_rpc_boundaries(node, owner)
        self.test_oracle_rpc_boundaries(node, owner)
        self.test_customtx_rpc_boundaries(node, txid_u2a)

        # Ensure no stray transactions were admitted to mempool by any rejected RPC call
        assert_equal(node.getrawmempool(), [])

    def test_vault_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_vault.cpp boundary conditions...")
        assert_raises_rpc_error(-20, "not found", node.getvault, NONEXISTENT_ID)
        assert_raises_rpc_error(-8, "must be of length 64", node.getvault, "not-a-hex-id")
        assert_raises_rpc_error(-5, "not found", node.closevault, NONEXISTENT_ID, owner)
        assert_raises_rpc_error(
            -5, "not found", node.updatevault, NONEXISTENT_ID, {"ownerAddress": owner}
        )

    def test_loan_rpc_boundaries(self, node):
        self.log.info("Testing rpc_loan.cpp boundary conditions...")
        assert_raises_rpc_error(
            -8,
            "does not exist",
            node.setcollateraltoken,
            {
                "token": "NONEXISTENT",
                "factor": 1.0,
                "fixedIntervalPriceId": "DFI/USD",
            },
        )
        assert_raises_rpc_error(
            -8,
            "price feed not in valid format",
            node.setloantoken,
            {
                "symbol": "DUSD",
                "fixedIntervalPriceId": "INVALIDFORMAT",
            },
        )
        assert_raises_rpc_error(
            -8,
            "token/currency contains empty string",
            node.setloantoken,
            {
                "symbol": "DUSD",
                "fixedIntervalPriceId": "/USD",
            },
        )
        assert_raises_rpc_error(-8, "does not exist", node.getloantoken, "NONEXISTENT")
        assert_raises_rpc_error(
            -8, "does not exist", node.getcollateraltoken, "NONEXISTENT"
        )

    def test_poolpair_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_poolpair.cpp boundary conditions...")
        assert_raises_rpc_error(-5, "Pool not found", node.getpoolpair, "999999")
        assert_raises_rpc_error(
            -8,
            "tokenFrom is empty",
            node.testpoolswap,
            {
                "from": owner,
                "tokenFrom": "",
                "amountFrom": 1,
                "to": owner,
                "tokenTo": "DFI",
            },
        )
        assert_raises_rpc_error(
            -8,
            "tokenTo is empty",
            node.testpoolswap,
            {
                "from": owner,
                "tokenFrom": "DFI",
                "amountFrom": 1,
                "to": owner,
                "tokenTo": "",
            },
        )
        assert_raises_rpc_error(
            -8,
            "TokenFrom was not found",
            node.testpoolswap,
            {
                "from": owner,
                "tokenFrom": "NO_SUCH_TOKEN",
                "amountFrom": 1,
                "to": owner,
                "tokenTo": "DFI",
            },
        )
        assert_raises_rpc_error(
            -8,
            "TokenTo was not found",
            node.testpoolswap,
            {
                "from": owner,
                "tokenFrom": "DFI",
                "amountFrom": 1,
                "to": owner,
                "tokenTo": "NO_SUCH_TOKEN",
            },
        )

    def test_proposal_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_proposals.cpp boundary conditions...")
        assert_raises_rpc_error(
            -8,
            "<title> is required",
            node.creategovcfp,
            {"context": "ctx", "amount": 1, "payoutAddress": owner},
        )
        assert_raises_rpc_error(
            -8,
            "<context> is required",
            node.creategovcfp,
            {"title": "t", "amount": 1, "payoutAddress": owner},
        )
        assert_raises_rpc_error(
            -8,
            "<amount> is required",
            node.creategovcfp,
            {"title": "t", "context": "ctx", "payoutAddress": owner},
        )
        assert_raises_rpc_error(
            -8,
            "<payoutAddress> is required",
            node.creategovcfp,
            {"title": "t", "context": "ctx", "amount": 1},
        )
        assert_raises_rpc_error(
            -8,
            "is of an unknown type",
            node.creategovcfp,
            {
                "title": "t",
                "context": "ctx",
                "amount": 1,
                "payoutAddress": "not-an-address",
            },
        )
        assert_raises_rpc_error(
            -8, "<title> is required", node.creategovvoc, {"context": "ctx"}
        )
        assert_raises_rpc_error(
            -8, "<context> is required", node.creategovvoc, {"title": "t"}
        )
        assert_raises_rpc_error(-8, "does not exist", node.getgovproposal, NONEXISTENT_ID)

    def test_token_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_tokens.cpp boundary conditions...")
        assert_raises_rpc_error(
            -32600,
            "Invalid token symbol",
            node.createtoken,
            {"symbol": "", "name": "Empty", "collateralAddress": owner},
        )
        assert_raises_rpc_error(
            -8, "Token name cannot be empty", node.updatetoken, "", {"name": "New"}
        )
        assert_raises_rpc_error(
            -8,
            "does not exist",
            node.updatetoken,
            "NONEXISTENT",
            {"name": "New"},
        )
        assert_raises_rpc_error(
            -8, "Can't alter DFI token", node.updatetoken, "DFI", {"name": "New"}
        )
        assert_raises_rpc_error(-5, "Token not found", node.gettoken, "NONEXISTENT")

    def test_account_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_accounts.cpp boundary conditions...")
        assert_raises_rpc_error(
            -5, "Invalid owner address", node.getaccount, "not-a-valid-address"
        )
        assert_raises_rpc_error(
            -5,
            "does not refer to any valid address",
            node.accounttoaccount,
            "not-a-valid-address",
            {owner: "1@DFI"},
        )
        assert_raises_rpc_error(
            -3,
            "Amount out of range",
            node.accounttoaccount,
            owner,
            {owner: "-1@DFI"},
        )
        assert_raises_rpc_error(
            -3, "Amount out of range", node.utxostoaccount, {owner: "-1@DFI"}
        )
        assert_raises_rpc_error(
            -8,
            "doesn't represent a correct hex",
            node.listaccounts,
            {"start": "not-valid-hex"},
        )

    def test_oracle_rpc_boundaries(self, node, owner):
        self.log.info("Testing rpc_oracles.cpp boundary conditions...")
        assert_raises_rpc_error(
            -25,
            "the weightage value is out of bounds",
            node.appointoracle,
            owner,
            [{"token": "DFI", "currency": "USD"}],
            -1,
        )
        assert_raises_rpc_error(
            -25,
            "the weightage value is out of bounds",
            node.appointoracle,
            owner,
            [{"token": "DFI", "currency": "USD"}],
            256,
        )
        assert_raises_rpc_error(
            -8,
            "currency is required field",
            node.appointoracle,
            owner,
            [{"token": "DFI"}],
            10,
        )
        assert_raises_rpc_error(
            -8,
            "token is required field",
            node.appointoracle,
            owner,
            [{"currency": "USD"}],
            10,
        )
        assert_raises_rpc_error(-20, "not found", node.getoracledata, NONEXISTENT_ID)

    def test_customtx_rpc_boundaries(self, node, txid_u2a):
        self.log.info("Testing rpc_customtx.cpp boundary conditions...")
        assert_raises_rpc_error(
            -5, "No such mempool or wallet transaction", node.getcustomtx, NONEXISTENT_ID
        )
        assert_raises_rpc_error(
            -8, "must be of length 64", node.getcustomtx, "bad-txid"
        )
        info = node.getcustomtx(txid_u2a)
        assert_equal(info["type"], "UtxosToAccount")
        assert_equal(info["valid"], True)


if __name__ == "__main__":
    DfiRpcBoundariesTest().main()
