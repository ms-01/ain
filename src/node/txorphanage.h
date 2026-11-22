// Copyright (c) 2021-present The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NODE_TXORPHANAGE_H
#define DEFI_NODE_TXORPHANAGE_H

#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace node {
/** Default value for TxOrphanage::m_reserved_usage_per_peer. Helps limit the total amount of memory used by the orphanage. */
inline constexpr int64_t DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER{404'000};
/** Default value for TxOrphanage::m_max_global_latency_score. Helps limit the maximum latency for operations like
 *  EraseForBlock and LimitOrphans. */
inline constexpr unsigned int DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE{3'000};

/** Expiration time of an orphan announcement in seconds (chain specific extension of the upstream
 *  orphanage, which has no time based expiry at all). */
inline constexpr int64_t ORPHAN_TX_EXPIRE_TIME{20 * 60};

// Keep the connection caps below the default global latency limit to avoid a zero per-peer share.
static_assert(MAX_ADDNODE_CONNECTIONS + DEFAULT_MAX_PEER_CONNECTIONS < DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);

/**
 * A class to track orphan transactions (failed on TX_MISSING_INPUTS).
 *
 * Since we cannot distinguish orphans from bad transactions with non-existent inputs, we heavily limit the amount of
 * announcements (unique (NodeId, wtxid) pairs), the number of inputs, and size of the orphans stored (both individual
 * and summed). We also try to prevent adversaries from churning this data structure: once global limits are reached, we
 * continuously evict the oldest announcement (sorting non-reconsiderable orphans before reconsiderable ones) from the
 * most resource-intensive peer until we are back within limits.
 * - Peers can exceed their individual limits (e.g. because they are very useful transaction relay peers) as long as the
 *   global limits are not exceeded.
 * - As long as the orphan has 1 announcer, it remains in the orphanage.
 * - No peer can trigger the eviction of another peer's orphans.
 * - Peers' orphans are effectively protected from eviction as long as they don't exceed their limits.
 *
 * Chain specific extensions on top of the upstream (Bitcoin Core v30) implementation:
 * - Orphans are additionally indexed by txid, so that the txid based INV/erase paths keep working.
 * - Announcements expire after ORPHAN_TX_EXPIRE_TIME seconds (upstream has no expiry).
 * - The number of stored orphans can be capped (the `-maxorphantx` option) and the eviction caused by that cap
 *   only ever affects the peer that is exceeding its reserved share.
 *
 * Not thread-safe. Requires external synchronization (the chain uses `g_cs_orphans`).
 */
class TxOrphanage {
public:
    using Usage = int64_t;
    using Count = unsigned int;

    /** Allows providing orphan information externally */
    struct OrphanInfo {
        CTransactionRef tx;
        /** Peers added with AddTx or AddAnnouncer. */
        std::set<NodeId> announcers;

        // Constructor with moved announcers
        OrphanInfo(CTransactionRef tx, std::set<NodeId>&& announcers) :
            tx(std::move(tx)),
            announcers(std::move(announcers))
        {}
    };

    virtual ~TxOrphanage() = default;

    /** Add a new orphan transaction. Returns true if the transaction was not known before. */
    virtual bool AddTx(const CTransactionRef& tx, NodeId peer) = 0;

    /** Add an additional announcer to an orphan if it exists. Otherwise, do nothing. */
    virtual bool AddAnnouncer(const Wtxid& wtxid, NodeId peer) = 0;

    /** Get a transaction by its witness txid */
    virtual CTransactionRef GetTx(const Wtxid& wtxid) const = 0;

    /** Check if we already have an orphan transaction (by wtxid only) */
    virtual bool HaveTx(const Wtxid& wtxid) const = 0;

    /** Check if a {tx, peer} exists in the orphanage.*/
    virtual bool HaveTxFromPeer(const Wtxid& wtxid, NodeId peer) const = 0;

    /** Check if we have an orphan transaction with the given txid (chain specific). */
    virtual bool HaveTxByTxid(const uint256& txid) const = 0;

    /** Get an orphan transaction by its txid (chain specific). */
    virtual CTransactionRef GetTxByTxid(const uint256& txid) const = 0;

    /** Get the peer that announced the orphan with the given txid first. This is the peer that gets
     *  punished for an invalid orphan transaction (chain specific). */
    virtual NodeId GetPrimaryAnnouncer(const uint256& txid) const = 0;

    /** Extract a transaction from a peer's work set, and flip it back to non-reconsiderable.
     *  Returns nullptr if there are no transactions to work on.
     *  Otherwise returns the transaction reference, and removes
     *  it from the work set.
     */
    virtual CTransactionRef GetTxToReconsider(NodeId peer) = 0;

    /** Erase an orphan by wtxid, including all announcements if there are multiple.
     *  Returns true if an orphan was erased, false if no tx with this wtxid exists. */
    virtual bool EraseTx(const Wtxid& wtxid) = 0;

    /** Erase an orphan by txid, including all announcements if there are multiple (chain specific). */
    virtual bool EraseTxByTxid(const uint256& txid) = 0;

    /** Maybe erase all orphans announced by a peer (eg, after that peer disconnects). If an orphan
     *  has been announced by another peer, don't erase, just remove this peer from the list of announcers. */
    virtual void EraseForPeer(NodeId peer) = 0;

    /** Erase all orphans included in or invalidated by a new block */
    virtual void EraseForBlock(const CBlock& block) = 0;

    /** Add any orphans that list a particular tx as a parent into the from peer's work set */
    virtual std::vector<std::pair<Wtxid, NodeId>> AddChildrenToWorkSet(const CTransaction& tx, FastRandomContext& rng) = 0;

    /** Does this peer have any work to do? */
    virtual bool HaveTxToReconsider(NodeId peer) = 0;

    /** Get all children that spend from this tx and were received from nodeid. Sorted
     *  reconsiderable before non-reconsiderable, then from most recent to least recent. */
    virtual std::vector<CTransactionRef> GetChildrenFromSamePeer(const CTransactionRef& parent, NodeId nodeid) const = 0;

    /** Get all orphan children of the given transaction, independent of the announcing peer. The returned
     *  pairs contain the txid of the child and the peer that announced it first (chain specific). */
    virtual std::vector<std::pair<uint256, NodeId>> GetChildren(const CTransactionRef& parent) const = 0;

    /** Get all orphan transactions */
    virtual std::vector<OrphanInfo> GetOrphanTransactions() const = 0;

    /** Get the total usage (weight) of all orphans. If an orphan has multiple announcers, its usage is
     *  only counted once within this total. */
    virtual Usage TotalOrphanUsage() const = 0;

    /** Total usage (weight) of orphans for which this peer is an announcer. If an orphan has multiple
     *  announcers, its weight will be accounted for in each PeerOrphanInfo, so the total of all
     *  peers' UsageByPeer() may be larger than TotalOrphanUsage(). Similarly, UsageByPeer() may be far higher than
     *  ReservedPeerUsage(), particularly if many peers have provided the same orphans. */
    virtual Usage UsageByPeer(NodeId peer) const = 0;

    /** Check consistency between PeerOrphanInfo and m_orphans. Recalculate counters and ensure they
     *  match what is cached. */
    virtual void SanityCheck() const = 0;

    /** Number of announcements, i.e. total size of m_orphans. Ones for the same wtxid are not de-duplicated.
     *  Not the same as TotalLatencyScore(). */
    virtual Count CountAnnouncements() const = 0;

    /** Number of unique orphans (by wtxid). */
    virtual Count CountUniqueOrphans() const = 0;

    /** Number of orphans stored from this peer. */
    virtual Count AnnouncementsFromPeer(NodeId peer) const = 0;

    /** Latency score of transactions announced by this peer. */
    virtual Count LatencyScoreFromPeer(NodeId peer) const = 0;

    /** Get the maximum global latency score allowed */
    virtual Count MaxGlobalLatencyScore() const = 0;

    /** Get the total latency score of all orphans */
    virtual Count TotalLatencyScore() const = 0;

    /** Get the reserved usage per peer */
    virtual Usage ReservedPeerUsage() const = 0;

    /** Get the maximum latency score allowed per peer */
    virtual Count MaxPeerLatencyScore() const = 0;

    /** Get the maximum usage allowed */
    virtual Usage MaxGlobalUsage() const = 0;

    /** Trim the orphanage if the global latency/usage limits are exceeded (upstream behaviour). */
    virtual void LimitOrphans() = 0;

    /** Trim the orphanage down to at most `max_orphans` unique orphans. Only announcements of peers
     *  that exceed their reserved share of the global limit are evicted, so that no peer can trigger
     *  the eviction of another peer's orphans (chain specific, implements `-maxorphantx`). */
    virtual unsigned int LimitOrphanCount(size_t max_orphans) = 0;

    /** Erase all announcements that are older than ORPHAN_TX_EXPIRE_TIME seconds. Returns the number of
     *  erased announcements (chain specific). */
    virtual unsigned int ExpireOrphans(int64_t now) = 0;

    /** Remove all orphans (chain specific). */
    virtual void Clear() = 0;
};

/** Create a new TxOrphanage instance */
std::unique_ptr<TxOrphanage> MakeTxOrphanage() noexcept;
std::unique_ptr<TxOrphanage> MakeTxOrphanage(TxOrphanage::Count max_global_latency_score, TxOrphanage::Usage reserved_peer_usage) noexcept;
} // namespace node
#endif // DEFI_NODE_TXORPHANAGE_H
