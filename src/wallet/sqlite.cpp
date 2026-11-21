// Copyright (c) 2020 The Bitcoin Core developers
// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/sqlite.h>

#include <config/defi-config.h>
#include <fs.h>
#include <logging.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/db.h>

#include <stdint.h>

#include <optional>
#include <utility>
#include <vector>

#ifdef USE_SQLITE

#include <sqlite3.h>

/**
 * Version of the wallet schema that is written into the `user_version` pragma.
 * The legacy Berkeley DB wallet does not have a schema version, the SQLite
 * wallet uses the very same record format, so version 0 is the initial one.
 */
static constexpr int32_t WALLET_SCHEMA_VERSION = 0;

static Mutex g_sqlite_mutex;
static int g_sqlite_count GUARDED_BY(g_sqlite_mutex) = 0;
/**
 * Whether the SQLite library was already configured and initialized.
 *
 * The configuration (logging and threading mode) may only be changed before the
 * library is initialized, therefore it is done exactly once per process and
 * every entry point that uses the library has to go through
 * EnsureSQLiteInitialized().
 */
static bool g_sqlite_initialized GUARDED_BY(g_sqlite_mutex) = false;

static void ErrorLogCallback(void* arg, int code, const char* msg)
{
    // From sqlite3_config() documentation for the SQLITE_CONFIG_LOG option:
    // "The void pointer that is the second argument to the SQLITE_CONFIG_LOG is passed through as
    // the first parameter to the application-defined logger function whenever that function is
    // invoked."
    assert(arg == nullptr);
    LogPrintf("SQLite Error. Code: %d. Message: %s\n", code, msg);
}

static std::optional<int> ReadPragmaInteger(sqlite3* db, const std::string& key, const std::string& description, std::string& error)
{
    std::string stmt_text = strprintf("PRAGMA %s", key);
    sqlite3_stmt* pragma_read_stmt{nullptr};
    int ret = sqlite3_prepare_v2(db, stmt_text.c_str(), -1, &pragma_read_stmt, nullptr);
    if (ret != SQLITE_OK) {
        sqlite3_finalize(pragma_read_stmt);
        error = strprintf("SQLiteDatabase: Failed to prepare the statement to fetch %s: %s", description, sqlite3_errstr(ret));
        return std::nullopt;
    }
    ret = sqlite3_step(pragma_read_stmt);
    if (ret != SQLITE_ROW) {
        sqlite3_finalize(pragma_read_stmt);
        error = strprintf("SQLiteDatabase: Failed to fetch %s: %s", description, sqlite3_errstr(ret));
        return std::nullopt;
    }
    int result = sqlite3_column_int(pragma_read_stmt, 0);
    sqlite3_finalize(pragma_read_stmt);
    return result;
}

static void SetPragma(sqlite3* db, const std::string& key, const std::string& value, const std::string& err_msg)
{
    std::string stmt_text = strprintf("PRAGMA %s = %s", key, value);
    int ret = sqlite3_exec(db, stmt_text.c_str(), nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: %s: %s\n", err_msg, sqlite3_errstr(ret)));
    }
}

/**
 * Configure and initialize the SQLite library.
 *
 * The configuration of the library (error log and threading mode) may only be
 * changed before the library is initialized. As every user of the library has
 * to go through this function, the configuration is guaranteed to happen
 * exactly once and before the first use of the library.
 */
static void EnsureSQLiteInitialized()
{
    LOCK(g_sqlite_mutex);
    if (g_sqlite_initialized) return;

    // Setup logging
    int ret = sqlite3_config(SQLITE_CONFIG_LOG, ErrorLogCallback, nullptr);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup error log: %s\n", sqlite3_errstr(ret)));
    }
    // Force serialized threading mode
    ret = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to configure serialized threading mode: %s\n", sqlite3_errstr(ret)));
    }
    ret = sqlite3_initialize(); // This is a no-op if sqlite3 is already initialized
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to initialize SQLite: %s\n", sqlite3_errstr(ret)));
    }
    g_sqlite_initialized = true;
}

