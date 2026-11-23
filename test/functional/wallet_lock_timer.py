#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the RPC timer that locks the wallet again after the timeout."""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, wait_until

# The timeout of the wallet unlock, in seconds.
UNLOCK_TIMEOUT = 2


class WalletLockTimerTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.log.info("Create a wallet that is encrypted with a passphrase")
        self.nodes[0].createwallet("timerwallet", False, False, "pass")
        wallet = self.nodes[0].get_wallet_rpc("timerwallet")

        self.log.info("The wallet is locked after the creation")
        assert_equal(wallet.getwalletinfo()["unlocked_until"], 0)

        self.log.info("Unlock the wallet for %i seconds", UNLOCK_TIMEOUT)
        wallet.walletpassphrase("pass", UNLOCK_TIMEOUT)
        assert wallet.getwalletinfo()["unlocked_until"] > 0

        self.log.info("The wallet is locked again by the RPC timer")
        wait_until(lambda: wallet.getwalletinfo()["unlocked_until"] == 0, timeout=30)

        self.log.info("The wallet can be unlocked again")
        wallet.walletpassphrase("pass", UNLOCK_TIMEOUT)
        assert wallet.getwalletinfo()["unlocked_until"] > 0
        wait_until(lambda: wallet.getwalletinfo()["unlocked_until"] == 0, timeout=30)


if __name__ == "__main__":
    WalletLockTimerTest().main()
