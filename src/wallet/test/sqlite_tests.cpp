// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <config/defi-config.h>
#include <fs.h>
#include <streams.h>
#include <test/setup_common.h>
#include <wallet/db.h>
#include <wallet/sqlite.h>

#include <util/system.h>

#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

/**
 * The very same behavioural suite is executed against every wallet database
 * backend, this proves that the backends are interchangeable.
 */
BOOST_FIXTURE_TEST_SUITE(sqlite_tests, BasicTestingSetup)

namespace {

/** Create a wallet database of the given backend in the given directory. */
std::unique_ptr<WalletDatabase> MakeDatabase(DatabaseFormat format, const fs::path& dir)
{
    return WalletDatabase::Create(dir, format);
}

/** Serialize a string the same way the wallet database does. */
std::string Serialize(const std::string& value)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << value;
    return std::string(reinterpret_cast<const char*>(ss.data()), ss.size());
}

/** Read all records of a batch as raw (serialized) pairs, in iteration order. */
std::vector<std::pair<std::string, std::string>> ReadAllRecords(DatabaseBatch& batch)
{
    std::vector<std::pair<std::string, std::string>> records;
    BOOST_REQUIRE(batch.StartCursor());
    while (true) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        bool complete = false;
        BOOST_REQUIRE(batch.ReadAtCursor(ssKey, ssValue, complete));
        if (complete) break;

        records.emplace_back(
            std::string(reinterpret_cast<const char*>(ssKey.data()), ssKey.size()),
            std::string(reinterpret_cast<const char*>(ssValue.data()), ssValue.size()));
    }
    batch.CloseCursor();
    return records;
}

} // namespace

/**
 * Run the given test case against every available backend.
 */
static void ForEachBackend(const std::function<void(DatabaseFormat)>& test)
{
    test(DatabaseFormat::BERKELEY);
#ifdef USE_SQLITE
    test(DatabaseFormat::SQLITE);
#endif
}

BOOST_AUTO_TEST_CASE(write_read_erase)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("walletdb_%d", (int)format);
        std::unique_ptr<WalletDatabase> db = MakeDatabase(format, dir);
        std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);

        const std::string key = "somekey";
        const std::string value = "somevalue";
        BOOST_CHECK(batch->Write(key, value));
        BOOST_CHECK(batch->Exists(key));

        std::string read_value;
        BOOST_CHECK(batch->Read(key, read_value));
        BOOST_CHECK_EQUAL(read_value, value);

        BOOST_CHECK(batch->Erase(key));
        BOOST_CHECK(!batch->Exists(key));
        BOOST_CHECK(!batch->Read(key, read_value));
    });
}

BOOST_AUTO_TEST_CASE(overwrite_semantics)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("walletdb_overwrite_%d", (int)format);
        std::unique_ptr<WalletDatabase> db = MakeDatabase(format, dir);
        std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);

        const std::string key = "overwritekey";
        BOOST_CHECK(batch->Write(key, std::string("first"), false));
        // Writing without the overwrite flag must not replace the existing value.
        BOOST_CHECK(!batch->Write(key, std::string("second"), false));

        std::string read_value;
        BOOST_CHECK(batch->Read(key, read_value));
        BOOST_CHECK_EQUAL(read_value, "first");

        // ... but an explicit overwrite does.
        BOOST_CHECK(batch->Write(key, std::string("third"), true));
        BOOST_CHECK(batch->Read(key, read_value));
        BOOST_CHECK_EQUAL(read_value, "third");
    });
}

BOOST_AUTO_TEST_CASE(cursor_iteration)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("walletdb_cursor_%d", (int)format);
        std::unique_ptr<WalletDatabase> db = MakeDatabase(format, dir);
        std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);

        const std::vector<std::string> keys{"key_a", "key_b", "key_c", "key_d"};
        for (const std::string& key : keys) {
            BOOST_CHECK(batch->Write(key, key + "_value"));
        }

        const auto records = ReadAllRecords(*batch);
        BOOST_REQUIRE(records.size() >= keys.size());

        // The iteration order has to be stable and sorted by the (raw) key.
        for (size_t i = 1; i < records.size(); ++i) {
            BOOST_CHECK(records[i - 1].first < records[i].first);
        }

        // Every written record has to be present with the expected value.
        for (const std::string& key : keys) {
            bool found = false;
            for (const auto& record : records) {
                if (record.first == Serialize(key)) {
                    BOOST_CHECK_EQUAL(record.second, Serialize(key + "_value"));
                    found = true;
                    break;
                }
            }
            BOOST_CHECK(found);
        }
    });
}