SQLiteDatabase::SQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock)
    : WalletDatabase(), m_mock(mock), m_dir_path(dir_path), m_file_path(file_path)
{
    EnsureSQLiteInitialized();
    {
        LOCK(g_sqlite_mutex);
        LogPrintf("Using SQLite Version %s\n", SQLiteDatabaseVersion());
        LogPrintf("Using wallet %s\n", fs::PathToString(m_dir_path));
        ++g_sqlite_count;
    }

    try {
        Open();
    } catch (const std::runtime_error&) {
        // If open fails, cleanup this object and rethrow the exception
        Cleanup();
        throw;
    }

    if (!m_mock) {
        RegisterSQLiteWallet(m_file_path);
    }
}

void SQLiteBatch::SetupSQLStatements()
{
    const std::vector<std::pair<sqlite3_stmt**, const char*>> statements{
        {&m_read_stmt, "SELECT value FROM main WHERE key = ?"},
        {&m_insert_stmt, "INSERT INTO main VALUES(?, ?)"},
        {&m_overwrite_stmt, "INSERT or REPLACE into main values(?, ?)"},
        {&m_delete_stmt, "DELETE FROM main WHERE key = ?"},
        {&m_cursor_stmt, "SELECT key, value FROM main"},
    };

    for (const auto& [stmt_prepared, stmt_text] : statements) {
        if (*stmt_prepared == nullptr) {
            int res = sqlite3_prepare_v2(m_db, stmt_text, -1, stmt_prepared, nullptr);
            if (res != SQLITE_OK) {
                throw std::runtime_error(strprintf(
                    "SQLiteDatabase: Failed to setup SQL statements: %s\n", sqlite3_errstr(res)));
            }
        }
    }
}

SQLiteDatabase::~SQLiteDatabase()
{
    if (!m_mock) {
        UnregisterSQLiteWallet(m_file_path);
    }
    Cleanup();
}

void SQLiteDatabase::Cleanup() noexcept
{
    Close();

    // The SQLite library is intentionally not shut down here: sqlite3_shutdown()
    // deinitializes the library and every subsequent use of the library would
    // require another sqlite3_initialize() call. The library stays initialized
    // for the lifetime of the process instead.
    LOCK(g_sqlite_mutex);
    if (g_sqlite_count > 0) --g_sqlite_count;
}

bool SQLiteDatabase::Verify(std::string& error)
{
    assert(m_db);

    // Check our schema version
    auto read_result = ReadPragmaInteger(m_db, "user_version", "sqlite wallet schema version", error);
    if (!read_result.has_value()) return false;
    int32_t user_ver = read_result.value();
    if (user_ver != WALLET_SCHEMA_VERSION) {
        error = strprintf("SQLiteDatabase: Unknown sqlite wallet schema version %d. Only version %d is supported", user_ver, WALLET_SCHEMA_VERSION);
        return false;
    }

    sqlite3_stmt* stmt{nullptr};
    int ret = sqlite3_prepare_v2(m_db, "PRAGMA integrity_check", -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        sqlite3_finalize(stmt);
        error = strprintf("SQLiteDatabase: Failed to prepare statement to verify database: %s", sqlite3_errstr(ret));
        return false;
    }
    while (true) {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_DONE) {
            break;
        }
        if (ret != SQLITE_ROW) {
            error = strprintf("SQLiteDatabase: Failed to execute statement to verify database: %s", sqlite3_errstr(ret));
            break;
        }
        const char* msg = (const char*)sqlite3_column_text(stmt, 0);
        if (!msg) {
            error = strprintf("SQLiteDatabase: Failed to read database verification error: %s", sqlite3_errstr(ret));
            break;
        }
        std::string str_msg(msg);
        if (str_msg == "ok") {
            continue;
        }
        if (error.empty()) {
            error = "Failed to verify database";
        }
        error += strprintf("\n%s\n", str_msg);
    }
    sqlite3_finalize(stmt);
    return error.empty();
}

