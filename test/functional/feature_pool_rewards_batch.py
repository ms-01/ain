#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the batched pool reward calculation.

The pool rewards of an account which has not been touched for a long time are
credited with a single batch instead of one credit per height. The credited
amount has to be exactly the same as the sum of the single per-height rewards
which are reported by the account history.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal

from decimal import Decimal


class PoolRewardsBatchTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=50",
                "-bayfrontgardensheight=50",
                "-clarkequayheight=50",
                "-grandcentralheight=50",
            ]
        ]

    def run_test(self):
        node = self.nodes[0]

        # leave the initial block download
        node.generate(102)

        # the tokens of the pool
        collateral_a = node.getnewaddress("", "legacy")
        collateral_b = node.getnewaddress("", "legacy")
        node.createtoken(
            {"symbol": "GOLD", "name": "gold", "collateralAddress": collateral_a}
        )
        node.generate(1)
        node.createtoken(
            {"symbol": "SILVER", "name": "silver", "collateralAddress": collateral_b}
        )
        node.generate(1)

        tokens = node.listtokens()
        token_a = [idx for idx, token in tokens.items() if token["symbol"] == "GOLD"][0]
        token_b = [idx for idx, token in tokens.items() if token["symbol"] == "SILVER"][
            0
        ]

        node.minttokens(["100000@" + token_a])
        node.minttokens(["100000@" + token_b])
        node.generate(1)

        pool_owner = node.getnewaddress("", "legacy")
        node.createpoolpair(
            {
                "tokenA": "GOLD#" + token_a,
                "tokenB": "SILVER#" + token_b,
                "commission": 0.001,
                "status": True,
                "ownerAddress": pool_owner,
                "pairSymbol": "SILVGOLD",
            }
        )
        node.generate(1)
        pool_id = list(node.getpoolpair("SILVGOLD").keys())[0]

        # the liquidity provider which becomes stale later on
        provider = node.getnewaddress("", "legacy")
        node.addpoolliquidity({"*": ["1000@" + token_a, "1000@" + token_b]}, provider)
        node.generate(1)

        # the daily reward of the liquidity providers and the share of the pool
        node.setgov({"LP_DAILY_DFI_REWARD": 1})
        node.generate(1)
        node.setgov({"LP_SPLITS": {pool_id: 1}})
        node.generate(1)

        # the reward history of a pool starts at the grand central height
        while node.getblockcount() < 160:
            node.generate(1)

        # an account which sends the rewards to the provider
        sender = node.getnewaddress("", "legacy")
        node.utxostoaccount({sender: "100@DFI"})
        node.generate(1)

        def dfi_balance(address):
            for entry in node.getaccount(address):
                amount, token = entry.split("@")
                if token in ("0", "DFI"):
                    return Decimal(amount)
            return Decimal("0")

        def touch(address):
            """touch the account to settle its pool rewards"""
            node.accounttoaccount(sender, {address: "0.00000001@DFI"})
            node.generate(1)

        # settle the rewards of the provider once
        touch(provider)
        balance = dfi_balance(provider)

        # touch the provider in every single block to measure the reward of a
        # single height
        for _ in range(5):
            touch(provider)
        touched_balance = dfi_balance(provider)
        # five transfers of one satoshi and five heights of pool rewards
        reward_per_height = (touched_balance - balance - Decimal("0.00000005")) / 5
        assert reward_per_height > 0

        # let the provider become stale
        stale_blocks = 50
        height_before = node.getblockcount()
        node.generate(stale_blocks)

        # the rewards of all the missed heights are credited at once
        touch(provider)
        height_after = node.getblockcount()

        # the number of credited heights: the heights between the last touch and
        # the current one
        credited_heights = height_after - height_before
        assert credited_heights == stale_blocks + 1
        expected = reward_per_height * credited_heights + Decimal("0.00000001")
        assert_equal(dfi_balance(provider) - touched_balance, expected)

        # the account history reports the very same rewards, one entry per height
        history = node.listaccounthistory(provider, {"depth": 200})
        rewards = [
            entry
            for entry in history
            if entry.get("type") == "Rewards" and entry.get("rewardType") == "Coinbase"
        ]
        assert len(rewards) >= credited_heights
        # the account history reports the reward of every single height, the
        # reward of a height has to be exactly the same as the reward which is
        # credited by the batched calculation
        for entry in rewards:
            for amount in entry["amounts"]:
                value, token = amount.split("@")
                if token in ("0", "DFI"):
                    assert Decimal(value) == reward_per_height


if __name__ == "__main__":
    PoolRewardsBatchTest().main()
