// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <mempool_accept.h>

#include <ain_rs_exports.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <dfi/customtx.h>
#include <dfi/govvariables/attributes.h>
#include <dfi/mn_checks.h>
#include <logging.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <policy/settings.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/validation.h>
#include <validation.h>
#include <validationinterface.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

// Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
// were somehow broken and returning the wrong scriptPubKeys
static bool CheckInputsFromMempoolAndCache(const CTransaction &tx,
                                           CValidationState &state,
                                           const CCoinsViewCache &view,
                                           const CTxMemPool &pool,
                                           unsigned int flags,
                                           bool cacheSigStore,
                                           PrecomputedTransactionData &txdata) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    AssertLockHeld(cs_main);

    // pool.cs should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view
    // and when we actually call through to CheckInputs
    LOCK(pool.cs);

    assert(!tx.IsCoinBase());
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);

        // At this point we haven't actually checked if the coins are all
        // available (or shouldn't assume we have, since CheckInputs does).
        // So we just return failure if the inputs are not available here,
        // and then only have to check equivalence for available inputs.
        if (coin.IsSpent()) {
            return false;
        }

        const CTransactionRef &txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->vout.size() > txin.prevout.n);
            assert(txFrom->vout[txin.prevout.n] == coin.out);
        } else {
            const Coin &coinFromDisk = ::ChainstateActive().CoinsTip().AccessCoin(txin.prevout);
            assert(!coinFromDisk.IsSpent());
            assert(coinFromDisk.out == coin.out);
        }
    }

    return CheckInputs(tx, state, view, true, flags, cacheSigStore, true, txdata);
}

static bool IsCurrentForFeeEstimation() EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    AssertLockHeld(cs_main);
    if (::ChainstateActive().IsInitialBlockDownload()) {
        return false;
    }
    if (::ChainActive().Tip()->GetBlockTime() < (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE)) {
        return false;
    }
    if (::ChainActive().Height() < pindexBestHeader->nHeight - 1) {
        return false;
    }
    return true;
}

} // namespace

MemPoolAccept::MemPoolAccept(CTxMemPool &pool, const CTransactionRef &ptx)
    : m_pool(pool),
      m_ptx(ptx),
      m_view(&m_dummy),
      m_viewmempool(&::ChainstateActive().CoinsTip(), m_pool) {
}

bool MemPoolAccept::AcceptSingleTransaction(ATMPArgs &args) {
    // Pool lock should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view
    // and when we actually call through to CheckInputs
    LOCK(m_pool.cs);
    AssertLockHeld(cs_main);

    Workspace ws(m_ptx);

    if (!PreChecks(args, ws)) {
        return false;
    }
    if (!ReplacementChecks(args, ws)) {
        return false;
    }

    PrecomputedTransactionData txdata(*ws.ptx);
    if (!PolicyScriptChecks(args, ws, txdata)) {
        return false;
    }
    if (!ConsensusScriptChecks(args, ws, txdata)) {
        return false;
    }
    if (!EvmChecks(args, ws)) {
        return false;
    }
    return Finalize(args, ws);
}