void SQLiteDatabase::Open()
{
    int flags = SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (m_mock) {
        flags |= SQLITE_OPEN_MEMORY; // In memory database for mock db
    }

    if (m_db == nullptr) {
        if (!m_mock) {
            TryCreateDirectories(m_dir_path);
        }
        const std::string path = m_mock ? ":memory:" : fs::PathToString(m_file_path);
        int ret = sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr);
        if (ret != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to open database: %s\n", sqlite3_errstr(ret)));
        }
    }

    if (sqlite3_db_readonly(m_db, "main") != 0) {
        throw std::runtime_error("SQLiteDatabase: Database opened in readonly mode but read-write permissions are needed");
    }

    // Acquire an exclusive lock on the database.
    // First change the locking mode to exclusive.
    SetPragma(m_db, "locking_mode", "exclusive", "Unable to change database locking mode to exclusive");
    // Now begin a transaction to acquire the exclusive lock. This lock won't be released until we close
    // because of the exclusive locking mode.
    int ret = sqlite3_exec(m_db, "BEGIN EXCLUSIVE TRANSACTION", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("Error initializing wallet database environment %s! The wallet file is locked by another instance.\n",
                                           fs::quoted(fs::PathToString(m_file_path))));
    }
    ret = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Unable to end exclusive lock transaction: %s\n", sqlite3_errstr(ret)));
    }

    // Enable fullfsync for the platforms that use it
    SetPragma(m_db, "fullfsync", "true", "Failed to enable fullfsync");

    if (gArgs.GetBoolArg("-unsafesqlitesync", false)) {
        // Use normal synchronous mode for the journal
        LogPrintf("WARNING SQLite is configured to not wait for data to be flushed to disk. Data loss and corruption may occur.\n");
        SetPragma(m_db, "synchronous", "OFF", "Failed to set synchronous mode to OFF");
    }

    // Make the table for our key-value pairs
    // First check that the main table exists
    sqlite3_stmt* check_main_stmt{nullptr};
    ret = sqlite3_prepare_v2(m_db, "SELECT name FROM sqlite_master WHERE type='table' AND name='main'", -1, &check_main_stmt, nullptr);
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to prepare statement to check table existence: %s\n", sqlite3_errstr(ret)));
    }
    ret = sqlite3_step(check_main_stmt);
    if (sqlite3_finalize(check_main_stmt) != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to finalize statement checking table existence: %s\n", sqlite3_errstr(ret)));
    }
    bool table_exists;
    if (ret == SQLITE_DONE) {
        table_exists = false;
    } else if (ret == SQLITE_ROW) {
        table_exists = true;
    } else {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to execute statement to check table existence: %s\n", sqlite3_errstr(ret)));
    }

    // Do the db setup things because the table doesn't exist only when we are creating a new wallet
    if (!table_exists) {
        ret = sqlite3_exec(m_db, "CREATE TABLE main(key BLOB PRIMARY KEY NOT NULL, value BLOB NOT NULL)", nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to create new database: %s\n", sqlite3_errstr(ret)));
        }

        // Set the user version
        SetPragma(m_db, "user_version", strprintf("%d", WALLET_SCHEMA_VERSION),
                  "Failed to set the wallet schema version");
    }
}

bool SQLiteDatabase::Rewrite(const char* skip)
{
    // Rewrite the database using the VACUUM command: https://sqlite.org/lang_vacuum.html
    int ret = sqlite3_exec(m_db, "VACUUM", nullptr, nullptr, nullptr);
    return ret == SQLITE_OK;
}

