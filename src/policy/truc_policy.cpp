// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/truc_policy.h>

#include <tinyformat.h>

#include <algorithm>
#include <vector>

namespace {

/** Return deduplicated direct in-mempool parents of ptx. */
CTxMemPool::setEntries FindMempoolParents(const CTxMemPool &pool, const CTransactionRef &ptx)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    CTxMemPool::setEntries parents;
    for (const CTxIn &txin : ptx->vin) {
        auto it = pool.mapTx.find(txin.prevout.hash);
        if (it != pool.mapTx.end()) {
            parents.insert(it);
        }
    }
    return parents;
}

/** Return indices of transactions within package that are direct parents of ptx. */
std::vector<size_t> FindInPackageParents(const Package &package, const CTransactionRef &ptx)
{
    std::vector<size_t> in_package_parents;

    std::set<uint256> possible_parents;
    for (const auto &input : ptx->vin) {
        possible_parents.insert(input.prevout.hash);
    }

    for (size_t i{0}; i < package.size(); ++i) {
        const auto &tx = package.at(i);
        // Package is sorted, so no parent can appear at or after ptx.
        if (tx.get() == ptx.get()) break;
        if (possible_parents.count(tx->GetHash()) > 0) {
            in_package_parents.push_back(i);
        }
    }
    return in_package_parents;
}

struct ParentInfo {
    const uint256 &m_txid;
    const uint256 &m_wtxid;
    decltype(CTransaction::nVersion) m_version;
    bool m_has_mempool_descendant;

    ParentInfo(const uint256 &txid,
               const uint256 &wtxid,
               decltype(CTransaction::nVersion) version,
               bool has_mempool_descendant)
        : m_txid{txid},
          m_wtxid{wtxid},
          m_version{version},
          m_has_mempool_descendant{has_mempool_descendant}
    {}
};

} // namespace

