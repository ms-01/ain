// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_WALLET_BDB_H
#define DEFI_WALLET_BDB_H

#include <wallet/db.h>

#include <util/system.h>
#include <util/translation.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <db_cxx.h>

struct WalletDatabaseFileId {
    u_int8_t value[DB_FILE_ID_LEN];
    bool operator==(const WalletDatabaseFileId& rhs) const;
};

class BerkeleyDatabase;

class BerkeleyEnvironment
{
private:
    bool fDbEnvInit;
    bool fMockDb;
    // Don't change into fs::path, as that can result in
    // shutdown problems/crashes caused by a static initialized internal pointer.
    std::string strPath;

public:
    std::unique_ptr<DbEnv> dbenv;
    std::map<std::string, int> mapFileUseCount;
    std::map<std::string, std::reference_wrapper<BerkeleyDatabase>> m_databases;
    std::unordered_map<std::string, WalletDatabaseFileId> m_fileids;
    std::condition_variable_any m_db_in_use;

    BerkeleyEnvironment(const fs::path& env_directory);
    BerkeleyEnvironment();
    ~BerkeleyEnvironment();
    void Reset();

    bool IsMock() const { return fMockDb; }
    bool IsInitialized() const { return fDbEnvInit; }
    bool IsDatabaseLoaded(const std::string& db_filename) const { return m_databases.find(db_filename) != m_databases.end(); }
    fs::path Directory() const { return fs::PathFromString(strPath); }

    /**
     * Verify that database file strFile is OK. If it is not,
     * call the callback to try to recover.
     * This must be called BEFORE strFile is opened.
     * Returns true if strFile is OK.
     */
    enum class VerifyResult { VERIFY_OK,
                        RECOVER_OK,
                        RECOVER_FAIL };
    typedef bool (*recoverFunc_type)(const fs::path& file_path, std::string& out_backup_filename);
    VerifyResult Verify(const std::string& strFile, recoverFunc_type recoverFunc, std::string& out_backup_filename);
    /**
     * Salvage data from a file that Verify says is bad.
     * fAggressive sets the DB_AGGRESSIVE flag (see berkeley DB->verify() method documentation).
     * Appends binary key/value pairs to vResult, returns true if successful.
     * NOTE: reads the entire database into memory, so cannot be used
     * for huge databases.
     */
    typedef std::pair<std::vector<unsigned char>, std::vector<unsigned char> > KeyValPair;
    bool Salvage(const std::string& strFile, bool fAggressive, std::vector<KeyValPair>& vResult);

    bool Open(bool retry);
    void Close();
    void Flush(bool fShutdown);
    void CheckpointLSN(const std::string& strFile, bool lsnReset = true);

    void CloseDb(const std::string& strFile);
    void ReloadDbEnv();

    DbTxn* TxnBegin(int flags = DB_TXN_WRITE_NOSYNC)
    {
        DbTxn* ptxn = nullptr;
        int ret = dbenv->txn_begin(nullptr, &ptxn, flags);
        if (!ptxn || ret != 0)
            return nullptr;
        return ptxn;
    }
};

/** Get BerkeleyEnvironment and database filename given a wallet path. */
std::shared_ptr<BerkeleyEnvironment> GetWalletEnv(const fs::path& wallet_path, std::string& database_filename);

/** Return whether a Berkeley DB backed wallet is currently loaded. */
bool IsBDBWalletLoaded(const fs::path& wallet_path);

/** An instance of this class represents one database.
 * For BerkeleyDB this is just a (env, strFile) tuple.
 **/
class BerkeleyDatabase : public WalletDatabase
{
    friend class BerkeleyBatch;
public:
    /** Create dummy DB handle */
    BerkeleyDatabase() : env(nullptr)
    {
    }

    /** Create DB handle to real database */
    BerkeleyDatabase(std::shared_ptr<BerkeleyEnvironment> env, std::string filename) :
        env(std::move(env)), strFile(std::move(filename))
    {
        auto inserted = this->env->m_databases.emplace(strFile, std::ref(*this));
        assert(inserted.second);
        m_directory = this->env->Directory();
    }

    ~BerkeleyDatabase() {
        if (env) {
            size_t erased = env->m_databases.erase(strFile);
            assert(erased == 1);
        }
    }

    /** Return object for accessing database at specified path. */
    static std::unique_ptr<BerkeleyDatabase> Create(const fs::path& path)
    {
        std::string filename;
        return std::make_unique<BerkeleyDatabase>(GetWalletEnv(path, filename), std::move(filename));
    }