bool SQLiteDatabase::Backup(const std::string& dest)
{
    sqlite3* db_copy;
    int res = sqlite3_open(dest.c_str(), &db_copy);
    if (res != SQLITE_OK) {
        sqlite3_close(db_copy);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(db_copy, "main", m_db, "main");
    if (!backup) {
        LogPrintf("%s: Unable to begin backup: %s\n", __func__, sqlite3_errmsg(m_db));
        sqlite3_close(db_copy);
        return false;
    }
    // Specifying -1 will copy all of the pages
    res = sqlite3_backup_step(backup, -1);
    if (res != SQLITE_DONE) {
        LogPrintf("%s: Unable to backup: %s\n", __func__, sqlite3_errstr(res));
        sqlite3_backup_finish(backup);
        sqlite3_close(db_copy);
        return false;
    }
    res = sqlite3_backup_finish(backup);
    sqlite3_close(db_copy);
    return res == SQLITE_OK;
}

void SQLiteDatabase::Close()
{
    if (!m_db) return;
    int res = sqlite3_close(m_db);
    if (res != SQLITE_OK) {
        LogPrintf("SQLiteDatabase: Failed to close database: %s\n", sqlite3_errstr(res));
    }
    m_db = nullptr;
}

std::unique_ptr<DatabaseBatch> SQLiteDatabase::MakeBatch(DatabaseBatchMode mode, bool flush_on_close)
{
    // We ignore flush_on_close because we don't do manual flushing for SQLite.
    // A read-only batch shares the connection of the database: the database
    // holds an exclusive lock on the file, so a second connection would not be
    // able to read from it.
    if (mode == DatabaseBatchMode::READ_ONLY) {
        return std::make_unique<SQLiteBatch>(*this, m_db, false, true);
    }
    return std::make_unique<SQLiteBatch>(*this, m_db, false);
}

SQLiteBatch::SQLiteBatch(SQLiteDatabase& database, sqlite3* db, bool owns_db, bool read_only)
    : m_database(database), m_db(db), m_owns_db(owns_db), m_read_only(read_only)
{
    // Make sure we have a db handle
    assert(m_db);

    SetupSQLStatements();
}

void SQLiteBatch::Close()
{
    // If this batch started a transaction that was neither committed nor aborted, roll it back.
    if (m_db && m_in_txn && sqlite3_get_autocommit(m_db) == 0) {
        if (TxnAbort()) {
            LogPrintf("SQLiteBatch: Batch closed unexpectedly without the transaction being explicitly committed or aborted\n");
        } else {
            LogPrintf("SQLiteBatch: Batch closed and failed to abort transaction\n");
        }
    }

    // Free all of the prepared statements
    const std::vector<std::pair<sqlite3_stmt**, const char*>> statements{
        {&m_read_stmt, "read"},
        {&m_insert_stmt, "insert"},
        {&m_overwrite_stmt, "overwrite"},
        {&m_delete_stmt, "delete"},
        {&m_cursor_stmt, "cursor"},
    };

    for (const auto& [stmt_prepared, stmt_description] : statements) {
        int res = sqlite3_finalize(*stmt_prepared);
        if (res != SQLITE_OK) {
            LogPrintf("SQLiteBatch: Batch closed but could not finalize %s statement: %s\n",
                      stmt_description, sqlite3_errstr(res));
        }
        *stmt_prepared = nullptr;
    }

    if (m_owns_db && m_db) {
        sqlite3_close(m_db);
    }
    m_db = nullptr;
}

bool SQLiteBatch::ReadKey(CDataStream&& key, CDataStream& value)
{
    if (!m_db) return false;
    assert(m_read_stmt);

    // Bind: leftmost parameter in statement is index 1
    int res = sqlite3_bind_blob(m_read_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        LogPrintf("%s: Unable to bind statement: %s\n", __func__, sqlite3_errstr(res));
        sqlite3_clear_bindings(m_read_stmt);
        sqlite3_reset(m_read_stmt);
        return false;
    }
    res = sqlite3_step(m_read_stmt);
    if (res != SQLITE_ROW) {
        if (res != SQLITE_DONE) {
            // SQLITE_DONE means "not found", don't log an error in that case.
            LogPrintf("%s: Unable to execute statement: %s\n", __func__, sqlite3_errstr(res));
        }
        sqlite3_clear_bindings(m_read_stmt);
        sqlite3_reset(m_read_stmt);
        return false;
    }
    // Leftmost column in result is index 0
    const char* data = reinterpret_cast<const char*>(sqlite3_column_blob(m_read_stmt, 0));
    int data_size = sqlite3_column_bytes(m_read_stmt, 0);
    value.write(data, data_size);

    sqlite3_clear_bindings(m_read_stmt);
    sqlite3_reset(m_read_stmt);
    return true;
}

bool SQLiteBatch::WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite)
{
    if (!m_db) return false;
    if (m_read_only) {
        LogPrintf("%s: refusing to write to a read-only SQLite batch\n", __func__);
        return false;
    }
    assert(m_insert_stmt && m_overwrite_stmt);

    sqlite3_stmt* stmt;
    if (overwrite) {
        stmt = m_overwrite_stmt;
    } else {
        stmt = m_insert_stmt;
    }

    // Bind: leftmost parameter in statement is index 1
    // Insert index 1 is key, 2 is value
    int res = sqlite3_bind_blob(stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        LogPrintf("%s: Unable to bind key to statement: %s\n", __func__, sqlite3_errstr(res));
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        return false;
    }
    res = sqlite3_bind_blob(stmt, 2, value.data(), value.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        LogPrintf("%s: Unable to bind value to statement: %s\n", __func__, sqlite3_errstr(res));
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        return false;
    }

    // Execute
    res = sqlite3_step(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_reset(stmt);
    if (res != SQLITE_DONE) {
        LogPrintf("%s: Unable to execute statement: %s\n", __func__, sqlite3_errstr(res));
    }
    return res == SQLITE_DONE;
}

bool SQLiteBatch::EraseKey(CDataStream&& key)
{
    if (!m_db) return false;
    if (m_read_only) {
        LogPrintf("%s: refusing to erase from a read-only SQLite batch\n", __func__);
        return false;
    }
    assert(m_delete_stmt);

    // Bind: leftmost parameter in statement is index 1
    int res = sqlite3_bind_blob(m_delete_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        LogPrintf("%s: Unable to bind statement: %s\n", __func__, sqlite3_errstr(res));
        sqlite3_clear_bindings(m_delete_stmt);
        sqlite3_reset(m_delete_stmt);
        return false;
    }

    // Execute
    res = sqlite3_step(m_delete_stmt);
    sqlite3_clear_bindings(m_delete_stmt);
    sqlite3_reset(m_delete_stmt);
    if (res != SQLITE_DONE) {
        LogPrintf("%s: Unable to execute statement: %s\n", __func__, sqlite3_errstr(res));
    }
    return res == SQLITE_DONE;
}

bool SQLiteBatch::HasKey(CDataStream&& key)
{
    if (!m_db) return false;
    assert(m_read_stmt);

    // Bind: leftmost parameter in statement is index 1
    bool ret = false;
    int res = sqlite3_bind_blob(m_read_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res == SQLITE_OK) {
        res = sqlite3_step(m_read_stmt);
        if (res == SQLITE_ROW) {
            ret = true;
        }
    }

    sqlite3_clear_bindings(m_read_stmt);
    sqlite3_reset(m_read_stmt);
    return ret;
}

bool SQLiteBatch::StartCursor()
{
    assert(!m_cursor_init);
    if (!m_db) return false;
    m_cursor_init = true;
    return true;
}

bool SQLiteBatch::ReadAtCursor(CDataStream& key, CDataStream& value, bool& complete)
{
    complete = false;

    if (!m_cursor_init) return false;

    int res = sqlite3_step(m_cursor_stmt);
    if (res == SQLITE_DONE) {
        complete = true;
        return true;
    }
    if (res != SQLITE_ROW) {
        LogPrintf("SQLiteBatch::ReadAtCursor: Unable to execute cursor step: %s\n", sqlite3_errstr(res));
        return false;
    }

    // Leftmost column in result is index 0
    const char* key_data = reinterpret_cast<const char*>(sqlite3_column_blob(m_cursor_stmt, 0));
    int key_data_size = sqlite3_column_bytes(m_cursor_stmt, 0);
    key.write(key_data, key_data_size);
    const char* value_data = reinterpret_cast<const char*>(sqlite3_column_blob(m_cursor_stmt, 1));
    int value_data_size = sqlite3_column_bytes(m_cursor_stmt, 1);
    value.write(value_data, value_data_size);
    return true;
}

void SQLiteBatch::CloseCursor()
{
    if (m_cursor_stmt) sqlite3_reset(m_cursor_stmt);
    m_cursor_init = false;
}

bool SQLiteBatch::TxnBegin()
{
    if (!m_db || sqlite3_get_autocommit(m_db) == 0) return false;
    int res = sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    if (res != SQLITE_OK) {
        LogPrintf("SQLiteBatch: Failed to begin the transaction\n");
    }
    m_in_txn = res == SQLITE_OK;
    return m_in_txn;
}

bool SQLiteBatch::TxnCommit()
{
    if (!m_db || sqlite3_get_autocommit(m_db) != 0) return false;
    int res = sqlite3_exec(m_db, "COMMIT TRANSACTION", nullptr, nullptr, nullptr);
    if (res != SQLITE_OK) {
        LogPrintf("SQLiteBatch: Failed to commit the transaction\n");
    }
    m_in_txn = false;
    return res == SQLITE_OK;
}

bool SQLiteBatch::TxnAbort()
{
    if (!m_db || sqlite3_get_autocommit(m_db) != 0) return false;
    int res = sqlite3_exec(m_db, "ROLLBACK TRANSACTION", nullptr, nullptr, nullptr);
    if (res != SQLITE_OK) {
        LogPrintf("SQLiteBatch: Failed to abort the transaction\n");
    }
    m_in_txn = false;
    return res == SQLITE_OK;
}

std::unique_ptr<WalletDatabase> MakeSQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock)
{
    return std::make_unique<SQLiteDatabase>(dir_path, file_path, mock);
}