std::optional<std::string> PackageTRUCChecks(
    const CTxMemPool &pool,
    const CTransactionRef &ptx,
    int64_t vsize,
    const Package &package)
{
    AssertLockHeld(pool.cs);
    static_assert(TRUC_ANCESTOR_LIMIT == 2);
    static_assert(TRUC_DESCENDANT_LIMIT == 2);

    const auto mempool_parents = FindMempoolParents(pool, ptx);
    const auto in_package_parents = FindInPackageParents(package, ptx);

    if (ptx->nVersion == TRUC_VERSION) {
        if (vsize > TRUC_MAX_VSIZE) {
            return strprintf("version=3 tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
                             ptx->GetHash().ToString(),
                             ptx->GetWitnessHash().ToString(),
                             vsize,
                             TRUC_MAX_VSIZE);
        }

        if (mempool_parents.size() + in_package_parents.size() + 1 > TRUC_ANCESTOR_LIMIT) {
            return strprintf("tx %s (wtxid=%s) would have too many ancestors",
                             ptx->GetHash().ToString(),
                             ptx->GetWitnessHash().ToString());
        }

        if (!mempool_parents.empty()) {
            if ((*mempool_parents.begin())->GetCountWithAncestors() + in_package_parents.size() + 1 >
                TRUC_ANCESTOR_LIMIT) {
                return strprintf("tx %s (wtxid=%s) would have too many ancestors",
                                 ptx->GetHash().ToString(),
                                 ptx->GetWitnessHash().ToString());
            }
        }

        const bool has_parent{mempool_parents.size() + in_package_parents.size() > 0};
        if (has_parent) {
            if (vsize > TRUC_CHILD_MAX_VSIZE) {
                return strprintf("version=3 child tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
                                 ptx->GetHash().ToString(),
                                 ptx->GetWitnessHash().ToString(),
                                 vsize,
                                 TRUC_CHILD_MAX_VSIZE);
            }

            const auto parent_info = [&]() -> ParentInfo {
                if (!mempool_parents.empty()) {
                    const auto parent_it = *mempool_parents.begin();
                    return ParentInfo{parent_it->GetTx().GetHash(),
                                      parent_it->GetTx().GetWitnessHash(),
                                      parent_it->GetTx().nVersion,
                                      parent_it->GetCountWithDescendants() > 1};
                }
                const auto &package_parent = package.at(in_package_parents.front());
                return ParentInfo{package_parent->GetHash(),
                                  package_parent->GetWitnessHash(),
                                  package_parent->nVersion,
                                  false};
            }();

            if (parent_info.m_version != TRUC_VERSION) {
                return strprintf("version=3 tx %s (wtxid=%s) cannot spend from non-version=3 tx %s (wtxid=%s)",
                                 ptx->GetHash().ToString(),
                                 ptx->GetWitnessHash().ToString(),
                                 parent_info.m_txid.ToString(),
                                 parent_info.m_wtxid.ToString());
            }

            for (const auto &package_tx : package) {
                if (package_tx.get() == ptx.get()) continue;

                for (const auto &input : package_tx->vin) {
                    if (input.prevout.hash == parent_info.m_txid) {
                        return strprintf("tx %s (wtxid=%s) would exceed descendant count limit",
                                         parent_info.m_txid.ToString(),
                                         parent_info.m_wtxid.ToString());
                    }
                    if (input.prevout.hash == ptx->GetHash()) {
                        return strprintf("tx %s (wtxid=%s) would have too many ancestors",
                                         package_tx->GetHash().ToString(),
                                         package_tx->GetWitnessHash().ToString());
                    }
                }
            }

            if (parent_info.m_has_mempool_descendant) {
                return strprintf("tx %s (wtxid=%s) would exceed descendant count limit",
                                 parent_info.m_txid.ToString(),
                                 parent_info.m_wtxid.ToString());
            }
        }
    } else {
        for (const auto &parent_it : mempool_parents) {
            if (parent_it->GetTx().nVersion == TRUC_VERSION) {
                return strprintf("non-version=3 tx %s (wtxid=%s) cannot spend from version=3 tx %s (wtxid=%s)",
                                 ptx->GetHash().ToString(),
                                 ptx->GetWitnessHash().ToString(),
                                 parent_it->GetTx().GetHash().ToString(),
                                 parent_it->GetTx().GetWitnessHash().ToString());
            }
        }
        for (const auto index : in_package_parents) {
            const auto &parent_tx = package.at(index);
            if (parent_tx->nVersion == TRUC_VERSION) {
                return strprintf("non-version=3 tx %s (wtxid=%s) cannot spend from version=3 tx %s (wtxid=%s)",
                                 ptx->GetHash().ToString(),
                                 ptx->GetWitnessHash().ToString(),
                                 parent_tx->GetHash().ToString(),
                                 parent_tx->GetWitnessHash().ToString());
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, CTransactionRef>> SingleTRUCChecks(
    const CTxMemPool &pool,
    const CTransactionRef &ptx,
    const std::set<uint256> &direct_conflicts,
    int64_t vsize)
{
    AssertLockHeld(pool.cs);
    static_assert(TRUC_ANCESTOR_LIMIT == 2);
    static_assert(TRUC_DESCENDANT_LIMIT == 2);

    const auto mempool_parents = FindMempoolParents(pool, ptx);

    // Check TRUC and non-TRUC inheritance.
    for (const auto &parent_it : mempool_parents) {
        if (ptx->nVersion != TRUC_VERSION && parent_it->GetTx().nVersion == TRUC_VERSION) {
            return std::make_pair(
                strprintf("non-version=3 tx %s (wtxid=%s) cannot spend from version=3 tx %s (wtxid=%s)",
                          ptx->GetHash().ToString(),
                          ptx->GetWitnessHash().ToString(),
                          parent_it->GetTx().GetHash().ToString(),
                          parent_it->GetTx().GetWitnessHash().ToString()),
                nullptr);
        }
        if (ptx->nVersion == TRUC_VERSION && parent_it->GetTx().nVersion != TRUC_VERSION) {
            return std::make_pair(
                strprintf("version=3 tx %s (wtxid=%s) cannot spend from non-version=3 tx %s (wtxid=%s)",
                          ptx->GetHash().ToString(),
                          ptx->GetWitnessHash().ToString(),
                          parent_it->GetTx().GetHash().ToString(),
                          parent_it->GetTx().GetWitnessHash().ToString()),
                nullptr);
        }
    }

    if (ptx->nVersion != TRUC_VERSION) return std::nullopt;

    if (vsize > TRUC_MAX_VSIZE) {
        return std::make_pair(
            strprintf("version=3 tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
                      ptx->GetHash().ToString(),
                      ptx->GetWitnessHash().ToString(),
                      vsize,
                      TRUC_MAX_VSIZE),
            nullptr);
    }

    if (mempool_parents.size() + 1 > TRUC_ANCESTOR_LIMIT) {
        return std::make_pair(
            strprintf("tx %s (wtxid=%s) would have too many ancestors",
                      ptx->GetHash().ToString(),
                      ptx->GetWitnessHash().ToString()),
            nullptr);
    }

    if (!mempool_parents.empty()) {
        const auto parent_it = *mempool_parents.begin();
        if (parent_it->GetCountWithAncestors() + 1 > TRUC_ANCESTOR_LIMIT) {
            return std::make_pair(
                strprintf("tx %s (wtxid=%s) would have too many ancestors",
                          ptx->GetHash().ToString(),
                          ptx->GetWitnessHash().ToString()),
                nullptr);
        }
        if (vsize > TRUC_CHILD_MAX_VSIZE) {
            return std::make_pair(
                strprintf("version=3 child tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
                          ptx->GetHash().ToString(),
                          ptx->GetWitnessHash().ToString(),
                          vsize,
                          TRUC_CHILD_MAX_VSIZE),
                nullptr);
        }

        CTxMemPool::setEntries descendants;
        pool.CalculateDescendants(parent_it, descendants);
        descendants.erase(parent_it);

        const bool child_will_be_replaced =
            !descendants.empty() &&
            std::any_of(descendants.cbegin(), descendants.cend(), [&direct_conflicts](const CTxMemPool::txiter &child) {
                return direct_conflicts.count(child->GetTx().GetHash()) > 0;
            });

        if (parent_it->GetCountWithDescendants() + 1 > TRUC_DESCENDANT_LIMIT && !child_will_be_replaced) {
            const bool consider_sibling_eviction =
                !descendants.empty() &&
                parent_it->GetCountWithDescendants() == 2 &&
                (*descendants.begin())->GetCountWithAncestors() == 2;
            return std::make_pair(
                strprintf("tx %s (wtxid=%s) would exceed descendant count limit",
                          parent_it->GetTx().GetHash().ToString(),
                          parent_it->GetTx().GetWitnessHash().ToString()),
                consider_sibling_eviction ? (*descendants.begin())->GetSharedTx() : nullptr);
        }
    }

    return std::nullopt;
}
