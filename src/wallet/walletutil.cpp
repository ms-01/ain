// Copyright (c) 2017-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/walletutil.h>

#include <logging.h>
#include <util/system.h>
#include <wallet/db.h>

#include <exception>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

fs::path GetWalletDir()
{
    fs::path path;

    if (gArgs.IsArgSet("-walletdir")) {
        path = gArgs.GetPathArg("-walletdir");
        if (!fs::is_directory(path)) {
            // If the path specified doesn't exist, we return the deliberately
            // invalid empty string.
            path = "";
        }
    } else {
        path = GetDataDir();
        // If a wallets directory exists, use that, otherwise default to GetDataDir
        if (fs::is_directory(path / "wallets")) {
            path /= "wallets";
        }
    }

    return path;
}

std::vector<fs::path> ListWalletDir()
{
    const fs::path wallet_dir = GetWalletDir();
    const size_t offset = wallet_dir.native().size() + 1;
    std::vector<fs::path> paths;
    std::set<fs::path> seen;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(wallet_dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            LogPrintf("%s: %s %s\n", __func__, ec.message(), fs::PathToString(it->path()));
            continue;
        }

        // Get wallet path relative to walletdir by removing walletdir from the wallet path.
        // This can be replaced by boost::filesystem::lexically_relative once boost is bumped to 1.60.
        const auto path_str = it->path().native().substr(offset);
        const fs::path path{path_str.begin(), path_str.end()};

        if (it->status().type() == fs::file_type::directory) {
            // Found a directory which contains a wallet file (Berkeley DB or SQLite), add it as a wallet.
            if (IsBDBFile(it->path() / "wallet.dat") || IsSQLiteFile(it->path() / "wallet.sqlite")) {
                if (seen.insert(path).second) paths.emplace_back(path);
            }
        } else if (it.depth() == 0 && it->symlink_status().type() == fs::file_type::regular) {
            const bool is_bdb = IsBDBFile(it->path());
            const bool is_sqlite = IsSQLiteFile(it->path());
            if (!is_bdb && !is_sqlite) continue;

            const std::string filename = it->path().filename().string();
            if (filename == "wallet.dat" || filename == "wallet.sqlite") {
                // Found top-level wallet file, add top level directory "" as a wallet.
                if (seen.insert(fs::path()).second) paths.emplace_back();
            } else {
                // Found top-level wallet file not called wallet.dat/wallet.sqlite. Current defi
                // software will never create these files but will allow them to be
                // opened in a shared database environment for backwards compatibility.
                // Add it to the list of available wallets.
                if (seen.insert(path).second) paths.emplace_back(path);
            }
        }
    }

    return paths;
}

WalletLocation::WalletLocation(const std::string& name)
    : m_name(name)
    , m_path(fs::absolute(name.empty() ? GetWalletDir() : fs::path(GetWalletDir() / fs::PathFromString(name))))
{}

bool WalletLocation::Exists() const
{
    return fs::symlink_status(m_path).type() != fs::file_type::not_found;
}