bool MemPoolAccept::PreChecks(ATMPArgs &args, Workspace &ws) {
    const CTransaction &tx = *ws.ptx;
    const uint256 &hash = ws.hash;

    // Copy/alias what we need out of args
    CValidationState &state = args.state;
    bool *pfMissingInputs = args.pfMissingInputs;
    const int64_t nAcceptTime = args.accept_time;
    const bool bypass_limits = args.bypass_limits;
    const CAmount &nAbsurdFee = args.nAbsurdFee;
    std::vector<COutPoint> &coins_to_uncache = args.coins_to_uncache;
    const CChainParams &chainparams = args.chainparams;

    if (pfMissingInputs) {
        *pfMissingInputs = false;
    }

    if (!CheckTransaction(tx, state)) {
        return false; // state filled in by CheckTransaction
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase()) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, "coinbase");
    }

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    const auto isStandard{IsStandardTx(tx, reason)};
    if (reason == "eth-scriptpubkey" || (fRequireStandard && !isStandard)) {
        return state.Invalid(ValidationInvalidReason::TX_NOT_STANDARD, false, reason);
    }

    // Do not work on transactions that are too small.
    // A transaction with 1 segwit input and 1 P2WPHK output has non-witness size of 82 bytes.
    // Transactions smaller than this are not relayed to reduce unnecessary malloc overhead.
    if (::GetSerializeSize(tx, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) < MIN_STANDARD_TX_NONWITNESS_SIZE) {
        return state.Invalid(ValidationInvalidReason::TX_NOT_STANDARD, false, "tx-size-small");
    }

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS)) {
        return state.Invalid(ValidationInvalidReason::TX_PREMATURE_SPEND, false, "non-final");
    }

    // is it already in the memory pool?
    if (m_pool.exists(hash)) {
        return state.Invalid(ValidationInvalidReason::TX_CONFLICT, false, "txn-already-in-mempool");
    }

    ws.isEVMTx = IsEVMTx(tx);

    // Check for conflicts with in-memory transactions
    std::set<uint256> &setConflicts = ws.setConflicts;
    if (!ws.isEVMTx) {
        for (const CTxIn &txin : tx.vin) {
            const CTransaction *ptxConflicting = m_pool.GetConflictTx(txin.prevout);
            if (ptxConflicting) {
                if (!setConflicts.count(ptxConflicting->GetHash())) {
                    // Allow opt-out of transaction replacement by setting
                    // nSequence > MAX_BIP125_RBF_SEQUENCE (SEQUENCE_FINAL-2) on all inputs.
                    //
                    // SEQUENCE_FINAL-1 is picked to still allow use of nLockTime by
                    // non-replaceable transactions. All inputs rather than just one
                    // is for the sake of multi-party protocols, where we don't
                    // want a single party to be able to disable replacement.
                    //
                    // The opt-out ignores descendants as anyone relying on
                    // first-seen mempool behavior should be checking all
                    // unconfirmed ancestors anyway; doing otherwise is hopelessly
                    // insecure.
                    bool fReplacementOptOut = true;
                    for (const CTxIn &_txin : ptxConflicting->vin) {
                        if (_txin.nSequence <= MAX_BIP125_RBF_SEQUENCE) {
                            fReplacementOptOut = false;
                            break;
                        }
                    }
                    if (fReplacementOptOut) {
                        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "txn-mempool-conflict");
                    }

                    setConflicts.insert(ptxConflicting->GetHash());
                }
            }
        }
    }

    LockPoints lp;
    CCoinsViewCache &coins_cache = ::ChainstateActive().CoinsTip();
    m_view.SetBackend(m_viewmempool);

    const auto height = GetSpendHeight(m_view);

    // do all inputs exist?
    if (!ws.isEVMTx) {
        for (const CTxIn &txin : tx.vin) {
            if (!coins_cache.HaveCoinInCache(txin.prevout)) {
                coins_to_uncache.push_back(txin.prevout);
            }

            // Note: this call may add txin.prevout to the coins cache
            // (CoinsTip().cacheCoins) by way of FetchCoin(). It should be removed
            // later (via coins_to_uncache) if this tx turns out to be invalid.
            if (!m_view.HaveCoin(txin.prevout)) {
                // Are inputs missing because we already have the tx?
                for (size_t out = 0; out < tx.vout.size(); out++) {
                    // Optimistically just do efficient check of cache for outputs
                    if (coins_cache.HaveCoinInCache(COutPoint(hash, out))) {
                        return state.Invalid(ValidationInvalidReason::TX_CONFLICT, false, "txn-already-known");
                    }
                }
                // Otherwise assume this might be an orphan tx for which we just haven't seen parents yet
                if (pfMissingInputs) {
                    *pfMissingInputs = true;
                }
                return false; // fMissingInputs and !state.IsInvalid() is used to detect this condition, don't set
                              // state.Invalid()
            }

            // Special check of collateral spending for _not_created_ mn or token (cheating?), those creation tx yet
            // in mempool. CMasternode::CanSpend() (and CheckTxInputs()) will skip this situation
            if (txin.prevout.n == 1 && IsMempooledCustomTxCreate(m_pool, txin.prevout.hash, height)) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS,
                                     false,
                                     "collateral-locked-in-mempool",
                                     strprintf("tried to spend collateral of non-created mn or token %s",
                                               txin.prevout.hash.ToString()));
            }
        }
    }

    // Bring the best block into scope
    m_view.GetBestBlock();

    // rebuild accounts view if dirty
    m_pool.rebuildAccountsView(height, m_view);

    // Get view after we rebuild account view
    m_mnview = std::make_unique<CCustomCSView>(m_pool.accountsView());
    CCustomCSView &mnview = *m_mnview;

    CAmount nFees = 0;
    if (!Consensus::CheckTxInputs(tx, state, m_view, mnview, height, nFees, chainparams)) {
        return error(
            "%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
    }

    if (nAbsurdFee && nFees > nAbsurdFee) {
        return state.Invalid(ValidationInvalidReason::TX_NOT_STANDARD,
                             false,
                             "absurdly-high-fee",
                             strprintf("%d > %d", nFees, nAbsurdFee));
    }

    // let make sure we have needed coins
    if (!ws.isEVMTx && m_view.GetValueIn(tx) < nFees) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "bad-txns-inputs-below-tx-fee");
    }

    const auto &consensus = chainparams.GetConsensus();

    m_blockCtx = std::make_unique<BlockContext>(static_cast<uint32_t>(height),
                                                static_cast<uint64_t>(nAcceptTime),
                                                consensus,
                                                &mnview,
                                                IsEVMEnabled(mnview),
                                                std::shared_ptr<CScopedTemplate>(),
                                                true);

    m_txCtx = std::make_unique<TransactionContext>(m_view, tx, *m_blockCtx);

    auto res = ApplyCustomTx(*m_blockCtx, *m_txCtx);
    if (!res.ok || (res.code & CustomTxErrCodes::Fatal)) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, res.msg);
    }

    // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
    m_view.SetBackend(m_dummy);

    // Only accept BIP68 sequence locked transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
    // CoinsViewCache instead of create its own
    if (!ws.isEVMTx && !CheckSequenceLocks(m_pool, tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp)) {
        return state.Invalid(ValidationInvalidReason::TX_PREMATURE_SPEND, false, "non-BIP68-final");
    }

    // Check for non-standard pay-to-script-hash in inputs
    if (fRequireStandard && !ws.isEVMTx && !AreInputsStandard(tx, m_view)) {
        return state.Invalid(ValidationInvalidReason::TX_INPUTS_NOT_STANDARD, false, "bad-txns-nonstandard-inputs");
    }

    // Check for non-standard witness in P2WSH
    if (tx.HasWitness() && fRequireStandard && !IsWitnessStandard(tx, m_view)) {
        return state.Invalid(ValidationInvalidReason::TX_WITNESS_MUTATED, false, "bad-witness-nonstandard");
    }

    int64_t nSigOpsCost = GetTransactionSigOpCost(tx, m_view, STANDARD_SCRIPT_VERIFY_FLAGS);

    // nModifiedFees includes any fee deltas from PrioritiseTransaction
    CAmount nModifiedFees = nFees;
    m_pool.ApplyDelta(hash, nModifiedFees);
    ws.nModifiedFees = nModifiedFees;

    // Keep track of transactions that spend a coinbase, which we re-scan
    // during reorgs to ensure COINBASE_MATURITY is still met.
    bool fSpendsCoinbase = false;
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = m_view.AccessCoin(txin.prevout);
        if (coin.IsCoinBase()) {
            fSpendsCoinbase = true;
            break;
        }
    }

    ws.entry = std::make_unique<CTxMemPoolEntry>(ws.ptx, nFees, nAcceptTime, ::ChainActive().Height(), fSpendsCoinbase, nSigOpsCost, lp);
    ws.nSize = ws.entry->GetTxSize();

    if (nSigOpsCost > MAX_STANDARD_TX_SIGOPS_COST) {
        return state.Invalid(ValidationInvalidReason::TX_NOT_STANDARD,
                             false,
                             "bad-txns-too-many-sigops",
                             strprintf("%d", nSigOpsCost));
    }

    CAmount mempoolRejectFee =
        m_pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(ws.nSize);
    if (!bypass_limits && mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY,
                             false,
                             "mempool min fee not met",
                             strprintf("%d < %d", nModifiedFees, mempoolRejectFee));
    }

    // No transactions are allowed below minRelayTxFee except from disconnected blocks
    if (!ws.isEVMTx && !bypass_limits && nModifiedFees < ::minRelayTxFee.GetFee(ws.nSize)) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY,
                             false,
                             "min relay fee not met",
                             strprintf("%d < %d", nModifiedFees, ::minRelayTxFee.GetFee(ws.nSize)));
    }

    // Calculate in-mempool ancestors, up to a limit.
    CTxMemPool::setEntries &setAncestors = ws.setAncestors;
    size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
    size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
    size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
    size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000;
    std::string errString;
    if (!m_pool.CalculateMemPoolAncestors(*ws.entry,
                                          setAncestors,
                                          nLimitAncestors,
                                          nLimitAncestorSize,
                                          nLimitDescendants,
                                          nLimitDescendantSize,
                                          errString)) {
        setAncestors.clear();
        // If CalculateMemPoolAncestors fails second time, we want the original error string.
        std::string dummy_err_string;
        // If the new transaction is relatively small (up to 40k weight)
        // and has at most one ancestor (ie ancestor limit of 2, including
        // the new transaction), allow it if its parent has exactly the
        // descendant limit descendants.
        //
        // This allows protocols which rely on distrusting counterparties
        // being able to broadcast descendants of an unconfirmed transaction
        // to be secure by simply only having two immediately-spendable
        // outputs - one for each counterparty. For more info on the uses for
        // this, see https://lists.linuxfoundation.org/pipermail/defi-dev/2018-November/016518.html
        if (ws.nSize > EXTRA_DESCENDANT_TX_SIZE_LIMIT ||
            !m_pool.CalculateMemPoolAncestors(*ws.entry,
                                              setAncestors,
                                              2,
                                              nLimitAncestorSize,
                                              nLimitDescendants + 1,
                                              nLimitDescendantSize + EXTRA_DESCENDANT_TX_SIZE_LIMIT,
                                              dummy_err_string)) {
            return state.Invalid(
                ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "too-long-mempool-chain", errString);
        }
    }

    return true;
}

