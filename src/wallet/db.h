// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_WALLET_DB_H
#define DEFI_WALLET_DB_H

#include <clientversion.h>
#include <fs.h>
#include <streams.h>

#include <atomic>
#include <memory>
#include <string>
#include <sync.h>

static const unsigned int DEFAULT_WALLET_DBLOGSIZE = 100;
static const bool DEFAULT_WALLET_PRIVDB = true;

/**
 * Wallet database backends.
 *
 * The wallet storage layer is backend agnostic: every backend stores the very
 * same generic serialized key/value records, so the concrete implementation
 * only differs in the way the records are persisted on disk.
 */
enum class DatabaseFormat {
    BERKELEY, //!< Legacy Berkeley DB (BDB 4.8) wallet files ("wallet.dat").
    SQLITE,   //!< SQLite wallet files ("wallet.sqlite").
};

/** Access mode used when opening a wallet database batch. */
enum class DatabaseBatchMode {
    READ_WRITE, //!< Open an existing database for reading and writing.
    CREATE,     //!< Open for reading and writing, creating the database if it does not exist yet.
    READ_ONLY,  //!< Open an existing database for reading only.
};

/** Human readable name of a wallet database backend. */
std::string DatabaseFormatName(DatabaseFormat format);

/**
 * RAII class that provides access to a wallet database.
 *
 * The class only knows about serialized key/value pairs, it is agnostic of the
 * backend that actually stores them.
 */
class DatabaseBatch
{
protected:
    DatabaseBatch() = default;
    DatabaseBatch(const DatabaseBatch&) = delete;
    DatabaseBatch& operator=(const DatabaseBatch&) = delete;

    //! Read the value belonging to `key`, returns false if the key does not exist.
    virtual bool ReadKey(CDataStream&& key, CDataStream& value) = 0;
    //! Write `value` for `key`, returns false on failure.
    virtual bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) = 0;
    //! Erase `key`, returns false on failure.
    virtual bool EraseKey(CDataStream&& key) = 0;
    //! Check whether `key` exists.
    virtual bool HasKey(CDataStream&& key) = 0;

public:
    virtual ~DatabaseBatch() = default;

    /**
     * Read a raw (already serialized) key/value pair.
     *
     * This is the byte level counterpart of Read(), it is used to copy records
     * between two wallet databases without re-serializing them.
     */
    bool ReadRaw(CDataStream&& key, CDataStream& value) { return ReadKey(std::move(key), value); }

    /**
     * Write a raw (already serialized) key/value pair.
     *
     * This is the byte level counterpart of Write(), it is used to copy records
     * between two wallet databases without re-serializing them.
     */
    bool WriteRaw(CDataStream&& key, CDataStream&& value, bool overwrite = true) { return WriteKey(std::move(key), std::move(value), overwrite); }

    //! Flush the batch to disk.
    virtual void Flush() = 0;
    //! Close the batch, releasing all backend resources.
    virtual void Close() = 0;

    template <typename K, typename T>
    bool Read(const K& key, T& value)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;

        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        if (!ReadKey(std::move(ssKey), ssValue)) return false;
        try {
            ssValue >> value;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    template <typename K, typename T>
    bool Write(const K& key, const T& value, bool fOverwrite = true)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;

        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.reserve(10000);
        ssValue << value;

        return WriteKey(std::move(ssKey), std::move(ssValue), fOverwrite);
    }

    template <typename K>
    bool Erase(const K& key)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;

        return EraseKey(std::move(ssKey));
    }

    template <typename K>
    bool Exists(const K& key)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;

        return HasKey(std::move(ssKey));
    }

    //! Start a full iteration over all records of the database.
    virtual bool StartCursor() = 0;
    //! Read the next record, `complete` is set to true once the iteration is done.
    virtual bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) = 0;
    //! Stop the iteration.
    virtual void CloseCursor() = 0;

    //! Begin a transaction, all following operations are committed at once.
    virtual bool TxnBegin() = 0;
    //! Commit the current transaction.
    virtual bool TxnCommit() = 0;
    //! Abort the current transaction.
    virtual bool TxnAbort() = 0;
};

/**
 * An instance of this class represents one wallet database.
 */