BOOST_AUTO_TEST_CASE(transaction_commit_and_abort)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("walletdb_txn_%d", (int)format);
        std::unique_ptr<WalletDatabase> db = MakeDatabase(format, dir);
        std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);

        const std::string committed = "committed";
        const std::string aborted = "aborted";

        BOOST_CHECK(batch->TxnBegin());
        BOOST_CHECK(batch->Write(committed, committed));
        BOOST_CHECK(batch->TxnCommit());

        std::string read_value;
        BOOST_CHECK(batch->Read(committed, read_value));
        BOOST_CHECK_EQUAL(read_value, committed);

        BOOST_CHECK(batch->TxnBegin());
        BOOST_CHECK(batch->Write(aborted, aborted));
        BOOST_CHECK(batch->TxnAbort());

        BOOST_CHECK(!batch->Read(aborted, read_value));
    });
}

BOOST_AUTO_TEST_CASE(dummy_database_never_fails)
{
    std::unique_ptr<WalletDatabase> db = WalletDatabase::CreateDummy();
    std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);

    const std::string key = "mockkey";
    BOOST_CHECK(batch->Write(key, std::string("mockvalue")));

    std::string read_value;
    BOOST_CHECK(!batch->Read(key, read_value));
    BOOST_CHECK(!batch->Exists(key));
    BOOST_CHECK(!batch->Erase(key));
}

BOOST_AUTO_TEST_CASE(format_detection)
{
    const fs::path dir = GetDataDir() / "walletdb_detection";
    DatabaseFormat format = DatabaseFormat::BERKELEY;

    // A directory without a wallet file has no format yet.
    BOOST_CHECK(DetectWalletFormat(dir, format) == WalletFormatStatus::NOT_FOUND);

    std::unique_ptr<WalletDatabase> db = MakeDatabase(DatabaseFormat::BERKELEY, dir);
    std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);
    BOOST_CHECK(batch->Write(std::string("key"), std::string("value")));
    batch.reset();
    db.reset();

    // Now the wallet file exists and the format is detected.
    format = DatabaseFormat::SQLITE;
    BOOST_CHECK(DetectWalletFormat(dir, format) == WalletFormatStatus::OK);
    BOOST_CHECK(format == DatabaseFormat::BERKELEY);
}

#ifdef USE_SQLITE
BOOST_AUTO_TEST_CASE(sqlite_specifics)
{
    const fs::path dir = GetDataDir() / "walletdb_sqlite";
    std::unique_ptr<WalletDatabase> db = MakeDatabase(DatabaseFormat::SQLITE, dir);
    std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::CREATE);
    BOOST_CHECK(batch->Write(std::string("key"), std::string("value")));
    batch.reset();

    // The wallet is a single SQLite file with the expected magic.
    const fs::path data_file = dir / "wallet.sqlite";
    BOOST_CHECK(fs::exists(data_file));
    BOOST_CHECK(IsSQLiteFile(data_file));
    BOOST_CHECK(!IsBDBFile(data_file));

    db.reset();

    std::string error;
    BOOST_CHECK(VerifySQLiteWallet(dir, error));
    BOOST_CHECK(error.empty());

    // The integrity check detects a corrupted database.
    {
        std::fstream file(data_file, std::ios::in | std::ios::out | std::ios::binary);
        std::vector<char> garbage(4096, 'x');
        file.seekp(1000);
        file.write(garbage.data(), garbage.size());
    }
    BOOST_CHECK(!VerifySQLiteWallet(dir, error));
    BOOST_CHECK(!error.empty());
}
#endif // USE_SQLITE

BOOST_AUTO_TEST_SUITE_END()