std::string SQLiteDatabaseVersion()
{
    return std::string(sqlite3_libversion());
}

bool VerifySQLiteWallet(const fs::path& wallet_path, std::string& error)
{
    const fs::path file_path = fs::is_regular_file(wallet_path) ? wallet_path : fs::path(wallet_path / WALLET_SQLITE_FILENAME);

    if (!fs::exists(file_path)) {
        // A wallet that does not exist yet is not an error, it will be created.
        return true;
    }

    if (!IsSQLiteFile(file_path)) {
        error = strprintf("Wallet file %s is not a SQLite wallet file", fs::PathToString(file_path));
        return false;
    }

    // A wallet that is already loaded in this process holds an exclusive lock
    // on the file, the file can therefore not be opened a second time for the
    // verification. The loaded wallet was verified when it was loaded.
    if (IsWalletLoaded(wallet_path)) {
        return true;
    }

    // Make sure the library is initialized, it might have been used before.
    try {
        EnsureSQLiteInitialized();
    } catch (const std::runtime_error& e) {
        error = e.what();
        return false;
    }

    sqlite3* db{nullptr};
    int ret = sqlite3_open_v2(fs::PathToString(file_path).c_str(), &db, SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READONLY, nullptr);
    if (ret != SQLITE_OK) {
        if (db) sqlite3_close(db);
        error = strprintf("Failed to open SQLite wallet %s: %s", fs::PathToString(file_path), sqlite3_errstr(ret));
        return false;
    }

    sqlite3_stmt* stmt{nullptr};
    ret = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        if (ret == SQLITE_BUSY || ret == SQLITE_LOCKED) {
            error = strprintf("Error initializing wallet database environment %s! The wallet file is locked by another instance.",
                               fs::quoted(fs::PathToString(file_path)));
            return false;
        }
        error = strprintf("Failed to prepare statement to verify database: %s", sqlite3_errstr(ret));
        return false;
    }
    while (true) {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_DONE) break;
        if (ret != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            if (ret == SQLITE_BUSY || ret == SQLITE_LOCKED) {
                error = strprintf("Error initializing wallet database environment %s! The wallet file is locked by another instance.",
                                   fs::quoted(fs::PathToString(file_path)));
                return false;
            }
            error = strprintf("Failed to execute statement to verify database: %s", sqlite3_errstr(ret));
            return false;
        }
        const char* msg = (const char*)sqlite3_column_text(stmt, 0);
        if (!msg) {
            error = "Failed to read database verification error";
            break;
        }
        if (std::string(msg) == "ok") continue;
        error = strprintf("Failed to verify database\n%s\n", msg);
        break;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return error.empty();
}

#else // !USE_SQLITE

//
// The wallet was built without SQLite support. The SQLite backend is not
// available, every attempt to use it fails with a descriptive error.
//

std::unique_ptr<WalletDatabase> MakeSQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock)
{
    throw std::runtime_error("This build of the wallet does not support the SQLite database backend");
}

bool VerifySQLiteWallet(const fs::path& wallet_path, std::string& error)
{
    error = "This build of the wallet does not support the SQLite database backend";
    return false;
}

std::string SQLiteDatabaseVersion()
{
    return "disabled";
}

#endif // USE_SQLITE
