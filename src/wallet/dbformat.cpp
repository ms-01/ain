// Copyright (c) 2026 The DeFi Blockchain developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

/**
 * Backend dependent parts of the wallet database layer.
 *
 * The concrete wallet database backends are implemented in wallet/bdb.cpp and
 * wallet/sqlite.cpp. The factories that select a backend live in this file so
 * that the (backend independent) wallet/db.h module does not have to include
 * the backend headers, which would introduce circular dependencies between the
 * wallet database modules.
 */

#include <wallet/db.h>

#include <fs.h>
#include <logging.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <wallet/bdb.h>
#include <wallet/sqlite.h>

#include <memory>

std::unique_ptr<WalletDatabase> WalletDatabase::Create(const fs::path& path, DatabaseFormat default_format)
{
    DatabaseFormat format = default_format;

    const WalletFormatStatus status = DetectWalletFormat(path, format);
    if (status == WalletFormatStatus::BAD_FORMAT) {
        throw std::runtime_error(strprintf("Wallet file %s is neither a Berkeley DB nor a SQLite wallet file", fs::PathToString(path)));
    }
    if (status == WalletFormatStatus::AMBIGUOUS) {
        throw std::runtime_error(strprintf("Wallet directory %s contains both a Berkeley DB (%s) and a SQLite (%s) wallet file, please remove the file that should not be used",
                                           fs::PathToString(path), WALLET_BDB_FILENAME, WALLET_SQLITE_FILENAME));
    }

    if (format == DatabaseFormat::BERKELEY) {
        return std::unique_ptr<WalletDatabase>(BerkeleyDatabase::Create(path));
    }

    const fs::path file_path = SQLiteWalletFilePath(path);
    return MakeSQLiteDatabase(file_path.parent_path(), file_path);
}

std::unique_ptr<WalletDatabase> WalletDatabase::CreateMock()
{
    return std::unique_ptr<WalletDatabase>(BerkeleyDatabase::CreateMock());
}

bool IsWalletLoaded(const fs::path& wallet_path)
{
    if (IsSQLiteFile(wallet_path)) {
        return IsSQLiteWalletLoaded(wallet_path);
    }
    if (fs::is_directory(wallet_path) && fs::exists(wallet_path / WALLET_SQLITE_FILENAME)) {
        return IsSQLiteWalletLoaded(wallet_path);
    }

    return IsBDBWalletLoaded(wallet_path);
}
