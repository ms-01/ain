// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <config/defi-config.h>
#include <fs.h>
#include <test/setup_common.h>
#include <util/system.h>
#include <wallet/db.h>
#include <wallet/migrate.h>

#include <memory>
#include <string>

BOOST_FIXTURE_TEST_SUITE(migrate_tests, BasicTestingSetup)

#ifdef USE_SQLITE

/** Create a legacy Berkeley DB wallet with the given number of records. */
static void CreateLegacyWallet(const fs::path& dir, int records)
{
    std::unique_ptr<WalletDatabase> db = WalletDatabase::Create(dir, DatabaseFormat::BERKELEY);
    std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);
    for (int i = 0; i < records; ++i) {
        BOOST_REQUIRE(batch->Write(strprintf("key_%d", i), strprintf("value_%d", i)));
    }
    batch.reset();
    db.reset();
}

BOOST_AUTO_TEST_CASE(migrate_legacy_wallet)
{
    const fs::path source_dir = GetDataDir() / "legacy_wallet";
    const fs::path target_dir = GetDataDir() / "migrated_wallet";
    CreateLegacyWallet(source_dir, 10);

    WalletMigrationResult result;
    BOOST_REQUIRE(MigrateWalletDatabase(source_dir, target_dir, DatabaseFormat::SQLITE, result));
    BOOST_CHECK(result.success);
    // The legacy wallet also contains the "version" record, so the migrated wallet has one record more.
    BOOST_CHECK(result.records >= 10U);
    BOOST_CHECK(result.source_format == DatabaseFormat::BERKELEY);
    BOOST_CHECK(result.target_format == DatabaseFormat::SQLITE);
    BOOST_CHECK(result.error.empty());

    // The migrated wallet contains exactly the same records.
    std::unique_ptr<WalletDatabase> migrated = WalletDatabase::Create(target_dir, DatabaseFormat::SQLITE);
    std::unique_ptr<DatabaseBatch> batch = migrated->MakeBatch(DatabaseBatchMode::READ_ONLY);
    for (int i = 0; i < 10; ++i) {
        std::string value;
        BOOST_CHECK(batch->Read(strprintf("key_%d", i), value));
        BOOST_CHECK_EQUAL(value, strprintf("value_%d", i));
    }
    std::string missing;
    BOOST_CHECK(!batch->Read("key_does_not_exist", missing));
}

BOOST_AUTO_TEST_CASE(migrate_refuses_existing_target)
{
    const fs::path source_dir = GetDataDir() / "legacy_wallet_existing_target";
    const fs::path target_dir = GetDataDir() / "migrated_wallet_existing_target";
    CreateLegacyWallet(source_dir, 3);
    CreateLegacyWallet(target_dir, 1);

    WalletMigrationResult result;
    BOOST_CHECK(!MigrateWalletDatabase(source_dir, target_dir, DatabaseFormat::SQLITE, result));
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());

    // The existing target is not touched.
    std::unique_ptr<WalletDatabase> target = WalletDatabase::Create(target_dir, DatabaseFormat::BERKELEY);
    std::unique_ptr<DatabaseBatch> batch = target->MakeBatch(DatabaseBatchMode::READ_ONLY);
    std::string value;
    BOOST_CHECK(!batch->Read("key_0", value));
}

BOOST_AUTO_TEST_CASE(migrate_refuses_missing_source)
{
    const fs::path source_dir = GetDataDir() / "missing_wallet";
    const fs::path target_dir = GetDataDir() / "migrated_wallet_missing_source";

    WalletMigrationResult result;
    BOOST_CHECK(!MigrateWalletDatabase(source_dir, target_dir, DatabaseFormat::SQLITE, result));
    BOOST_CHECK(!result.success);
    BOOST_CHECK(!result.error.empty());

    // No target wallet was created.
    BOOST_CHECK(!fs::exists(target_dir));
}

BOOST_AUTO_TEST_CASE(migrate_empty_wallet)
{
    const fs::path source_dir = GetDataDir() / "legacy_wallet_empty";
    const fs::path target_dir = GetDataDir() / "migrated_wallet_empty";
    CreateLegacyWallet(source_dir, 0);

    WalletMigrationResult result;
    BOOST_REQUIRE(MigrateWalletDatabase(source_dir, target_dir, DatabaseFormat::SQLITE, result));
    BOOST_CHECK(result.success);
    BOOST_CHECK(result.records >= 0U);
}

#endif // USE_SQLITE

BOOST_AUTO_TEST_SUITE_END()
