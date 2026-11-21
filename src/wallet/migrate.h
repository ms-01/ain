// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_WALLET_MIGRATE_H
#define DEFI_WALLET_MIGRATE_H

#include <fs.h>
#include <wallet/db.h>

#include <cstdint>
#include <string>

/** Result of a wallet database migration. */
struct WalletMigrationResult {
    //! Whether the migration completed successfully and was verified.
    bool success{false};
    //! Number of records that were copied.
    uint64_t records{0};
    //! Backend of the source wallet.
    DatabaseFormat source_format{DatabaseFormat::BERKELEY};
    //! Backend of the newly created wallet.
    DatabaseFormat target_format{DatabaseFormat::SQLITE};
    //! Path of the source wallet.
    std::string source_path;
    //! Path of the target wallet.
    std::string target_path;
    //! Error message, only set if the migration failed.
    std::string error;
};

/**
 * Migrate the wallet database at `source_path` into a new wallet database at
 * `target_path`.
 *
 * The source database is opened read-only and is never modified, the target is
 * always written to a new location and an already existing target is refused.
 * All records are copied inside a single transaction and the result is verified
 * record by record (and by a digest over all records) before the migration is
 * reported as successful. If any of the checks fail, the partially written
 * target is removed again.
 *
 * @param[out] result Details about the migration.
 * @return true if the migration was successful.
 */
bool MigrateWalletDatabase(const fs::path& source_path, const fs::path& target_path, DatabaseFormat target_format, WalletMigrationResult& result);

#endif // DEFI_WALLET_MIGRATE_H