bool MemPoolAccept::ReplacementChecks(ATMPArgs &args, Workspace &ws) {
    const CTransaction &tx = *ws.ptx;
    const uint256 &hash = ws.hash;
    CValidationState &state = args.state;

    // A transaction that spends outputs that would be replaced by it is invalid. Now
    // that we have the set of all ancestors we can detect this
    // pathological case by making sure setConflicts and setAncestors don't
    // intersect.
    if (const auto err{EntriesAndTxidsDisjoint(ws.setAncestors, ws.setConflicts, hash)}) {
        return state.Invalid(
            ValidationInvalidReason::CONSENSUS,
            false,
            "bad-txns-spends-conflicting-tx",
            *err);
    }

    // Check if it's economically rational to mine this transaction rather
    // than the ones it replaces.
    ws.fReplacementTransaction = ws.setConflicts.size();
    if (!ws.fReplacementTransaction) {
        return true;
    }

    // If we don't hold the lock allConflicting might be incomplete; the
    // subsequent RemoveStaged() and addUnchecked() calls don't guarantee
    // mempool consistency for us.
    const CTxMemPool::setEntries setIterConflicting = m_pool.GetIterSet(ws.setConflicts);

    const CFeeRate newFeeRate(ws.nModifiedFees, ws.nSize);

    // Rule #6: the replacement transaction must have a higher feerate than its
    // direct conflicts.
    if (const auto err{PaysMoreThanConflicts(setIterConflicting, newFeeRate, hash)}) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "insufficient fee", *err);
    }

    // Rule #5: don't replace more than MAX_REPLACEMENT_CANDIDATES entries and
    // collect the transactions that would have to be evicted.
    if (const auto err{GetEntriesForConflicts(tx, m_pool, setIterConflicting, ws.allConflicting)}) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "too many potential replacements", *err);
    }

    // Rule #2: a replacement may not add an unconfirmed input that was not
    // already spent by the transactions it replaces.
    if (const auto err{HasNoNewUnconfirmed(tx, m_pool, setIterConflicting)}) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "replacement-adds-unconfirmed", *err);
    }

    for (CTxMemPool::txiter it : ws.allConflicting) {
        ws.nConflictingFees += it->GetModifiedFee();
        ws.nConflictingSize += it->GetTxSize();
    }

    // Rules #3 and #4: the replacement must pay greater fees than the
    // transactions it replaces and must pay for its own bandwidth.
    if (const auto err{PaysForRBF(ws.nConflictingFees, ws.nModifiedFees, ws.nSize, ::incrementalRelayFee, hash)}) {
        return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "insufficient fee", *err);
    }

    return true;
}

