// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <txmempool.h>
#include <uint256.h>
#include <validation.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <numeric>
#include <unordered_set>

namespace {
// A package must fit into the mempool limits, otherwise the package could never be
// accepted. The chain uses the very same default limits as upstream Bitcoin Core
// (see `validation.h`), so the upstream static assertions hold unchanged. They are
// evaluated here (instead of in the header) to avoid a circular include of
// `validation.h` into `policy/packages.h`.
static_assert(MAX_PACKAGE_SIZE * WITNESS_SCALE_FACTOR * 1000 >= MAX_STANDARD_TX_WEIGHT);
static_assert(DEFAULT_ANCESTOR_LIMIT >= MAX_PACKAGE_COUNT);
static_assert(DEFAULT_DESCENDANT_LIMIT >= MAX_PACKAGE_COUNT);
static_assert(DEFAULT_ANCESTOR_SIZE_LIMIT >= MAX_PACKAGE_SIZE);
static_assert(DEFAULT_DESCENDANT_SIZE_LIMIT >= MAX_PACKAGE_SIZE);
} // namespace

bool CheckPackage(const Package &txns, PackageValidationState &state)
{
    const unsigned int package_count = txns.size();

    if (package_count > MAX_PACKAGE_COUNT) {
        return state.Invalid(PackageValidationResult::PCKG_POLICY, "package-too-many-transactions");
    }

    const int64_t total_size = std::accumulate(txns.cbegin(), txns.cend(), 0,
                               [](int64_t sum, const auto &tx) { return sum + GetVirtualTransactionSize(*tx); });
    // If the package only contains 1 tx, it's better to report the policy violation on individual tx size.
    if (package_count > 1 && total_size > MAX_PACKAGE_SIZE * 1000) {
        return state.Invalid(PackageValidationResult::PCKG_POLICY, "package-too-large");
    }

    // Require the package to be sorted in order of dependency, i.e. parents appear before children.
    // An unsorted package will fail anyway on missing-inputs, but it's better to quit earlier and
    // fail on something less ambiguous (missing-inputs could also be an orphan or trying to
    // spend nonexistent coins).
    std::unordered_set<uint256, SaltedTxidHasher> later_txids;
    std::transform(txns.cbegin(), txns.cend(), std::inserter(later_txids, later_txids.end()),
                   [](const auto &tx) { return tx->GetHash(); });
    for (const auto &tx : txns) {
        for (const auto &input : tx->vin) {
            if (later_txids.find(input.prevout.hash) != later_txids.end()) {
                // The parent is a subsequent transaction in the package.
                return state.Invalid(PackageValidationResult::PCKG_POLICY, "package-not-sorted");
            }
        }
        later_txids.erase(tx->GetHash());
    }

    // Don't allow any conflicting transactions, i.e. spending the same inputs, in a package.
    std::unordered_set<COutPoint, SaltedOutpointHasher> inputs_seen;
    for (const auto &tx : txns) {
        for (const auto &input : tx->vin) {
            if (inputs_seen.find(input.prevout) != inputs_seen.end()) {
                // This input is also present in another tx in the package.
                return state.Invalid(PackageValidationResult::PCKG_POLICY, "conflict-in-package");
            }
        }
        // Batch-add all the inputs for a tx at a time. If we added them 1 at a time, we could
        // catch duplicate inputs within a single tx.  This is a more severe, consensus error,
        // and we want to report that from CheckTransaction instead.
        std::transform(tx->vin.cbegin(), tx->vin.cend(), std::inserter(inputs_seen, inputs_seen.end()),
                       [](const auto &input) { return input.prevout; });
    }
    return true;
}

bool IsChildWithParents(const Package &package)
{
    assert(std::all_of(package.cbegin(), package.cend(), [](const auto &tx) { return tx != nullptr; }));
    if (package.size() < 2) return false;

    // The package is expected to be sorted, so the last transaction is the child.
    const auto &child = package.back();
    std::unordered_set<uint256, SaltedTxidHasher> input_txids;
    std::transform(child->vin.cbegin(), child->vin.cend(),
                   std::inserter(input_txids, input_txids.end()),
                   [](const auto &input) { return input.prevout.hash; });

    // Every transaction must be a parent of the last transaction in the package.
    return std::all_of(package.cbegin(), package.cend() - 1,
                       [&input_txids](const auto &ptx) { return input_txids.count(ptx->GetHash()) > 0; });
}

uint256 GetPackageHash(const Package &transactions)
{
    // Create a vector of the wtxids.
    std::vector<uint256> wtxids_copy;
    wtxids_copy.reserve(transactions.size());
    std::transform(transactions.cbegin(), transactions.cend(), std::back_inserter(wtxids_copy),
                   [](const auto &tx) { return tx->GetWitnessHash(); });

    // Sort in ascending order (reverse byte order, matching upstream uint256 comparison).
    std::sort(wtxids_copy.begin(), wtxids_copy.end(), [](const uint256 &lhs, const uint256 &rhs) {
        return std::lexicographical_compare(std::make_reverse_iterator(lhs.end()), std::make_reverse_iterator(lhs.begin()),
                                            std::make_reverse_iterator(rhs.end()), std::make_reverse_iterator(rhs.begin()));
    });

    // Get SHA256 hash of the wtxids concatenated in this order.
    CSHA256 hasher;
    for (const auto &wtxid : wtxids_copy) {
        hasher.Write(wtxid.begin(), wtxid.size());
    }
    uint256 result;
    hasher.Finalize(result.begin());
    return result;
}