class WalletDatabase
{
public:
    WalletDatabase() : nUpdateCounter(0) {}
    virtual ~WalletDatabase() = default;

    WalletDatabase(const WalletDatabase&) = delete;
    WalletDatabase& operator=(const WalletDatabase&) = delete;

    /**
     * Create a wallet database handle for the given path.
     *
     * If the path already contains a wallet file, the backend is detected from
     * the file magic. Otherwise `default_format` is used for the newly created
     * database.
     */
    static std::unique_ptr<WalletDatabase> Create(const fs::path& path, DatabaseFormat default_format = DatabaseFormat::BERKELEY);

    /** Return object for accessing a dummy database with no read/write capabilities. */
    static std::unique_ptr<WalletDatabase> CreateDummy();

    /** Return object for accessing a temporary in-memory database. */
    static std::unique_ptr<WalletDatabase> CreateMock();

    /** Make a batch connected to this database. */
    virtual std::unique_ptr<DatabaseBatch> MakeBatch(DatabaseBatchMode mode = DatabaseBatchMode::READ_WRITE, bool flush_on_close = true) = 0;

    /** Open the database if it is not already opened. */
    virtual void Open() = 0;

    /** Rewrite the entire database on disk, with the exception of key pszSkip if non-zero */
    virtual bool Rewrite(const char* pszSkip = nullptr) = 0;

    /** Back up the entire database to a file. */
    virtual bool Backup(const std::string& strDest) = 0;

    /** Make sure all changes are flushed to disk. */
    virtual void Flush(bool shutdown) = 0;

    /* Flush the wallet passively (TRY_LOCK), ideal to be called periodically. */
    virtual bool PeriodicFlush() = 0;

    /** Close the database and release all backend resources. */
    virtual void Close() = 0;

    /** Reopen the underlying database environment. */
    virtual void ReloadDbEnv() = 0;

    /** Increment the update counter (used to trigger periodic flushes). */
    void IncrementUpdateCounter() { ++nUpdateCounter; }

    /** Name of the main database file. */
    virtual std::string Filename() = 0;

    /** Name of the backend format. */
    virtual std::string Format() = 0;

    /** Directory that contains the wallet database. */
    virtual fs::path Directory() = 0;

    std::atomic<unsigned int> nUpdateCounter;
};

/** Name of the legacy Berkeley DB wallet data file inside a wallet directory. */
extern const char* const WALLET_BDB_FILENAME;
/** Name of the SQLite wallet data file inside a wallet directory. */
extern const char* const WALLET_SQLITE_FILENAME;

/** Return whether the given file is a Berkeley DB btree wallet file. */
bool IsBDBFile(const fs::path& path);

/** Return whether the given file is a SQLite wallet file. */
bool IsSQLiteFile(const fs::path& path);

/** Result of the wallet backend format detection. */
enum class WalletFormatStatus {
    OK,        //!< The format of the wallet was detected.
    NOT_FOUND, //!< No wallet file exists (yet), the default format applies.
    BAD_FORMAT, //!< A wallet file exists but its magic does not match any known backend.
    AMBIGUOUS, //!< Both a Berkeley DB and a SQLite wallet file exist in the same directory.
};

/**
 * Detect the backend format of the wallet located at `wallet_path`.
 *
 * On success `format` is set to the detected backend. If no wallet file exists
 * yet, `NOT_FOUND` is returned and `format` is left untouched.
 */
WalletFormatStatus DetectWalletFormat(const fs::path& wallet_path, DatabaseFormat& format);

/** Return whether a wallet database is currently loaded. */
bool IsWalletLoaded(const fs::path& wallet_path);

/** Return whether a SQLite wallet with the given path is currently loaded. */
bool IsSQLiteWalletLoaded(const fs::path& wallet_path);

/** Path of the SQLite data file that belongs to the given wallet path. */
fs::path SQLiteWalletFilePath(const fs::path& wallet_path);

/** Register/unregister a SQLite wallet file as being loaded. */
void RegisterSQLiteWallet(const fs::path& wallet_file);
void UnregisterSQLiteWallet(const fs::path& wallet_file);

/** Given a wallet directory path or legacy file path, return path to main data file in the wallet database. */
fs::path WalletDataFilePath(const fs::path& wallet_path);

#endif // DEFI_WALLET_DB_H