bool MemPoolAccept::PolicyScriptChecks(ATMPArgs &args, Workspace &ws, PrecomputedTransactionData &txdata) {
    const CTransaction &tx = *ws.ptx;
    CValidationState &state = args.state;

    constexpr unsigned int scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;

    // Check against previous transactions
    // This is done last to help prevent CPU exhaustion denial-of-service attacks.
    if (!CheckInputs(tx, state, m_view, true, scriptVerifyFlags, true, false, txdata)) {
        // SCRIPT_VERIFY_CLEANSTACK requires SCRIPT_VERIFY_WITNESS, so we
        // need to turn both off, and compare against just turning off CLEANSTACK
        // to see if the failure is specifically due to witness validation.
        CValidationState stateDummy; // Want reported failures to be from first CheckInputs
        if (!tx.HasWitness() &&
            CheckInputs(tx,
                        stateDummy,
                        m_view,
                        true,
                        scriptVerifyFlags & ~(SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_CLEANSTACK),
                        true,
                        false,
                        txdata) &&
            !CheckInputs(
                tx, stateDummy, m_view, true, scriptVerifyFlags & ~SCRIPT_VERIFY_CLEANSTACK, true, false, txdata)) {
            // Only the witness is missing, so the transaction itself may be fine.
            state.Invalid(ValidationInvalidReason::TX_WITNESS_STRIPPED,
                          false,
                          state.GetRejectReason(),
                          state.GetDebugMessage());
        }
        assert(IsTransactionReason(state.GetReason()));
        return false; // state filled in by CheckInputs
    }

    return true;
}

