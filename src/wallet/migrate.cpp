// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/migrate.h>

#include <crypto/sha256.h>
#include <fs.h>
#include <logging.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <wallet/db.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace {

/**
 * Order independent digest accumulator.
 *
 * The digest of every single record is XOR'ed into the accumulator, so the
 * result only depends on the set of records and not on their order.
 */
class RecordDigest
{
private:
    unsigned char m_data[CSHA256::OUTPUT_SIZE];

public:
    RecordDigest()
    {
        memset(m_data, 0, sizeof(m_data));
    }

    void Add(const unsigned char* key, size_t key_len, const unsigned char* value, size_t value_len)
    {
        CSHA256 sha;
        sha.Write(key, key_len);
        sha.Write(value, value_len);
        unsigned char out[CSHA256::OUTPUT_SIZE];
        sha.Finalize(out);
        for (size_t i = 0; i < CSHA256::OUTPUT_SIZE; ++i) {
            m_data[i] ^= out[i];
        }
    }

    bool operator==(const RecordDigest& other) const
    {
        return memcmp(m_data, other.m_data, sizeof(m_data)) == 0;
    }
};

/** SHA256 checksum of a file, empty string if the file cannot be read. */
std::string FileChecksum(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::string();

    CSHA256 sha;
    std::vector<char> buffer(64 * 1024);
    while (file) {
        file.read(buffer.data(), buffer.size());
        const std::streamsize read = file.gcount();
        if (read > 0) {
            sha.Write(reinterpret_cast<const unsigned char*>(buffer.data()), read);
        }
    }
    unsigned char out[CSHA256::OUTPUT_SIZE];
    sha.Finalize(out);
    return HexStr(out, out + CSHA256::OUTPUT_SIZE);
}

/** Remove a partially written target wallet. */
void RemoveTarget(const fs::path& target_path, DatabaseFormat format)
{
    std::error_code ec;
    const fs::path data_file = fs::is_regular_file(target_path)
                                   ? target_path
                                   : fs::path(target_path / (format == DatabaseFormat::SQLITE ? WALLET_SQLITE_FILENAME : WALLET_BDB_FILENAME));
    fs::remove(data_file, ec);
    if (fs::is_directory(target_path)) {
        std::error_code ec2;
        // Only remove the directory if it is empty, it may contain other wallets.
        if (fs::is_empty(target_path, ec2)) {
            fs::remove(target_path, ec2);
        }
    }
}

} // namespace