    /** Return object for accessing dummy database with no read/write capabilities. */
    static std::unique_ptr<BerkeleyDatabase> CreateDummy()
    {
        return std::make_unique<BerkeleyDatabase>();
    }

    /** Return object for accessing temporary in-memory database. */
    static std::unique_ptr<BerkeleyDatabase> CreateMock()
    {
        return std::make_unique<BerkeleyDatabase>(std::make_shared<BerkeleyEnvironment>(), "");
    }

    std::unique_ptr<DatabaseBatch> MakeBatch(DatabaseBatchMode mode = DatabaseBatchMode::READ_WRITE, bool flush_on_close = true) override;

    /** The Berkeley DB environment is opened lazily by the first batch. */
    void Open() override {}

    bool Rewrite(const char* pszSkip = nullptr) override;

    bool Backup(const std::string& strDest) override;

    void Flush(bool shutdown) override;

    bool PeriodicFlush() override;

    void Close() override;

    void ReloadDbEnv() override;

    std::string Filename() override { return strFile; }

    std::string Format() override { return "bdb"; }

    fs::path Directory() override;

    /**
     * Pointer to shared database environment.
     *
     * Normally there is only one BerkeleyDatabase object per
     * BerkeleyEnvivonment, but in the special, backwards compatible case where
     * multiple wallet BDB data files are loaded from the same directory, this
     * will point to a shared instance that gets freed when the last data file
     * is closed.
     */
    std::shared_ptr<BerkeleyEnvironment> env;

    /** Database pointer. This is initialized lazily and reset during flushes, so it can be null. */
    std::unique_ptr<Db> m_db;

    const std::string& GetFileName() const { return strFile; }

private:
    std::string strFile;

    /** Directory the wallet database lives in, kept separately as the environment may be released on shutdown. */
    fs::path m_directory;

    /**
     * Return whether this database handle is a dummy for testing.
     * Only to be used at a low level, application should ideally not care
     * about this.
     */
    bool IsDummy() { return env == nullptr; }
};

/** RAII class that provides access to a Berkeley database */
class BerkeleyBatch : public DatabaseBatch
{
    /** RAII class that automatically cleanses its data on destruction */
    class SafeDbt final
    {
        Dbt m_dbt;

    public:
        // construct Dbt with internally-managed data
        SafeDbt();
        // construct Dbt with provided data
        SafeDbt(void* data, size_t size);
        ~SafeDbt();

        // delegate to Dbt
        const void* get_data() const;
        u_int32_t get_size() const;

        // conversion operator to access the underlying Dbt
        operator Dbt*();
    };

protected:
    Db* pdb;
    std::string strFile;
    DbTxn* activeTxn;
    bool fReadOnly;
    bool fFlushOnClose;
    BerkeleyEnvironment *env;

    /** Cursor used by the backend agnostic iteration interface. */
    Dbc* m_cursor{nullptr};

    bool ReadKey(CDataStream&& key, CDataStream& value) override;
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override;
    bool EraseKey(CDataStream&& key) override;
    bool HasKey(CDataStream&& key) override;

public:
    explicit BerkeleyBatch(BerkeleyDatabase& database, const char* pszMode = "r+", bool fFlushOnCloseIn=true);
    ~BerkeleyBatch() override { Close(); }

    BerkeleyBatch(const BerkeleyBatch&) = delete;
    BerkeleyBatch& operator=(const BerkeleyBatch&) = delete;

    void Flush() override;
    void Close() override;
    static bool Recover(const fs::path& file_path, void *callbackDataIn, bool (*recoverKVcallback)(void* callbackData, CDataStream ssKey, CDataStream ssValue), std::string& out_backup_filename);
    /* flush the wallet passively (TRY_LOCK)
       ideal to be called periodically */
    static bool PeriodicFlush(BerkeleyDatabase& database);
    /* verifies the database environment */
    static bool VerifyEnvironment(const fs::path& file_path, std::string& errorStr);
    /* verifies the database file */
    static bool VerifyDatabaseFile(const fs::path& file_path, std::string& warningStr, std::string& errorStr, BerkeleyEnvironment::recoverFunc_type recoverFunc);

    bool StartCursor() override;
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override;
    void CloseCursor() override;
    bool TxnBegin() override;
    bool TxnCommit() override;
    bool TxnAbort() override;

    //! Low level cursor access, only used by the backend itself.
    Dbc* GetCursor();
    //! Low level cursor read, only used by the backend itself.
    int ReadAtCursorRaw(Dbc* pcursor, CDataStream& ssKey, CDataStream& ssValue);

    bool static Rewrite(BerkeleyDatabase& database, const char* pszSkip = nullptr);
};

#endif // DEFI_WALLET_BDB_H
