// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_MEMPOOL_ACCEPT_H
#define DEFI_MEMPOOL_ACCEPT_H

#include <amount.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <txmempool.h>
#include <uint256.h>

#include <list>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class BlockContext;
class CChainParams;
class CCoinsView;
class CCoinsViewCache;
class CCoinsViewMemPool;
class CCustomCSView;
class TransactionContext;
struct PrecomputedTransactionData;

/**
 * Returns true if the exact interpretation of the governance attribute
 * `v0/evm/block/rbf_increment_fee_pct` (a fraction of 100% scaled by 1e8) is used at the given
 * height, i.e. from the `DF25Height` activation height on. Below the activation height the
 * legacy whole-percent interpretation is used, which truncates the governance value to whole
 * percentage points.
 *
 * This is a mempool policy rule only: it does not affect block validity or state transitions.
 */
bool UseExactRbfFeeIncrement(const uint32_t height, const Consensus::Params &consensus);

/**
 * MemPoolAccept
 *
 * Acceptance of a single transaction into the mempool, split into the
 * following phases:
 *
 *  - PreChecks():          cheap policy checks (standardness, inputs, fees,
 *                          ancestor limits, DFI custom transaction handling).
 *  - ReplacementChecks():  BIP 125 replacement (RBF) rules.
 *  - PolicyScriptChecks(): script verification with the policy script flags.
 *  - ConsensusScriptChecks(): script verification against the current block
 *                          tip's script flags.
 *  - EvmChecks():          EVM / cross boundary specific sender, nonce and fee
 *                          checks.
 *  - Finalize():           removal of the conflicting transactions and insertion
 *                          of the accepted transaction into the mempool.
 *
 * The class is a behaviour preserving extraction of the former monolithic
 * AcceptToMemoryPoolWorker(). Every check keeps its original relative order,
 * its original ValidationInvalidReason and its original reject reason, so
 * that the RPC output (e.g. testmempoolaccept) stays unchanged.
 */
class MemPoolAccept
{
public:
    // We put the arguments we're handed into a struct, so we can pass them
    // around easier.
    struct ATMPArgs {
        const CChainParams &chainparams;
        CValidationState &state;
        bool *pfMissingInputs;
        const int64_t accept_time;
        std::list<CTransactionRef> *plTxnReplaced;
        const bool bypass_limits;
        const CAmount &nAbsurdFee;
        /**
         * Return any outpoints which were not previously present in the coins
         * cache, but were added as a result of validating the tx for mempool
         * acceptance. This allows the caller to optionally remove the cache
         * additions if the associated transaction ends up being rejected by
         * the mempool.
         */
        std::vector<COutPoint> &coins_to_uncache;
        const bool test_accept;
    };

    MemPoolAccept(CTxMemPool &pool, const CTransactionRef &ptx);

    // Single transaction acceptance
    bool AcceptSingleTransaction(ATMPArgs &args) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

private:
    // All the intermediate state that gets passed between the various levels
    // of checking a given transaction.
    struct Workspace {
        Workspace(const CTransactionRef &ptx) : ptx(ptx), hash(ptx->GetHash()) {}

        // Transactions in the mempool that conflict with the transaction
        // being accepted (i.e. that would be replaced by it).
        std::set<uint256> setConflicts;
        // All the mempool transactions that would be evicted by the
        // replacement (the conflicts and all of their descendants).
        CTxMemPool::setEntries allConflicting;
        // The in-mempool ancestors of the transaction being accepted.
        CTxMemPool::setEntries setAncestors;
        // The mempool entry of the transaction being accepted.
        std::unique_ptr<CTxMemPoolEntry> entry;

        bool fReplacementTransaction{false};
        bool fEvmReplacement{false};
        // True if the transaction is an EVM transaction (checked by the
        // transaction structure itself, without parsing the metadata).
        bool isEVMTx{false};
        CAmount nModifiedFees{0};
        CAmount nConflictingFees{0};
        size_t nConflictingSize{0};
        size_t nSize{0};
        // The EVM sender of the transaction, if any.
        std::optional<EvmAddressData> ethSender{std::nullopt};

        const CTransactionRef &ptx;
        const uint256 &hash;
    };

    // Run the policy checks on a given transaction, excluding any script checks.
    // Looks up inputs, calculates feerate, considers replacement, evaluates
    // package limits, etc. As this function can be invoked for "free" by a peer,
    // only tests that are fast should be done here (to avoid CPU DoS).
    bool PreChecks(ATMPArgs &args, Workspace &ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Check the BIP 125 replacement rules for a transaction that conflicts
    // with transactions already in the mempool.
    bool ReplacementChecks(ATMPArgs &args, Workspace &ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Run the script checks using our policy flags. As this can be slow, we should
    // only invoke this on transactions that have otherwise passed policy checks.
    bool PolicyScriptChecks(ATMPArgs &args, Workspace &ws, PrecomputedTransactionData &txdata) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Re-run the script checks, using the flags of the current block tip. This
    // should be done after PolicyScriptChecks(). This requires that all inputs
    // either be in our utxo set or in the mempool.
    bool ConsensusScriptChecks(ATMPArgs &args, Workspace &ws, PrecomputedTransactionData &txdata) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Run the EVM (cross boundary) specific checks. These have to be executed
    // after the script checks and before the transaction is added to the
    // mempool.
    bool EvmChecks(ATMPArgs &args, Workspace &ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Try to add the transaction to the mempool, removing any conflicts first.
    // Returns true if the transaction is in the mempool after any size
    // limiting is performed, false otherwise.
    bool Finalize(ATMPArgs &args, Workspace &ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    CTxMemPool &m_pool;
    const CTransactionRef m_ptx;

    // The coins view used for the input lookups. It is switched from the
    // mempool backend to the dummy backend once all inputs are cached.
    CCoinsView m_dummy;
    CCoinsViewCache m_view;
    CCoinsViewMemPool m_viewmempool;

    // DFI specific state, kept alive for the whole acceptance.
    std::unique_ptr<CCustomCSView> m_mnview;
    std::unique_ptr<BlockContext> m_blockCtx;
    std::unique_ptr<TransactionContext> m_txCtx;
};

#endif // DEFI_MEMPOOL_ACCEPT_H