bool MemPoolAccept::ConsensusScriptChecks(ATMPArgs &args, Workspace &ws, PrecomputedTransactionData &txdata) {
    const CTransaction &tx = *ws.ptx;
    const uint256 &hash = ws.hash;
    CValidationState &state = args.state;

    const auto &consensus = args.chainparams.GetConsensus();

    // Check again against the current block tip's script verification
    // flags to cache our script execution flags. This is, of course,
    // useless if the next block has different script flags from the
    // previous one, but because the cache tracks script flags for us it
    // will auto-invalidate and we'll just have a few blocks of extra
    // misses on soft-fork activation.
    //
    // This is also useful in case of bugs in the standard flags that cause
    // transactions to pass as valid when they're actually invalid. For
    // instance the STRICTENC flag was incorrectly allowing certain
    // CHECKSIG NOT scripts to pass, even though they were invalid.
    //
    // There is a similar check in CreateNewBlock () to prevent creating
    // invalid blocks (using TestBlockValidity), however allowing such
    // transactions into the mempool can be exploited as a DoS attack.
    unsigned int currentBlockScriptVerifyFlags = GetBlockScriptFlags(::ChainActive().Tip(), consensus);

    // TODO: We do multiple guess and parses. Streamline this later.
    // We also have IsEvmTx,  but we need the metadata here.
    std::vector<unsigned char> metadata;
    const auto txType = m_txCtx->GetTxType();
    const auto isEvmTx = txType == CustomTxType::EvmTx;
    ws.entry->SetCustomTxType(txType);

    if (!isEvmTx &&
        !CheckInputsFromMempoolAndCache(tx, state, m_view, m_pool, currentBlockScriptVerifyFlags, true, txdata)) {
        return error(
            "%s: BUG! PLEASE REPORT THIS! CheckInputs failed against latest-block but not STANDARD flags %s, %s",
            __func__,
            hash.ToString(),
            FormatStateMessage(state));
    }

    return true;
}

