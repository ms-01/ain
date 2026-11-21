#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the SQLite wallet database backend.

The SQLite backend is opt-in, the default backend stays the legacy Berkeley DB.
"""
import os

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class WalletSQLiteTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def wallet_dir(self, name=""):
        return os.path.join(self.nodes[0].datadir, "regtest", "wallets", name)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("The default wallet still uses the legacy Berkeley DB backend")
        default_wallet_file = os.path.join(self.wallet_dir(), "wallet.dat")
        assert os.path.isfile(default_wallet_file)
        assert not os.path.isfile(os.path.join(self.wallet_dir(), "wallet.sqlite"))

        self.log.info("Create a wallet with the SQLite backend")
        node.createwallet(wallet_name="sqlite_wallet", dbtype="sqlite")
        w = node.get_wallet_rpc("sqlite_wallet")

        wallet_file = os.path.join(self.wallet_dir("sqlite_wallet"), "wallet.sqlite")
        assert os.path.isfile(wallet_file)
        # The legacy Berkeley DB file is never created for a SQLite wallet.
        assert not os.path.isfile(
            os.path.join(self.wallet_dir("sqlite_wallet"), "wallet.dat")
        )
        # ... and the file has the SQLite magic.
        with open(wallet_file, "rb") as f:
            assert_equal(f.read(16), b"SQLite format 3\x00")

        self.log.info("An unknown database type is rejected")
        assert_raises_rpc_error(
            -8,
            'Unknown wallet database type unknown, expected "bdb" or "sqlite"',
            node.createwallet,
            wallet_name="invalid_wallet",
            dbtype="unknown",
        )

        self.log.info("The SQLite wallet behaves like a regular wallet")
        address = w.getnewaddress()
        node.generate(101, address=address)
        assert_equal(w.getbalance(), 50)
        assert_equal(w.getwalletinfo()["walletname"], "sqlite_wallet")

        self.log.info("The wallet can receive and send coins")
        target = w.getnewaddress()
        txid = w.sendtoaddress(target, 10)
        node.generate(1)
        assert_equal(w.gettransaction(txid)["confirmations"], 1)
        assert_equal(w.getaddressinfo(target)["ismine"], True)
        assert w.getbalance() > 0

        self.log.info("The wallet can be encrypted")
        w.encryptwallet("passphrase")
        assert w.getwalletinfo()["unlocked_until"] == 0
        w.walletpassphrase("passphrase", 10)
        assert w.getwalletinfo()["unlocked_until"] > 0

        self.log.info("The wallet can be backed up")
        backup_file = os.path.join(self.options.tmpdir, "sqlite_backup.sqlite")
        w.backupwallet(backup_file)
        assert os.path.isfile(backup_file)
        with open(backup_file, "rb") as f:
            assert_equal(f.read(16), b"SQLite format 3\x00")

        self.log.info("The state of the wallet survives a restart")
        balance = w.getbalance()
        self.restart_node(0)
        node.loadwallet("sqlite_wallet")
        w = node.get_wallet_rpc("sqlite_wallet")
        assert_equal(w.getbalance(), balance)
        assert_equal(w.getaddressinfo(address)["ismine"], True)

        self.log.info("The SQLite wallet is found by the wallet directory scan")
        names = [w["name"] for w in node.listwalletdir()["wallets"]]
        assert "sqlite_wallet" in names

        self.log.info("A SQLite wallet can be loaded explicitly")
        node.unloadwallet("sqlite_wallet")
        assert "sqlite_wallet" not in node.listwallets()
        node.loadwallet("sqlite_wallet")
        assert "sqlite_wallet" in node.listwallets()
        assert_equal(w.getbalance(), balance)


if __name__ == "__main__":
    WalletSQLiteTest().main()
