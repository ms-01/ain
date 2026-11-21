#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the migration of a legacy Berkeley DB wallet into a SQLite wallet."""
import hashlib
import os

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class WalletMigrationTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def wallet_dir(self, name=""):
        return os.path.join(self.nodes[0].datadir, "regtest", "wallets", name)

    def checksum(self, path):
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create a legacy Berkeley DB wallet with some coins")
        # The legacy wallet always uses the Berkeley DB backend, independently of
        # the default backend of the node.
        node.createwallet(wallet_name="legacy", dbtype="bdb")
        legacy = node.get_wallet_rpc("legacy")
        address = legacy.getnewaddress()
        node.generate(101, address=address)
        assert_equal(legacy.getbalance(), 50)

        target_address = legacy.getnewaddress()
        legacy.sendtoaddress(target_address, 10)
        node.generate(1)
        legacy_balance = legacy.getbalance()
        legacy_tx_count = len(legacy.listtransactions())
        assert legacy_balance > 0

        legacy_file = os.path.join(self.wallet_dir("legacy"), "wallet.dat")
        assert os.path.isfile(legacy_file)

        self.log.info("A loaded wallet cannot be migrated")
        assert_raises_rpc_error(
            -4,
            "Wallet legacy is loaded, unload it before migrating it.",
            node.migratewallet,
            "legacy",
        )

        self.log.info("Unload the legacy wallet and migrate it")
        node.unloadwallet("legacy")
        checksum_before = self.checksum(legacy_file)
        result = node.migratewallet("legacy")
        assert_equal(result["source"], "legacy")
        assert_equal(result["target"], "legacy_sqlite")
        assert_equal(result["format"], "sqlite")
        assert result["records"] > 0

        self.log.info("The legacy wallet was not modified by the migration")
        assert_equal(self.checksum(legacy_file), checksum_before)

        self.log.info("The migrated wallet contains the same state")
        node.loadwallet("legacy_sqlite")
        migrated = node.get_wallet_rpc("legacy_sqlite")
        assert_equal(migrated.getbalance(), legacy_balance)
        assert_equal(migrated.getaddressinfo(address)["ismine"], True)
        assert_equal(migrated.getaddressinfo(target_address)["ismine"], True)
        assert_equal(len(migrated.listtransactions()), legacy_tx_count)

        self.log.info("The migrated wallet is a SQLite wallet")
        migrated_file = os.path.join(self.wallet_dir("legacy_sqlite"), "wallet.sqlite")
        assert os.path.isfile(migrated_file)
        with open(migrated_file, "rb") as f:
            assert_equal(f.read(16), b"SQLite format 3\x00")
        assert not os.path.isfile(
            os.path.join(self.wallet_dir("legacy_sqlite"), "wallet.dat")
        )

        self.log.info("The migrated wallet survives a restart")
        balance = migrated.getbalance()
        self.restart_node(0)
        node.loadwallet("legacy_sqlite")
        migrated = node.get_wallet_rpc("legacy_sqlite")
        assert_equal(migrated.getbalance(), balance)

        self.log.info("An existing target wallet is never overwritten")
        assert_raises_rpc_error(
            -4,
            "Wallet legacy_sqlite already exists.",
            node.migratewallet,
            "legacy",
            "legacy_sqlite",
        )

        self.log.info("A missing source wallet is reported")
        assert_raises_rpc_error(
            -18,
            "Wallet does_not_exist not found.",
            node.migratewallet,
            "does_not_exist",
        )

        self.log.info("The legacy wallet can still be used, spent, and reopened")
        node.loadwallet("legacy")
        legacy = node.get_wallet_rpc("legacy")
        assert_equal(legacy.getbalance(), legacy_balance)
        legacy.sendtoaddress(target_address, 1)
        node.generate(1)
        assert legacy.getbalance() > 0


if __name__ == "__main__":
    WalletMigrationTest().main()