bool MemPoolAccept::EvmChecks(ATMPArgs &args, Workspace &ws) {
    CValidationState &state = args.state;
    CCustomCSView &mnview = *m_mnview;

    const auto txType = m_txCtx->GetTxType();
    const auto isEvmTx = txType == CustomTxType::EvmTx;

    std::optional<EvmAddressData> ethSender{};

    if (isEvmTx || txType == CustomTxType::TransferDomain) {
        const auto &[r, txMessage] = m_txCtx->GetTxMessage();
        if (!r) {
            LogPrint(BCLog::MEMPOOL, "Failed to parse EVM tx metadata\n");
            return state.Invalid(
                ValidationInvalidReason::TX_NOT_STANDARD, false, "failed-to-parse-evm-tx-metadata");
        }

        std::string rawEVMTx;

        if (ws.isEVMTx) {
            const auto obj = std::get<CEvmTxMessage>(txMessage);
            rawEVMTx = HexStr(obj.evmTx);
        } else {
            const auto obj = std::get<CTransferDomainMessage>(txMessage);
            if (obj.transfers[0].first.domain == static_cast<uint8_t>(VMDomain::DVM) &&
                obj.transfers[0].second.domain == static_cast<uint8_t>(VMDomain::EVM)) {
                rawEVMTx = HexStr(obj.transfers[0].second.data);
            } else if (obj.transfers[0].first.domain == static_cast<uint8_t>(VMDomain::EVM) &&
                       obj.transfers[0].second.domain == static_cast<uint8_t>(VMDomain::DVM)) {
                rawEVMTx = HexStr(obj.transfers[0].first.data);
            }
        }

        CrossBoundaryResult result;
        auto txResult = evm_try_get_tx_miner_info_from_raw_tx(
            result, rawEVMTx, static_cast<std::size_t>(reinterpret_cast<uintptr_t>(&mnview)));
        if (!result.ok) {
            LogPrint(BCLog::MEMPOOL, "EVM tx failed to get sender info %s\n", result.reason.c_str());
            return state.Invalid(ValidationInvalidReason::TX_NOT_STANDARD, false, "evm-sender-info");
        }

        EvmAddressWithNonce evmAddrAndNonce{txResult.nonce, txResult.address};

        const auto entryTipFee = ws.isEVMTx ? txResult.tip_fee : std::numeric_limits<uint64_t>::max();
        const auto minRbfFee = ws.isEVMTx ? txResult.min_rbf_tip_fee : std::numeric_limits<uint64_t>::max();

        ws.entry->SetEVMAddrAndNonce(evmAddrAndNonce);
        ws.entry->SetEVMRbfMinTipFee(minRbfFee);

        auto senderLimitFlag{false};
        CTxMemPool::setEntries evmConflicts;
        if (!m_pool.checkAddressNonceAndFee(*ws.entry, entryTipFee, txResult.address, senderLimitFlag, evmConflicts)) {
            if (senderLimitFlag) {
                LogPrint(BCLog::MEMPOOL,
                         "Too many replace-by-fee EVM tx from the same sender in mempool. Limit %d.\n",
                         MEMPOOL_MAX_ETH_RBF);
                return state.Invalid(
                    ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "too-many-evm-rbf-txs-by-sender");
            } else {
                LogPrint(BCLog::MEMPOOL, "EVM tx rejected due to same or lower fee as existing mempool entry\n");
                return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "evm-low-fee");
            }
        }

        if (!evmConflicts.empty()) {
            std::set<uint256> evmConflictTxids;
            for (const auto &it : evmConflicts) {
                evmConflictTxids.insert(it->GetTx().GetHash());
            }
            if (const auto err{EntriesAndTxidsDisjoint(ws.setAncestors, evmConflictTxids, ws.hash)}) {
                return state.Invalid(
                    ValidationInvalidReason::CONSENSUS,
                    false,
                    "bad-txns-spends-conflicting-tx",
                    *err);
            }

            CTxMemPool::setEntries evmAllConflicting;
            for (const auto &it : evmConflicts) {
                m_pool.CalculateDescendants(it, evmAllConflicting);
            }
            for (const auto &it : evmAllConflicting) {
                if (ws.allConflicting.insert(it).second) {
                    ws.nConflictingFees += it->GetModifiedFee();
                    ws.nConflictingSize += it->GetTxSize();
                }
            }
            ws.fReplacementTransaction = true;
            ws.fEvmReplacement = true;
        }

        const auto sender = m_pool.evmTxsBySender.find(txResult.address);
        if (sender != m_pool.evmTxsBySender.end()) {
            size_t stagedForSender{0};
            for (const auto &it : ws.allConflicting) {
                if (sender->second.count(it->GetTx().GetHash())) {
                    ++stagedForSender;
                }
            }
            if (sender->second.size() - stagedForSender >= MEMPOOL_MAX_ETH_TXS) {
                LogPrint(BCLog::MEMPOOL,
                         "Too many EVM tx from the same sender in mempool. Limit %d.\n",
                         MEMPOOL_MAX_ETH_TXS);
                return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "too-many-evm-txs-by-sender");
            }
        }
        ethSender = txResult.address;

        evm_try_dispatch_pending_transactions_event(result, rawEVMTx);
        if (!result.ok) {
            LogPrint(BCLog::MEMPOOL, "EVM tx failed to generate events %s\n", result.reason.c_str());
        }
    }

    ws.ethSender = ethSender;

    return true;
}

