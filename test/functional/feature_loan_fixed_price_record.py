#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Regression test for the fixed interval price records of loan and collateral tokens.

When a loan token or a collateral token is created, the aggregated oracle price
of its price feed is written into the fixed interval price record
(src/dfi/consensus/loans.cpp):

  * collateral token: fixedIntervalPrice.priceRecord[1] = *price;
  * loan token:       fixedIntervalPrice.priceRecord[1] = *nextPrice;

If such a price is silently collapsed to a boolean (1 satoshi), the record holds
0.00000001 instead of the real price. All interest calculations and the
collateral valuation of the vaults are based on that record, so the loan
interest and the collateral ratio are computed from a completely wrong price.

The test creates a loan token and two collateral tokens with fixed oracle
prices and verifies that

  * the fixed interval price record of every feed holds the aggregated oracle
    price right after the token creation (and not 0.00000001),
  * the active price of every feed becomes the aggregated oracle price with the
    next price update, and
  * the interest of a taken loan is derived from that price.
"""

import calendar
import time
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal


class LoanFixedPriceRecordTest(DefiTestFramework):
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

    # Oracle prices used by this test
    DFI_PRICE = Decimal("100.00000000")
    BTC_PRICE = Decimal("50.00000000")
    TSLA_PRICE = Decimal("100.00000000")

    def set_oracle_prices(self):
        """Feed the oracle with the fixed prices of this test."""
        prices = [
            {"currency": "USD", "tokenAmount": "100@DFI"},
            {"currency": "USD", "tokenAmount": "50@BTC"},
            {"currency": "USD", "tokenAmount": "100@TSLA"},
        ]
        timestamp = calendar.timegm(time.gmtime())
        self.nodes[0].setoracledata(self.oracle_id, timestamp, prices)
        self.nodes[0].generate(1)

    def assert_price_record(self, price_feed_id, expected_price):
        """The fixed interval price record has to hold the oracle price."""
        fixed_price = self.nodes[0].getfixedintervalprice(price_feed_id)
        # The next price is written by the token creation, the active price
        # becomes the next price with the first price update.
        assert_equal(fixed_price["nextPrice"], expected_price)
        assert fixed_price["nextPrice"] != Decimal("0.00000001")
        if fixed_price["activePrice"] > 0:
            assert_equal(fixed_price["activePrice"], expected_price)
            assert fixed_price["activePrice"] != Decimal("0.00000001")

    def run_test(self):
        node = self.nodes[0]
        account = node.get_genesis_keys().ownerAuthAddress

        # Mine blocks to get the block subsidy of the account
        node.generate(600)

        # Fund the account with the collateral of the vault
        node.utxostoaccount({account: "1000@DFI"})
        node.generate(1)

        # BTC is used as an additional collateral token
        node.createtoken(
            {
                "symbol": "BTC",
                "name": "BTC token",
                "isDAT": True,
                "collateralAddress": account,
            }
        )
        node.generate(1)

        # Mint BTC for the collateral of the vault
        node.minttokens("1000@BTC")
        node.generate(1)

        # Oracle with fixed prices for all price feeds of this test
        oracle_address = node.getnewaddress("", "legacy")
        price_feeds = [
            {"currency": "USD", "token": "DFI"},
            {"currency": "USD", "token": "BTC"},
            {"currency": "USD", "token": "TSLA"},
        ]
        self.oracle_id = node.appointoracle(oracle_address, price_feeds, 10)
        node.generate(1)
        self.set_oracle_prices()

        # Collateral tokens: the price record of the collateral price feeds is
        # written by CLoansConsensus::operator()(CLoanSetCollateralTokenMessage)
        node.setcollateraltoken(
            {"token": "DFI", "factor": 1, "fixedIntervalPriceId": "DFI/USD"}
        )
        node.generate(1)
        self.assert_price_record("DFI/USD", self.DFI_PRICE)

        node.setcollateraltoken(
            {"token": "BTC", "factor": 1, "fixedIntervalPriceId": "BTC/USD"}
        )
        node.generate(1)
        self.assert_price_record("BTC/USD", self.BTC_PRICE)

        # Loan token: the price record of the loan price feed is written by
        # CLoansConsensus::operator()(CLoanSetLoanTokenMessage)
        node.setloantoken(
            {
                "symbol": "TSLA",
                "name": "Tesla Token",
                "fixedIntervalPriceId": "TSLA/USD",
                "mintable": True,
                "interest": 1,
            }
        )
        node.generate(1)
        self.assert_price_record("TSLA/USD", self.TSLA_PRICE)

        # Let the price update run: the active price has to become the oracle
        # price of every price feed as well.
        for _ in range(12):
            node.generate(1)
            self.assert_price_record("DFI/USD", self.DFI_PRICE)
            self.assert_price_record("BTC/USD", self.BTC_PRICE)
            self.assert_price_record("TSLA/USD", self.TSLA_PRICE)

        # The interest of a taken loan is derived from the fixed price record
        node.createloanscheme(150, 1, "LOAN1")
        node.generate(1)

        vault_id = node.createvault(account, "LOAN1")
        node.generate(1)

        # 200 DFI @ 100 USD and 100 BTC @ 50 USD are worth 25000 USD
        node.deposittovault(vault_id, account, "200@DFI")
        node.generate(1)
        node.deposittovault(vault_id, account, "100@BTC")
        node.generate(1)

        vault = node.getvault(vault_id)
        assert_equal(vault["state"], "active")
        assert_equal(vault["collateralValue"], Decimal("25000.00000000"))
        assert_equal(
            vault["collateralAmounts"], ["200.00000000@DFI", "100.00000000@BTC"]
        )

        # 100 TSLA @ 100 USD are worth 10000 USD
        node.takeloan({"vaultId": vault_id, "amounts": "100@TSLA"})
        node.generate(1)

        vault = node.getvault(vault_id)
        assert_equal(vault["state"], "active")
        assert_equal(vault["collateralValue"], Decimal("25000.00000000"))
        assert_equal(len(vault["loanAmounts"]), 1)
        assert_equal(vault["loanAmounts"][0].split("@")[1], "TSLA")
        # The loan amount is the taken loan plus the accrued interest
        loan_amount = Decimal(vault["loanAmounts"][0].split("@")[0])
        assert loan_amount > Decimal("100"), loan_amount
        assert loan_amount < Decimal("101"), loan_amount
        assert_equal(vault["collateralRatio"], 250)


if __name__ == "__main__":
    LoanFixedPriceRecordTest().main()
