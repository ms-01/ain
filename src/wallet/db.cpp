// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <fs.h>
#include <wallet/db.h>

#include <logging.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

/** Name of the legacy Berkeley DB wallet data file inside a wallet directory. */
const char* const WALLET_BDB_FILENAME = "wallet.dat";
/** Name of the SQLite wallet data file inside a wallet directory. */
const char* const WALLET_SQLITE_FILENAME = "wallet.sqlite";

std::string DatabaseFormatName(DatabaseFormat format)
{
    switch (format) {
    case DatabaseFormat::BERKELEY:
        return "bdb";
    case DatabaseFormat::SQLITE:
        return "sqlite";
    }
    return "unknown";
}

bool IsBDBFile(const fs::path& path)
{
    if (!fs::exists(path)) return false;

    // A Berkeley DB Btree file has at least 4K.
    // This check also prevents opening lock files.
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) LogPrintf("%s: %s %s\n", __func__, ec.message(), fs::PathToString(path));
    if (size < 4096) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(12, std::ios::beg); // Magic bytes start at offset 12
    uint32_t data = 0;
    file.read((char*) &data, sizeof(data)); // Read 4 bytes of file to compare against magic

    // Berkeley DB Btree magic bytes, from:
    //  https://github.com/file/file/blob/5824af38469ec1ca9ac3ffd251e7afe9dc11e227/magic/Magdir/database#L74-L75
    //  - big endian systems - 00 05 31 62
    //  - little endian systems - 62 31 05 00
    return data == 0x00053162 || data == 0x62310500;
}

bool IsSQLiteFile(const fs::path& path)
{
    if (!fs::exists(path)) return false;

    // A SQLite database file is at least 512 bytes.
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) LogPrintf("%s: %s %s\n", __func__, ec.message(), fs::PathToString(path));
    if (size < 512) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // Magic is at the beginning of the file and is 16 bytes long.
    char magic[16];
    file.read(magic, 16);
    if (file.gcount() != 16) return false;

    // Check the magic, see https://sqlite.org/fileformat2.html
    return memcmp(magic, "SQLite format 3", 16) == 0;
}

namespace {

/**
 * Registry of the currently loaded SQLite wallets, used to detect duplicated
 * wallet paths.
 */
Mutex g_sqlite_wallets_mutex;
std::set<fs::path> g_sqlite_wallets GUARDED_BY(g_sqlite_wallets_mutex);

/** Return whether the given wallet path refers to a SQLite wallet. */
bool IsSQLiteWallet(const fs::path& wallet_path)
{
    if (fs::is_regular_file(wallet_path)) return IsSQLiteFile(wallet_path);
    return fs::exists(wallet_path / WALLET_SQLITE_FILENAME);
}

} // namespace

fs::path SQLiteWalletFilePath(const fs::path& wallet_path)
{
    if (fs::is_regular_file(wallet_path)) return wallet_path;
    return wallet_path / WALLET_SQLITE_FILENAME;
}

bool IsSQLiteWalletLoaded(const fs::path& wallet_path)
{
    LOCK(g_sqlite_wallets_mutex);
    return g_sqlite_wallets.count(SQLiteWalletFilePath(wallet_path)) > 0;
}

WalletFormatStatus DetectWalletFormat(const fs::path& wallet_path, DatabaseFormat& format)
{
    if (fs::is_regular_file(wallet_path)) {
        // A single file was given, detect the format from the file magic.
        if (IsBDBFile(wallet_path)) {
            format = DatabaseFormat::BERKELEY;
            return WalletFormatStatus::OK;
        }
        if (IsSQLiteFile(wallet_path)) {
            format = DatabaseFormat::SQLITE;
            return WalletFormatStatus::OK;
        }
        return WalletFormatStatus::BAD_FORMAT;
    }

    if (fs::is_directory(wallet_path)) {
        const bool has_bdb = fs::exists(wallet_path / WALLET_BDB_FILENAME);
        const bool has_sqlite = fs::exists(wallet_path / WALLET_SQLITE_FILENAME);
        if (has_bdb && has_sqlite) return WalletFormatStatus::AMBIGUOUS;
        if (has_bdb) {
            format = DatabaseFormat::BERKELEY;
            return WalletFormatStatus::OK;
        }
        if (has_sqlite) {
            format = DatabaseFormat::SQLITE;
            return WalletFormatStatus::OK;
        }
    }

    return WalletFormatStatus::NOT_FOUND;
}

void RegisterSQLiteWallet(const fs::path& wallet_file)
{
    LOCK(g_sqlite_wallets_mutex);
    g_sqlite_wallets.insert(wallet_file);
}

void UnregisterSQLiteWallet(const fs::path& wallet_file)
{
    LOCK(g_sqlite_wallets_mutex);
    g_sqlite_wallets.erase(wallet_file);
}

fs::path WalletDataFilePath(const fs::path& wallet_path)
{
    if (IsSQLiteWallet(wallet_path)) {
        return SQLiteWalletFilePath(wallet_path);
    }
    if (fs::is_regular_file(wallet_path)) {
        // Special case for backwards compatibility: if wallet path points to an
        // existing file, treat it as the path to a BDB data file in a parent
        // directory that also contains BDB log files.
        return wallet_path;
    }
    // Normal case: Interpret wallet path as a directory path containing
    // data and log files.
    return wallet_path / WALLET_BDB_FILENAME;
}

namespace {

/**
 * A batch that never fails. It mimics the behaviour of a Berkeley DB batch
 * that is connected to a dummy (non existing) database: reads always fail,
 * writes silently succeed.
 */
class DummyBatch : public DatabaseBatch
{
protected:
    bool ReadKey(CDataStream&& key, CDataStream& value) override { return false; }
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override { return true; }
    bool EraseKey(CDataStream&& key) override { return false; }
    bool HasKey(CDataStream&& key) override { return false; }

public:
    void Flush() override {}
    void Close() override {}

    bool StartCursor() override { return false; }
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override
    {
        complete = true;
        return false;
    }
    void CloseCursor() override {}
    bool TxnBegin() override { return false; }
    bool TxnCommit() override { return false; }
    bool TxnAbort() override { return false; }
};

/** A dummy wallet database that does nothing. Only used by unit tests. */
class DummyDatabase : public WalletDatabase
{
public:
    void Open() override {}
    std::unique_ptr<DatabaseBatch> MakeBatch(DatabaseBatchMode mode = DatabaseBatchMode::READ_WRITE, bool flush_on_close = true) override
    {
        return std::make_unique<DummyBatch>();
    }
    bool Rewrite(const char* pszSkip = nullptr) override { return true; }
    bool Backup(const std::string& strDest) override { return false; }
    void Flush(bool shutdown) override {}
    bool PeriodicFlush() override { return true; }
    void Close() override {}
    void ReloadDbEnv() override {}
    std::string Filename() override { return "dummy"; }
    std::string Format() override { return "dummy"; }
    fs::path Directory() override { return fs::path(); }
};

} // namespace

std::unique_ptr<WalletDatabase> WalletDatabase::CreateDummy()
{
    return std::make_unique<DummyDatabase>();
}
