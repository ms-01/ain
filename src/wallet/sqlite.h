// Copyright (c) 2020 The Bitcoin Core developers
// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_WALLET_SQLITE_H
#define DEFI_WALLET_SQLITE_H

#include <wallet/db.h>

#include <memory>
#include <string>

// Forward declarations of the SQLite types, the SQLite headers are only
// available when the wallet is built with SQLite support.
struct sqlite3;
struct sqlite3_stmt;

class SQLiteDatabase;

/** RAII class that provides access to a SQLite backed wallet database. */
class SQLiteBatch : public DatabaseBatch
{
private:
    SQLiteDatabase& m_database;

    /** Connection used by this batch. Either the shared read-write connection
     *  of the database or a dedicated read-only connection. */
    sqlite3* m_db{nullptr};
    /** Whether this batch owns (and therefore has to close) its connection. */
    bool m_owns_db{false};

    /** Whether the batch is only allowed to read. */
    bool m_read_only{false};

    /** Whether this batch started a transaction that has to be rolled back on close. */
    bool m_in_txn{false};

    bool m_cursor_init{false};

    sqlite3_stmt* m_read_stmt{nullptr};
    sqlite3_stmt* m_insert_stmt{nullptr};
    sqlite3_stmt* m_overwrite_stmt{nullptr};
    sqlite3_stmt* m_delete_stmt{nullptr};
    sqlite3_stmt* m_cursor_stmt{nullptr};

    void SetupSQLStatements();

    bool ReadKey(CDataStream&& key, CDataStream& value) override;
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override;
    bool EraseKey(CDataStream&& key) override;
    bool HasKey(CDataStream&& key) override;

public:
    explicit SQLiteBatch(SQLiteDatabase& database, sqlite3* db, bool owns_db, bool read_only = false);
    ~SQLiteBatch() override { Close(); }

    /* No-op. See comment on SQLiteDatabase::Flush */
    void Flush() override {}

    void Close() override;

    bool StartCursor() override;
    bool ReadAtCursor(CDataStream& key, CDataStream& value, bool& complete) override;
    void CloseCursor() override;
    bool TxnBegin() override;
    bool TxnCommit() override;
    bool TxnAbort() override;
};

/** An instance of this class represents one SQLite3 database. */
class SQLiteDatabase : public WalletDatabase
{
private:
    const bool m_mock{false};

    const fs::path m_dir_path;

    const fs::path m_file_path;

    void Cleanup() noexcept;

public:
    SQLiteDatabase() = delete;

    /** Create DB handle to real database */
    SQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock = false);

    ~SQLiteDatabase() override;

    /** Run the SQLite integrity check on the database. */
    bool Verify(std::string& error);

    /** Open the database if it is not already opened. */
    void Open() override;

    /** Close the database. */
    void Close() override;

    /* These functions are unused */
    void AddRef() { assert(false); }
    void RemoveRef() { assert(false); }

    /** Rewrite the entire database on disk */
    bool Rewrite(const char* skip = nullptr) override;

    /** Back up the entire database to a file. */
    bool Backup(const std::string& dest) override;

    /**
     * No-ops
     *
     * SQLite always flushes everything to the database file after each
     * transaction (each Read/Write/Erase that we do is its own transaction
     * unless we called TxnBegin) so there is no need to have Flush or
     * Periodic Flush.
     *
     * There is no DB env to reload, so ReloadDbEnv has nothing to do.
     */
    void Flush(bool shutdown) override {}
    bool PeriodicFlush() override { return false; }
    void ReloadDbEnv() override {}


    std::string Filename() override { return fs::PathToString(m_file_path); }
    std::string Format() override { return "sqlite"; }

    fs::path Directory() override { return m_dir_path; }

    /** Make a SQLiteBatch connected to this database */
    std::unique_ptr<DatabaseBatch> MakeBatch(DatabaseBatchMode mode = DatabaseBatchMode::READ_WRITE, bool flush_on_close = true) override;

    sqlite3* m_db{nullptr};
};

/** Create a SQLite backed wallet database for the given directory. */
std::unique_ptr<WalletDatabase> MakeSQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock = false);

/** Verify that the given SQLite wallet file exists and is not corrupt. */
bool VerifySQLiteWallet(const fs::path& wallet_path, std::string& error);

/** Version of the SQLite library that is linked in. */
std::string SQLiteDatabaseVersion();

#endif // DEFI_WALLET_SQLITE_H
