// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_POLICY_TRUC_POLICY_H
#define DEFI_POLICY_TRUC_POLICY_H

#include <consensus/consensus.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <txmempool.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>

// BIP 431 Topologically Restricted Until Confirmation (TRUC) transaction policy.
// A transaction with nVersion == 3 is treated as TRUC.
inline constexpr decltype(CTransaction::nVersion) TRUC_VERSION{3};

// TRUC only allows 1 parent and 1 child when unconfirmed (ancestor/descendant set size <= 2).
/** Maximum number of transactions including an unconfirmed TRUC tx and its descendants. */
inline constexpr unsigned int TRUC_DESCENDANT_LIMIT{2};
/** Maximum number of transactions including a TRUC tx and all its mempool ancestors. */
inline constexpr unsigned int TRUC_ANCESTOR_LIMIT{2};

/** Maximum sigop-adjusted virtual size of all v3 transactions. */
inline constexpr int64_t TRUC_MAX_VSIZE{10000};
inline constexpr int64_t TRUC_MAX_WEIGHT{TRUC_MAX_VSIZE * WITNESS_SCALE_FACTOR};
/** Maximum sigop-adjusted virtual size of a tx which spends from an unconfirmed TRUC transaction. */
inline constexpr int64_t TRUC_CHILD_MAX_VSIZE{1000};
inline constexpr int64_t TRUC_CHILD_MAX_WEIGHT{TRUC_CHILD_MAX_VSIZE * WITNESS_SCALE_FACTOR};

inline bool IsTrucTx(const CTransaction &tx)
{
    return tx.nVersion == TRUC_VERSION;
}

/**
 * Enforce BIP 431 TRUC rules for a single transaction being validated against the mempool.
 * Must be called for every transaction (even non-TRUC, to prevent spending unconfirmed TRUC parents).
 *
 * @returns
 * - std::nullopt if all TRUC checks pass.
 * - {debug_string, sibling_tx} if this transaction would be a second child of an unconfirmed TRUC
 *   parent and sibling eviction can be considered.
 * - {debug_string, nullptr} if this transaction violates a TRUC rule and sibling eviction does not apply.
 */
std::optional<std::pair<std::string, CTransactionRef>> SingleTRUCChecks(
    const CTxMemPool &pool,
    const CTransactionRef &ptx,
    const std::set<uint256> &direct_conflicts,
    int64_t vsize) EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/**
 * Enforce BIP 431 TRUC rules for a transaction submitted within a package.
 * Must be called for every transaction in the package (even non-TRUC).
 *
 * @returns debug string if a TRUC violation occurs, std::nullopt otherwise.
 */
std::optional<std::string> PackageTRUCChecks(
    const CTxMemPool &pool,
    const CTransactionRef &ptx,
    int64_t vsize,
    const Package &package) EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

#endif // DEFI_POLICY_TRUC_POLICY_H
