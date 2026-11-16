#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Regression test for the loan and collateral amounts of a vault.

The per token loan and collateral amounts of a vault are converted into the
vault currency in CCustomCSView::PopulateLoansData() / PopulateCollateralData()
(src/dfi/masternodes.cpp). The converted amounts are consumed by

  * the DFI/DUSD collateral percentage check of a take loan
    (CCustomTxVisitor::CollateralPctCheck), and
  * the split of the loan into auction batches (CollectAuctionBatches).

If a converted amount is silently collapsed to a boolean (1 satoshi), taking a
loan fails ("At least 50% of the minimum required collateral must be in DFI and
DUSD") and the auction batches are left without any collateral.

The test uses fixed oracle prices (100 USD per DFI and per loan token) and a
vault with a known collateral, so the regression is detected immediately.
"""

import calendar
import time
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal


class VaultAssetAmountsTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-txnotokens=0",
                "-amkheight=1",
                "-bayfrontheight=1",
                "-bayfrontgardensheight=1",
                "-eunosheight=1",
                "-fortcanningheight=1",
                "-fortcanninghillheight=1",
            ]
        ]

    def set_oracle_prices(self, dfi_price, tsla_price):
        """Feed the oracle with the given prices (fixed prices, no live feed)."""
        prices = [
            {"currency": "USD", "tokenAmount": dfi_price + "@DFI"},
            {"currency": "USD", "tokenAmount": tsla_price + "@TSLA"},
        ]
        timestamp = calendar.timegm(time.gmtime())
        self.nodes[0].setoracledata(self.oracle_id, timestamp, prices)
        self.nodes[0].generate(1)

    def assert_amount_close(self, amount, expected, tolerance=Decimal("0.05")):
        """Compare an amount (value or <value>@<symbol>) with a tolerance."""
        if isinstance(amount, str):
            amount = amount.split("@")[0]
        value = Decimal(amount)
        assert abs(value - expected) <= tolerance, "{} is not within {} of {}".format(
            amount, tolerance, expected
        )

    def run_test(self):
        node = self.nodes[0]
        account = node.get_genesis_keys().ownerAuthAddress

        # Mine blocks to get the block subsidy of the account
        node.generate(600)

        # Fund the account with the collateral of the vault
        node.utxostoaccount({account: "500@DFI"})
        node.generate(1)

        # Oracle with fixed prices: 100 USD per DFI and per TSLA
        oracle_address = node.getnewaddress("", "legacy")
        price_feeds = [
            {"currency": "USD", "token": "DFI"},
            {"currency": "USD", "token": "TSLA"},
        ]
        self.oracle_id = node.appointoracle(oracle_address, price_feeds, 10)
        node.generate(1)
        self.set_oracle_prices("100", "100")

        # DFI as collateral token, TSLA as loan token. The loan token does not
        # charge interest to keep the amounts deterministic.
        node.setcollateraltoken(
            {"token": "DFI", "factor": 1, "fixedIntervalPriceId": "DFI/USD"}
        )
        node.setloantoken(
            {
                "symbol": "TSLA",
                "name": "Tesla Token",
                "fixedIntervalPriceId": "TSLA/USD",
                "mintable": True,
                "interest": 0,
            }
        )
        # Let the fixed interval prices become active (6 blocks interval)
        node.generate(12)

        node.createloanscheme(150, 0.01, "LOAN150")
        node.generate(1)

        vault_id = node.createvault(account, "LOAN150")
        node.generate(1)

        # 200 DFI @ 100 USD are worth 20000 USD
        node.deposittovault(vault_id, account, "200@DFI")
        node.generate(1)

        vault = node.getvault(vault_id)
        assert_equal(vault["state"], "active")
        assert_equal(vault["collateralAmounts"], ["200.00000000@DFI"])
        assert_equal(vault["collateralValue"], Decimal("20000.00000000"))
        assert_equal(vault["loanAmounts"], [])
        assert_equal(vault["loanValue"], Decimal(0))

        # Taking the loan has to succeed: the collateral percentage check uses
        # the converted collateral amounts of the vault.
        # 100 TSLA @ 100 USD are worth 10000 USD
        node.takeloan({"vaultId": vault_id, "amounts": "100@TSLA"})
        node.generate(1)

        vault = node.getvault(vault_id)
        assert_equal(vault["state"], "active")
        assert_equal(vault["collateralAmounts"], ["200.00000000@DFI"])
        assert_equal(vault["collateralValue"], Decimal("20000.00000000"))
        assert_equal(len(vault["loanAmounts"]), 1)
        self.assert_amount_close(vault["loanAmounts"][0], Decimal("100"))
        self.assert_amount_close(vault["loanValue"], Decimal("10000"))
        assert_equal(vault["collateralRatio"], 200)

        # The same amounts have to be reported by listvaults
        vaults = node.listvaults({"verbose": True})
        assert_equal(len(vaults), 1)
        assert_equal(vaults[0]["collateralAmounts"], ["200.00000000@DFI"])
        assert_equal(vaults[0]["collateralValue"], Decimal("20000.00000000"))
        assert_equal(len(vaults[0]["loanAmounts"]), 1)
        self.assert_amount_close(vaults[0]["loanAmounts"][0], Decimal("100"))
        self.assert_amount_close(vaults[0]["loanValue"], Decimal("10000"))
        assert_equal(vaults[0]["collateralRatio"], 200)

        # Let the price of the loan token jump: the vault gets liquidated and
        # the loan is split into auction batches. The batches have to hold the
        # collateral of the vault, split by the value of the loan.
        # 100 TSLA @ 300 USD are worth 30000 USD, the collateral ratio of the
        # vault drops below the 150% of the loan scheme.
        self.set_oracle_prices("100", "300")
        node.generate(20)

        vault = node.getvault(vault_id)
        assert_equal(vault["state"], "inLiquidation")
        assert_equal(vault["batchCount"], 2)
        assert_equal(len(vault["batches"]), 2)
        for batch in vault["batches"]:
            assert_equal(batch["collaterals"], ["100.00000000@DFI"])
            self.assert_amount_close(batch["loan"], Decimal("50"))

        auctions = node.listauctions()
        assert_equal(len(auctions), 1)
        assert_equal(auctions[0]["batchCount"], 2)


if __name__ == "__main__":
    VaultAssetAmountsTest().main()