bool MemPoolAccept::Finalize(ATMPArgs &args, Workspace &ws) {
    CValidationState &state = args.state;
    const uint256 &hash = ws.hash;

    if (args.test_accept) {
        // Tx was accepted, but not added
        return true;
    }

    // Remove conflicting transactions from the mempool
    for (CTxMemPool::txiter it : ws.allConflicting) {
        LogPrint(BCLog::MEMPOOL,
                 "replacing tx %s with %s for %s DFI additional fees, %d delta bytes\n",
                 it->GetTx().GetHash().ToString(),
                 hash.ToString(),
                 FormatMoney(ws.nModifiedFees - ws.nConflictingFees),
                 (int)ws.nSize - (int)ws.nConflictingSize);
        if (args.plTxnReplaced) {
            args.plTxnReplaced->push_back(it->GetSharedTx());
        }
    }
    m_pool.RemoveStaged(ws.allConflicting, false, MemPoolRemovalReason::REPLACED);
    if (ws.fEvmReplacement && ws.ethSender) {
        m_pool.IncrementEvmRbfCount(*ws.ethSender);
    }

    // This transaction should only count for fee estimation if:
    // - it isn't a BIP 125 replacement transaction (may not be widely supported)
    // - it's not being re-added during a reorg which bypasses typical mempool fee limits
    // - the node is not behind
    // - the transaction is not dependent on any other transactions in the mempool
    bool validForFeeEstimation = !ws.fReplacementTransaction && !args.bypass_limits && IsCurrentForFeeEstimation() &&
                                 m_pool.HasNoInputsOf(*ws.ptx);

    // Store transaction in memory
    m_pool.addUnchecked(*ws.entry, ws.setAncestors, validForFeeEstimation, ws.ethSender);
    m_mnview->Flush();

    // trim mempool and check if tx was trimmed
    if (!args.bypass_limits) {
        LimitMempoolSize(m_pool,
                         MempoolSizeBytes(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE)),
                         MempoolExpirySeconds(gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_DVM_EXPIRY)),
                         MempoolExpirySeconds(gArgs.GetArg("-mempoolexpiryevm", DEFAULT_MEMPOOL_EVM_EXPIRY)));
        if (!m_pool.exists(hash)) {
            return state.Invalid(ValidationInvalidReason::TX_MEMPOOL_POLICY, false, "mempool full");
        }
    }

    GetMainSignals().TransactionAddedToMempool(ws.ptx);

    return true;
}