bool MigrateWalletDatabase(const fs::path& source_path, const fs::path& target_path, DatabaseFormat target_format, WalletMigrationResult& result)
{
    result.source_path = fs::PathToString(source_path);
    result.target_path = fs::PathToString(target_path);
    result.target_format = target_format;

    // The target must be a new location, an existing target is never overwritten.
    if (fs::exists(target_path)) {
        result.error = strprintf("The target wallet %s already exists, please choose a different name", fs::PathToString(target_path));
        return false;
    }

    // Detect the backend of the source wallet.
    DatabaseFormat source_format = DatabaseFormat::BERKELEY;
    const WalletFormatStatus status = DetectWalletFormat(source_path, source_format);
    if (status == WalletFormatStatus::BAD_FORMAT) {
        result.error = strprintf("The wallet %s is neither a Berkeley DB nor a SQLite wallet", fs::PathToString(source_path));
        return false;
    }
    if (status == WalletFormatStatus::AMBIGUOUS) {
        result.error = strprintf("The wallet directory %s contains both a Berkeley DB and a SQLite wallet file", fs::PathToString(source_path));
        return false;
    }
    if (status == WalletFormatStatus::NOT_FOUND) {
        result.error = strprintf("The wallet %s does not exist", fs::PathToString(source_path));
        return false;
    }
    result.source_format = source_format;

    const fs::path source_data_file = WalletDataFilePath(source_path);
    const std::string checksum_before = FileChecksum(source_data_file);
    if (checksum_before.empty()) {
        result.error = strprintf("Unable to read the wallet file %s", fs::PathToString(source_data_file));
        return false;
    }

    // Open the source read-only, it is never modified by the migration.
    std::unique_ptr<WalletDatabase> source_db;
    std::unique_ptr<WalletDatabase> target_db;
    std::unique_ptr<DatabaseBatch> source_batch;
    std::unique_ptr<DatabaseBatch> target_batch;

    try {
        source_db = WalletDatabase::Create(source_path, source_format);
        source_batch = source_db->MakeBatch(DatabaseBatchMode::READ_ONLY);
    } catch (const std::exception& e) {
        result.error = strprintf("Unable to open the source wallet %s: %s", fs::PathToString(source_path), e.what());
        return false;
    }

    if (!source_batch->StartCursor()) {
        result.error = strprintf("Unable to read the records of the wallet %s", fs::PathToString(source_path));
        return false;
    }

    try {
        target_db = WalletDatabase::Create(target_path, target_format);
        target_batch = target_db->MakeBatch(DatabaseBatchMode::CREATE);
    } catch (const std::exception& e) {
        source_batch->CloseCursor();
        result.error = strprintf("Unable to create the target wallet %s: %s", fs::PathToString(target_path), e.what());
        return false;
    }

    // Copy all records inside a single transaction.
    uint64_t records = 0;
    RecordDigest source_digest;
    bool success = true;

    if (!target_batch->TxnBegin()) {
        success = false;
        result.error = "Unable to start the migration transaction";
    }

    while (success) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        bool complete = false;
        if (!source_batch->ReadAtCursor(ssKey, ssValue, complete)) {
            success = false;
            result.error = "Unable to read the next record of the source wallet";
            break;
        }
        if (complete) break;

        CDataStream key_copy(SER_DISK, CLIENT_VERSION);
        key_copy.write(ssKey.data(), ssKey.size());
        CDataStream value_copy(SER_DISK, CLIENT_VERSION);
        value_copy.write(ssValue.data(), ssValue.size());

        if (!target_batch->WriteRaw(std::move(key_copy), std::move(value_copy), true)) {
            success = false;
            result.error = "Unable to write a record into the target wallet";
            break;
        }
        source_digest.Add(reinterpret_cast<const unsigned char*>(ssKey.data()), ssKey.size(),
                          reinterpret_cast<const unsigned char*>(ssValue.data()), ssValue.size());
        ++records;
    }

    source_batch->CloseCursor();

    if (success && !target_batch->TxnCommit()) {
        success = false;
        result.error = "Unable to commit the migration transaction";
    } else if (!success) {
        target_batch->TxnAbort();
    }

    if (!success) {
        target_batch->Close();
        target_db->Close();
        RemoveTarget(target_path, target_format);
        return false;
    }

    // Verify the migration record by record.
    uint64_t verified = 0;
    RecordDigest target_digest;
    if (!target_batch->StartCursor()) {
        success = false;
        result.error = "Unable to read the records of the target wallet";
    }

    while (success) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        bool complete = false;
        if (!target_batch->ReadAtCursor(ssKey, ssValue, complete)) {
            success = false;
            result.error = "Unable to read the next record of the target wallet";
            break;
        }
        if (complete) break;

        CDataStream key_copy(SER_DISK, CLIENT_VERSION);
        key_copy.write(ssKey.data(), ssKey.size());
        CDataStream source_value(SER_DISK, CLIENT_VERSION);
        if (!source_batch->ReadRaw(std::move(key_copy), source_value)) {
            success = false;
            result.error = "A record of the target wallet is missing in the source wallet";
            break;
        }
        if (source_value.size() != ssValue.size() || memcmp(source_value.data(), ssValue.data(), ssValue.size()) != 0) {
            success = false;
            result.error = "A record of the target wallet differs from the source wallet";
            break;
        }

        target_digest.Add(reinterpret_cast<const unsigned char*>(ssKey.data()), ssKey.size(),
                          reinterpret_cast<const unsigned char*>(ssValue.data()), ssValue.size());
        ++verified;
    }

    target_batch->CloseCursor();

    if (success && verified != records) {
        success = false;
        result.error = strprintf("The target wallet contains %u records, expected %u", verified, records);
    }
    if (success && !(target_digest == source_digest)) {
        success = false;
        result.error = "The digest of the migrated records does not match the digest of the source records";
    }

    // The source wallet must not have been modified by the migration.
    const std::string checksum_after = FileChecksum(source_data_file);
    if (success && checksum_after != checksum_before) {
        success = false;
        result.error = "The source wallet was modified during the migration";
    }

    target_batch->Close();
    target_db->Close();

    if (!success) {
        RemoveTarget(target_path, target_format);
        return false;
    }

    result.records = records;
    result.success = true;
    LogPrintf("Migrated %u records from %s to %s\n", records, fs::PathToString(source_path), fs::PathToString(target_path));
    return true;
}
