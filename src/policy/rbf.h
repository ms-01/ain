// Copyright (c) 2016-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_POLICY_RBF_H
#define DEFI_POLICY_RBF_H

#include <amount.h>
#include <primitives/transaction.h>
#include <threadsafety.h>
#include <txmempool.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>

class CFeeRate;
class uint256;

/** Maximum number of transactions that can be replaced by RBF (BIP 125 rule #5).
 * This includes all mempool conflicts and their descendants. */
static constexpr uint32_t MAX_REPLACEMENT_CANDIDATES{100};

enum class RBFTransactionState {
    UNKNOWN,
    REPLACEABLE_BIP125,
    FINAL
};

// Determine whether an in-mempool transaction is signaling opt-in to RBF
// according to BIP 125
// This involves checking sequence numbers of the transaction, as well
// as the sequence numbers of all in-mempool ancestors.
RBFTransactionState IsRBFOptIn(const CTransaction& tx, const CTxMemPool& pool) EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/** Get all descendants of iters_conflicting. Checks that there are no more than
 * MAX_REPLACEMENT_CANDIDATES potential entries. May overestimate if the entries in
 * iters_conflicting have overlapping descendants.
 * @param[in]   tx                 The replacement transaction.
 * @param[in]   pool               The mempool holding the conflicting entries.
 * @param[in]   iters_conflicting   The set of iterators to mempool entries.
 * @param[out]  all_conflicts       Populated with all the mempool entries that would be replaced,
 *                                  which includes iters_conflicting and all entries' descendants.
 *                                  Not cleared at the start; any existing mempool entries will
 *                                  remain in the set.
 * @returns an error message if MAX_REPLACEMENT_CANDIDATES may be exceeded, otherwise a std::nullopt.
 */
std::optional<std::string> GetEntriesForConflicts(const CTransaction& tx, CTxMemPool& pool,
                                                  const CTxMemPool::setEntries& iters_conflicting,
                                                  CTxMemPool::setEntries& all_conflicts)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/** The replacement transaction may only include an unconfirmed input if that input was included in
 * one of the original transactions (BIP 125 rule #2).
 * @returns error message if tx spends unconfirmed inputs not also spent by iters_conflicting,
 * otherwise std::nullopt. */
std::optional<std::string> HasNoNewUnconfirmed(const CTransaction& tx, const CTxMemPool& pool,
                                               const CTxMemPool::setEntries& iters_conflicting)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/** Check the intersection between two sets of transactions (a set of mempool entries and a set of
 * txids) to make sure they are disjoint.
 * @param[in]   ancestors           Set of mempool entries corresponding to ancestors of the
 *                                  replacement transactions.
 * @param[in]   direct_conflicts    Set of txids corresponding to the mempool conflicts
 *                                  (candidates to be replaced).
 * @param[in]   txid                Transaction ID, included in the error message if violation occurs.
 * @returns error message if the sets intersect, std::nullopt if they are disjoint.
 */
std::optional<std::string> EntriesAndTxidsDisjoint(const CTxMemPool::setEntries& ancestors,
                                                    const std::set<uint256> &direct_conflicts,
                                                    const uint256 &txid);

/** Check that the feerate of the replacement transaction is higher than the feerate of each
 * of the transactions in iters_conflicting (BIP 125 rule #6).
 * @param[in]   iters_conflicting  The set of mempool entries.
 * @param[in]   replacement_feerate The feerate of the replacement transaction.
 * @param[in]   txid               Transaction ID, included in the error message if violation occurs.
 * @returns error message if fees insufficient, otherwise std::nullopt.
 */
std::optional<std::string> PaysMoreThanConflicts(const CTxMemPool::setEntries &iters_conflicting,
                                                  CFeeRate replacement_feerate,
                                                  const uint256 &txid);

/** The replacement transaction must pay more fees than the original transactions (BIP 125 rule #3).
 * The additional fees must pay for the replacement's bandwidth at or above the incremental relay
 * feerate (BIP 125 rule #4).
 * @param[in]   original_fees       Total modified fees of original transaction(s).
 * @param[in]   replacement_fees    Total modified fees of replacement transaction(s).
 * @param[in]   replacement_vsize   Total size of replacement transaction(s).
 * @param[in]   relay_fee           The node's minimum feerate for transaction relay.
 * @param[in]   txid                Transaction ID, included in the error message if violation occurs.
 * @returns error string if fees are insufficient, otherwise std::nullopt.
 */
std::optional<std::string> PaysForRBF(CAmount original_fees,
                                       CAmount replacement_fees,
                                       size_t replacement_vsize,
                                       CFeeRate relay_fee,
                                       const uint256 &txid);

#endif // DEFI_POLICY_RBF_H