/** (try to) add transaction to memory pool with a specified acceptance time **/
bool AcceptToMemoryPoolWithTime(const CChainParams &chainparams,
                                       CTxMemPool &pool,
                                       CValidationState &state,
                                       const CTransactionRef &tx,
                                       bool *pfMissingInputs,
                                       int64_t nAcceptTime,
                                       std::list<CTransactionRef> *plTxnReplaced,
                                       bool bypass_limits,
                                       const CAmount nAbsurdFee,
                                       bool test_accept) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    std::vector<COutPoint> coins_to_uncache;

    MemPoolAccept mempoolAccept(pool, tx);
    MemPoolAccept::ATMPArgs args{chainparams,
                                 state,
                                 pfMissingInputs,
                                 nAcceptTime,
                                 plTxnReplaced,
                                 bypass_limits,
                                 nAbsurdFee,
                                 coins_to_uncache,
                                 test_accept};

    bool res = mempoolAccept.AcceptSingleTransaction(args);
    if (!res) {
        // Remove coins that were not present in the coins cache before calling ATMPW;
        // this is to prevent memory DoS in case we receive a large number of
        // invalid transactions that attempt to overrun the in-memory coins cache
        // (`CCoinsViewCache::cacheCoins`).

        for (const COutPoint &hashTx : coins_to_uncache) {
            ::ChainstateActive().CoinsTip().Uncache(hashTx);
        }
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    CValidationState stateDummy;
    ::ChainstateActive().FlushStateToDisk(chainparams, stateDummy, FlushStateMode::PERIODIC);
    return res;
}

bool AcceptToMemoryPool(CTxMemPool &pool,
                        CValidationState &state,
                        const CTransactionRef &tx,
                        bool *pfMissingInputs,
                        std::list<CTransactionRef> *plTxnReplaced,
                        bool bypass_limits,
                        const CAmount nAbsurdFee,
                        bool test_accept) {
    const CChainParams &chainparams = Params();
    return AcceptToMemoryPoolWithTime(chainparams,
                                      pool,
                                      state,
                                      tx,
                                      pfMissingInputs,
                                      GetTime(),
                                      plTxnReplaced,
                                      bypass_limits,
                                      nAbsurdFee,
                                      test_accept);
}
