// Copyright (c) 2021-present The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/txorphanage.h>

#include <coins.h>
#include <consensus/validation.h>
#include <logging.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <random.h>
#include <uint256.h>
#include <util/time.h>

#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index_container.hpp>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace node {
/** Minimum NodeId for lower_bound lookups (in practice, NodeIds start at 0). */
static constexpr NodeId MIN_PEER{std::numeric_limits<NodeId>::min()};
/** Maximum NodeId for upper_bound lookups. */
static constexpr NodeId MAX_PEER{std::numeric_limits<NodeId>::max()};

/**
 * Minimal replacement for upstream's util/feefrac.h: an exact ratio of two integers that is
 * compared via cross multiplication (no floating point involved).
 */
struct Ratio {
    int64_t numerator{0};
    int64_t denominator{1};

    Ratio() = default;
    Ratio(int64_t n, int64_t d) : numerator{n}, denominator{d} { assert(d > 0); }
};

#ifdef __SIZEOF_INT128__
using WideInt = __int128;
#else
using WideInt = long double;
#endif

/** Compare two ratios by their value (a < b). */
static inline bool RatioLess(const Ratio& a, const Ratio& b)
{
    return static_cast<WideInt>(a.numerator) * b.denominator < static_cast<WideInt>(b.numerator) * a.denominator;
}

/**
 * Total ordering of two ratios: first by their value, then by reversed denominator (i.e. the
 * larger denominator comes first). This mirrors upstream's ByRatioNegSize ordering.
 */
static inline int RatioCompareNegSize(const Ratio& a, const Ratio& b)
{
    if (RatioLess(a, b)) return -1;
    if (RatioLess(b, a)) return 1;
    if (a.denominator != b.denominator) return a.denominator > b.denominator ? -1 : 1;
    return 0;
}

class TxOrphanageImpl final : public TxOrphanage {
    // Type alias for sequence numbers
    using SequenceNumber = uint64_t;
    /** Global sequence number, increment each time an announcement is added. */
    SequenceNumber m_current_sequence{0};

    /** One orphan announcement. Each announcement (i.e. combination of wtxid, nodeid) is unique. There may be multiple
     * announcements for the same tx, and multiple transactions with the same txid but different wtxid are possible. */
    struct Announcement
    {
        const CTransactionRef m_tx;
        /** Which peer announced this tx */
        const NodeId m_announcer;
        /** What order this transaction entered the orphanage. */
        const SequenceNumber m_entry_sequence;
        /** When this announcement entered the orphanage (chain specific, used for expiry). */
        const int64_t m_entry_time;
        /** Whether this tx should be reconsidered. Always starts out false. A peer's workset is the collection of all
         * announcements with m_reconsider=true. */
        bool m_reconsider{false};

        Announcement(const CTransactionRef& tx, NodeId peer, SequenceNumber seq, int64_t entry_time) :
            m_tx{tx}, m_announcer{peer}, m_entry_sequence{seq}, m_entry_time{entry_time}
        { }

        /** Get an approximation for "memory usage". The total memory is a function of the memory used to store the
         * transaction itself, each entry in m_orphans, and each entry in m_outpoint_to_orphan_wtxids. We use weight because
         * it is often higher than the actual memory usage of the transaction. This metric conveniently encompasses
         * m_outpoint_to_orphan_wtxids usage since input data does not get the witness discount, and makes it easier to
         * reason about each peer's limits using well-understood transaction attributes. */
        TxOrphanage::Usage GetMemUsage()  const {
            return GetTransactionWeight(*m_tx);
        }

        /** Get an approximation of how much this transaction contributes to latency in EraseForBlock and EraseForPeer.
         * The computation time is a function of the number of entries in m_orphans (thus 1 per announcement) and the
         * number of entries in m_outpoint_to_orphan_wtxids (thus an additional 1 for every 10 inputs). Transactions with a
         * small number of inputs (9 or fewer) are counted as 1 to make it easier to reason about each peer's limits in
         * terms of "normal" transactions. */
        TxOrphanage::Count GetLatencyScore() const {
            return 1 + (m_tx->vin.size() / 10);
        }
    };

    // Index by wtxid, then peer
    struct ByWtxid {};
    using ByWtxidView = std::tuple<Wtxid, NodeId>;
    struct WtxidExtractor
    {
        using result_type = ByWtxidView;
        result_type operator()(const Announcement& ann) const
        {
            return ByWtxidView{Wtxid::FromUint256(ann.m_tx->GetWitnessHash()), ann.m_announcer};
        }
    };

    // Sort by peer, then by whether it is ready to reconsider, then by recency.
    struct ByPeer {};
    using ByPeerView = std::tuple<NodeId, bool, SequenceNumber>;
    struct ByPeerViewExtractor {
        using result_type = ByPeerView;
        result_type operator()(const Announcement& ann) const
        {
            return ByPeerView{ann.m_announcer, ann.m_reconsider, ann.m_entry_sequence};
        }
    };

    using AnnouncementMap = boost::multi_index::multi_index_container<
        Announcement,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<boost::multi_index::tag<ByWtxid>, WtxidExtractor>,
            boost::multi_index::ordered_unique<boost::multi_index::tag<ByPeer>, ByPeerViewExtractor>
        >
    >;
    template<typename Tag>
    using Iter = typename AnnouncementMap::index<Tag>::type::iterator;
    AnnouncementMap m_orphans;

    const TxOrphanage::Count m_max_global_latency_score{DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE};
    const TxOrphanage::Usage m_reserved_usage_per_peer{DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER};

    /** Number of unique orphans by wtxid. Less than or equal to the number of entries in m_orphans. */
    TxOrphanage::Count m_unique_orphans{0};

    /** Memory used by orphans (see Announcement::GetMemUsage()), deduplicated by wtxid. */
    TxOrphanage::Usage m_unique_orphan_usage{0};

    /** The sum of each unique transaction's latency scores including the inputs only (see Announcement::GetLatencyScore
     * but subtract 1 for the announcements themselves). The total orphanage's latency score is given by this value +
     * the number of entries in m_orphans. */
    TxOrphanage::Count m_unique_rounded_input_scores{0};

    /** Index from the parents' outputs to wtxids that exist in m_orphans. Used to find children of
     * a transaction that can be reconsidered and to remove entries that conflict with a block.*/
    std::unordered_map<COutPoint, std::set<Wtxid>, SaltedOutpointHasher> m_outpoint_to_orphan_wtxids;

    /** Set of Wtxids for which (exactly) one announcement with m_reconsider=true exists. */
    std::set<Wtxid> m_reconsiderable_wtxids;

    /** Index from txid to wtxid (chain specific). The chain's orphan pool historically was keyed by txid,
     *  so at most one orphan per txid is stored and the txid based code paths keep working. */
    std::map<uint256, Wtxid> m_txid_to_wtxid;

    struct PeerDoSInfo {
        TxOrphanage::Usage m_total_usage{0};
        TxOrphanage::Count m_count_announcements{0};
        TxOrphanage::Count m_total_latency_score{0};
        bool operator==(const PeerDoSInfo& other) const
        {
            return m_total_usage == other.m_total_usage &&
                   m_count_announcements == other.m_count_announcements &&
                   m_total_latency_score == other.m_total_latency_score;
        }
        void Add(const Announcement& ann)
        {
            m_total_usage += ann.GetMemUsage();
            m_total_latency_score += ann.GetLatencyScore();
            m_count_announcements += 1;
        }
        bool Subtract(const Announcement& ann)
        {
            assert(m_total_usage >= ann.GetMemUsage());
            assert(m_total_latency_score >= ann.GetLatencyScore());
            assert(m_count_announcements >= 1);

            m_total_usage -= ann.GetMemUsage();
            m_total_latency_score -= ann.GetLatencyScore();
            m_count_announcements -= 1;
            return m_count_announcements == 0;
        }
        /** There are 2 DoS scores:
        * - Latency score (ratio of total latency score / max allowed latency score)
        * - Memory score (ratio of total memory usage / max allowed memory usage).
        *
        * If the peer is using more than the allowed for either resource, its DoS score is > 1.
        * A peer having a DoS score > 1 does not necessarily mean that something is wrong, since we
        * do not trim unless the orphanage exceeds global limits, but it means that this peer will
        * be selected for trimming sooner. If the global latency score or global memory usage
        * limits are exceeded, it must be that there is a peer whose DoS score > 1. */
        Ratio GetDosScore(TxOrphanage::Count max_peer_latency_score, TxOrphanage::Usage max_peer_memory) const
        {
            assert(max_peer_latency_score > 0);
            assert(max_peer_memory > 0);
            const Ratio latency_score(m_total_latency_score, max_peer_latency_score);
            const Ratio mem_score(m_total_usage, max_peer_memory);
            // Take the "worst" of both ratios (ties are broken by the larger denominator).
            return RatioCompareNegSize(latency_score, mem_score) >= 0 ? latency_score : mem_score;
        }
    };
    /** Store per-peer statistics. Used to determine each peer's DoS score. The size of this map is used to determine the
     * number of peers and thus global {latency score, memory} limits. */
    std::unordered_map<NodeId, PeerDoSInfo> m_peer_orphanage_info;

    /** Erase from m_orphans and update m_peer_orphanage_info. */
    template<typename Tag>
    void Erase(Iter<Tag> it);

    /** Erase by wtxid. */
    bool EraseTxInternal(const Wtxid& wtxid);

    /** Check if there is exactly one announcement with the same wtxid as it. */
    bool IsUnique(Iter<ByWtxid> it) const;

    /** Check if the orphanage needs trimming. */
    bool NeedsTrim() const;

    /** Number of announcements for the given wtxid. */
    TxOrphanage::Count CountAnnouncers(const Wtxid& wtxid) const;

    /** The peer that announced the given orphan first (or -1 if the orphan is unknown). */
    NodeId PrimaryAnnouncer(const Wtxid& wtxid) const;

public:
    TxOrphanageImpl() = default;
    TxOrphanageImpl(Count max_global_latency_score, Usage reserved_peer_usage) :
        m_max_global_latency_score{max_global_latency_score},
        m_reserved_usage_per_peer{reserved_peer_usage}
    {}
    ~TxOrphanageImpl() noexcept override = default;

    TxOrphanage::Count CountAnnouncements() const override;
    TxOrphanage::Count CountUniqueOrphans() const override;
    TxOrphanage::Count AnnouncementsFromPeer(NodeId peer) const override;
    TxOrphanage::Count LatencyScoreFromPeer(NodeId peer) const override;
    TxOrphanage::Usage UsageByPeer(NodeId peer) const override;

    TxOrphanage::Count MaxGlobalLatencyScore() const override;
    TxOrphanage::Count TotalLatencyScore() const override;
    TxOrphanage::Usage ReservedPeerUsage() const override;

    /** Maximum allowed (deduplicated) latency score for all transactions (see Announcement::GetLatencyScore()). Dynamic
     * based on number of peers. Each peer has an equal amount, but the global maximum latency score stays constant. The
     * number of peers times MaxPeerLatencyScore() (rounded) adds up to MaxGlobalLatencyScore().  As long as every peer's
     * m_total_latency_score / MaxPeerLatencyScore() < 1, MaxGlobalLatencyScore() is not exceeded. */
    TxOrphanage::Count MaxPeerLatencyScore() const override;

    /** Maximum allowed (deduplicated) memory usage for all transactions (see Announcement::GetMemUsage()). Dynamic based
     * on number of peers. More peers means more allowed memory usage. The number of peers times ReservedPeerUsage()
     * adds up to MaxGlobalUsage(). As long as every peer's m_total_usage / ReservedPeerUsage() < 1, MaxGlobalUsage() is
     * not exceeded. */
    TxOrphanage::Usage MaxGlobalUsage() const override;

    bool AddTx(const CTransactionRef& tx, NodeId peer) override;
    bool AddAnnouncer(const Wtxid& wtxid, NodeId peer) override;
    CTransactionRef GetTx(const Wtxid& wtxid) const override;
    bool HaveTx(const Wtxid& wtxid) const override;
    bool HaveTxFromPeer(const Wtxid& wtxid, NodeId peer) const override;
    bool HaveTxByTxid(const uint256& txid) const override;
    CTransactionRef GetTxByTxid(const uint256& txid) const override;
    NodeId GetPrimaryAnnouncer(const uint256& txid) const override;
    CTransactionRef GetTxToReconsider(NodeId peer) override;
    bool EraseTx(const Wtxid& wtxid) override;
    bool EraseTxByTxid(const uint256& txid) override;
    void EraseForPeer(NodeId peer) override;
    void EraseForBlock(const CBlock& block) override;
    std::vector<std::pair<Wtxid, NodeId>> AddChildrenToWorkSet(const CTransaction& tx, FastRandomContext& rng) override;
    bool HaveTxToReconsider(NodeId peer) override;
    std::vector<CTransactionRef> GetChildrenFromSamePeer(const CTransactionRef& parent, NodeId nodeid) const override;
    std::vector<std::pair<uint256, NodeId>> GetChildren(const CTransactionRef& parent) const override;
    std::vector<OrphanInfo> GetOrphanTransactions() const override;
    TxOrphanage::Usage TotalOrphanUsage() const override;
    void SanityCheck() const override;
    void LimitOrphans() override;
    unsigned int LimitOrphanCount(size_t max_orphans) override;
    unsigned int ExpireOrphans(int64_t now) override;
    void Clear() override;
};

template<typename Tag>
void TxOrphanageImpl::Erase(Iter<Tag> it)
{
    // Update m_peer_orphanage_info and clean up entries if they point to an empty struct.
    // This means peers that are not storing any orphans do not have an entry in
    // m_peer_orphanage_info (they can be added back later if they announce another orphan) and
    // ensures disconnected peers are not tracked forever.
    auto peer_it = m_peer_orphanage_info.find(it->m_announcer);
    assert(peer_it != m_peer_orphanage_info.end());
    if (peer_it->second.Subtract(*it)) m_peer_orphanage_info.erase(peer_it);

    if (IsUnique(m_orphans.project<ByWtxid>(it))) {
        m_unique_orphans -= 1;
        m_unique_rounded_input_scores -= it->GetLatencyScore() - 1;
        m_unique_orphan_usage -= it->GetMemUsage();

        // Remove references in m_outpoint_to_orphan_wtxids
        const auto& wtxid{it->m_tx->GetWitnessHash()};
        for (const auto& input : it->m_tx->vin) {
            auto it_prev = m_outpoint_to_orphan_wtxids.find(input.prevout);
            if (it_prev != m_outpoint_to_orphan_wtxids.end()) {
                it_prev->second.erase(Wtxid::FromUint256(wtxid));
                // Clean up keys if they point to an empty set.
                if (it_prev->second.empty()) {
                    m_outpoint_to_orphan_wtxids.erase(it_prev);
                }
            }
        }
        // The orphan is gone: drop the txid index entry as well (chain specific).
        m_txid_to_wtxid.erase(it->m_tx->GetHash());
    }

    // If this was the (unique) reconsiderable announcement for its wtxid, then the wtxid won't
    // have any reconsiderable announcements left after erasing.
    if (it->m_reconsider) m_reconsiderable_wtxids.erase(Wtxid::FromUint256(it->m_tx->GetWitnessHash()));

    m_orphans.get<Tag>().erase(it);
}

bool TxOrphanageImpl::IsUnique(Iter<ByWtxid> it) const
{
    // Iterators ByWtxid are sorted by wtxid, so check if neighboring elements have the same wtxid.
    auto& index = m_orphans.get<ByWtxid>();
    if (it == index.end()) return false;
    if (std::next(it) != index.end() && std::next(it)->m_tx->GetWitnessHash() == it->m_tx->GetWitnessHash()) return false;
    if (it != index.begin() && std::prev(it)->m_tx->GetWitnessHash() == it->m_tx->GetWitnessHash()) return false;
    return true;
}

TxOrphanage::Count TxOrphanageImpl::CountAnnouncers(const Wtxid& wtxid) const
{
    auto& index = m_orphans.get<ByWtxid>();
    auto it = index.lower_bound(ByWtxidView{wtxid, MIN_PEER});
    if (it == index.end() || it->m_tx->GetWitnessHash() != wtxid) return 0;
    auto it_end = index.upper_bound(ByWtxidView{wtxid, MAX_PEER});
    return static_cast<TxOrphanage::Count>(std::distance(it, it_end));
}

NodeId TxOrphanageImpl::PrimaryAnnouncer(const Wtxid& wtxid) const
{
    auto& index = m_orphans.get<ByWtxid>();
    auto it = index.lower_bound(ByWtxidView{wtxid, MIN_PEER});
    if (it == index.end() || it->m_tx->GetWitnessHash() != wtxid) return -1;
    NodeId best_peer = it->m_announcer;
    SequenceNumber best_sequence = it->m_entry_sequence;
    auto it_end = index.upper_bound(ByWtxidView{wtxid, MAX_PEER});
    for (auto it2 = it; it2 != it_end; ++it2) {
        if (it2->m_entry_sequence < best_sequence) {
            best_sequence = it2->m_entry_sequence;
            best_peer = it2->m_announcer;
        }
    }
    return best_peer;
}

TxOrphanage::Usage TxOrphanageImpl::UsageByPeer(NodeId peer) const
{
    auto it = m_peer_orphanage_info.find(peer);
    return it == m_peer_orphanage_info.end() ? 0 : it->second.m_total_usage;
}

TxOrphanage::Count TxOrphanageImpl::CountAnnouncements() const { return m_orphans.size(); }

TxOrphanage::Usage TxOrphanageImpl::TotalOrphanUsage() const { return m_unique_orphan_usage; }

TxOrphanage::Count TxOrphanageImpl::CountUniqueOrphans() const { return m_unique_orphans; }

TxOrphanage::Count TxOrphanageImpl::AnnouncementsFromPeer(NodeId peer) const {
    auto it = m_peer_orphanage_info.find(peer);
    return it == m_peer_orphanage_info.end() ? 0 : it->second.m_count_announcements;
}

TxOrphanage::Count TxOrphanageImpl::LatencyScoreFromPeer(NodeId peer) const {
    auto it = m_peer_orphanage_info.find(peer);
    return it == m_peer_orphanage_info.end() ? 0 : it->second.m_total_latency_score;
}

bool TxOrphanageImpl::AddTx(const CTransactionRef& tx, NodeId peer)
{
    const auto wtxid{Wtxid::FromUint256(tx->GetWitnessHash())};
    const auto& txid{tx->GetHash()};

    // Ignore transactions above max standard size to avoid a send-big-orphans memory exhaustion attack.
    TxOrphanage::Usage sz = GetTransactionWeight(*tx);
    if (sz > MAX_STANDARD_TX_WEIGHT) {
        LogPrint(BCLog::MEMPOOL, "ignoring large orphan tx (size: %u, hash: %s)\n", (unsigned int)sz, txid.ToString());
        return false;
    }

    // Chain specific: at most one orphan per txid is stored (this is the historical behaviour of
    // the chain's orphan pool, which was keyed by txid). A transaction with the same txid but a
    // different witness is therefore rejected instead of being stored as a second orphan.
    auto txid_it = m_txid_to_wtxid.find(txid);
    if (txid_it != m_txid_to_wtxid.end() && txid_it->second != wtxid) {
        LogPrint(BCLog::MEMPOOL, "ignoring orphan tx with known txid but different witness (hash: %s)\n", txid.ToString());
        return false;
    }

    // We will return false if the tx already exists under a different peer.
    const bool brand_new{!HaveTx(wtxid)};

    auto [iter, inserted] = m_orphans.get<ByWtxid>().emplace(tx, peer, m_current_sequence, GetTime());
    // If the announcement (same wtxid, same peer) already exists, emplacement fails. Return false.
    if (!inserted) return false;

    ++m_current_sequence;
    auto& peer_info = m_peer_orphanage_info.try_emplace(peer).first->second;
    peer_info.Add(*iter);

    // Add links in m_outpoint_to_orphan_wtxids
    if (brand_new) {
        for (const auto& input : tx->vin) {
            auto& wtxids_for_prevout = m_outpoint_to_orphan_wtxids.try_emplace(input.prevout).first->second;
            wtxids_for_prevout.emplace(wtxid);
        }

        m_unique_orphans += 1;
        m_unique_orphan_usage += iter->GetMemUsage();
        m_unique_rounded_input_scores += iter->GetLatencyScore() - 1;
        m_txid_to_wtxid[txid] = wtxid;

        LogPrint(BCLog::MEMPOOL, "stored orphan tx %s (wtxid=%s), weight: %u (mapsz %u outsz %u)\n",
                    txid.ToString(), wtxid.ToString(), (unsigned int)sz, m_orphans.size(), m_outpoint_to_orphan_wtxids.size());
        assert(IsUnique(iter));
    } else {
        LogPrint(BCLog::MEMPOOL, "added peer=%d as announcer of orphan tx %s (wtxid=%s)\n",
                    peer, txid.ToString(), wtxid.ToString());
        assert(!IsUnique(iter));
    }

    // DoS prevention: do not allow the orphanage to grow unbounded (see CVE-2012-3789)
    LimitOrphans();
    return brand_new;
}

bool TxOrphanageImpl::AddAnnouncer(const Wtxid& wtxid, NodeId peer)
{
    auto& index_by_wtxid = m_orphans.get<ByWtxid>();
    auto it = index_by_wtxid.lower_bound(ByWtxidView{wtxid, MIN_PEER});

    // Do nothing if this transaction isn't already present. We can't create an entry if we don't
    // have the tx data.
    if (it == index_by_wtxid.end()) return false;
    if (it->m_tx->GetWitnessHash() != wtxid) return false;

    // Add another announcement, copying the CTransactionRef from one that already exists.
    const auto& ptx = it->m_tx;
    auto [iter, inserted] = index_by_wtxid.emplace(ptx, peer, m_current_sequence, GetTime());
    // If the announcement (same wtxid, same peer) already exists, emplacement fails. Return false.
    if (!inserted) return false;

    ++m_current_sequence;
    auto& peer_info = m_peer_orphanage_info.try_emplace(peer).first->second;
    peer_info.Add(*iter);

    const auto& txid = ptx->GetHash();
    LogPrint(BCLog::MEMPOOL, "added peer=%d as announcer of orphan tx %s (wtxid=%s)\n",
                peer, txid.ToString(), wtxid.ToString());

    assert(!IsUnique(iter));

    // DoS prevention: do not allow the orphanage to grow unbounded (see CVE-2012-3789)
    LimitOrphans();
    return true;
}

bool TxOrphanageImpl::EraseTxInternal(const Wtxid& wtxid)
{
    auto& index_by_wtxid = m_orphans.get<ByWtxid>();

    auto it = index_by_wtxid.lower_bound(ByWtxidView{wtxid, MIN_PEER});
    if (it == index_by_wtxid.end() || it->m_tx->GetWitnessHash() != wtxid) return false;

    auto it_end = index_by_wtxid.upper_bound(ByWtxidView{wtxid, MAX_PEER});
    unsigned int num_ann{0};
    const auto txid = it->m_tx->GetHash();
    while (it != it_end) {
        assert(it->m_tx->GetWitnessHash() == wtxid);
        Erase<ByWtxid>(it++);
        num_ann += 1;
    }
    LogPrint(BCLog::MEMPOOL, "removed orphan tx %s (wtxid=%s) (%u announcements)\n", txid.ToString(), wtxid.ToString(), num_ann);

    return true;
}

bool TxOrphanageImpl::EraseTx(const Wtxid& wtxid)
{
    const auto ret = EraseTxInternal(wtxid);

    // Deletions can cause the orphanage's MaxGlobalUsage to decrease, so we may need to trim here.
    LimitOrphans();

    return ret;
}

bool TxOrphanageImpl::EraseTxByTxid(const uint256& txid)
{
    auto it = m_txid_to_wtxid.find(txid);
    if (it == m_txid_to_wtxid.end()) return false;
    const Wtxid wtxid = it->second;
    return EraseTx(wtxid);
}

/** Erase all entries by this peer. */
void TxOrphanageImpl::EraseForPeer(NodeId peer)
{
    auto& index_by_peer = m_orphans.get<ByPeer>();
    auto it = index_by_peer.lower_bound(ByPeerView{peer, false, 0});
    if (it == index_by_peer.end() || it->m_announcer != peer) return;

    unsigned int num_ann{0};
    while (it != index_by_peer.end() && it->m_announcer == peer) {
        // Delete item, cleaning up m_outpoint_to_orphan_wtxids iff this entry is unique by wtxid.
        Erase<ByPeer>(it++);
        num_ann += 1;
    }
    assert(!m_peer_orphanage_info.contains(peer));

    if (num_ann > 0) LogPrint(BCLog::MEMPOOL, "Erased %d orphan transaction(s) from peer=%d\n", num_ann, peer);

    // Deletions can cause the orphanage's MaxGlobalUsage to decrease, so we may need to trim here.
    LimitOrphans();
}

/** If the data structure needs trimming, evicts announcements by selecting the DoSiest peer and evicting its oldest
 * announcement (sorting non-reconsiderable before reconsiderable). Does nothing if no global limits are exceeded.  This
 * eviction strategy effectively "reserves" an amount of announcements and space for each peer. The reserved amount is
 * protected from eviction even if there are peers spamming the orphanage.
 */
void TxOrphanageImpl::LimitOrphans()
{
    if (!NeedsTrim()) return;

    const auto original_unique_txns{CountUniqueOrphans()};

    // Even though it's possible for MaxPeerLatencyScore to increase within this call to LimitOrphans
    // (e.g. if a peer's orphans are removed entirely, changing the number of peers), use consistent limits throughout.
    const auto max_lat{MaxPeerLatencyScore()};
    const auto max_mem{ReservedPeerUsage()};

    // We have exceeded the global limit(s). Now, identify who is using too much and evict their orphans.
    // Create a heap of pairs (NodeId, DoS score), sorted by descending DoS score.
    std::vector<std::pair<NodeId, Ratio>> heap_peer_dos;
    heap_peer_dos.reserve(m_peer_orphanage_info.size());
    for (const auto& [nodeid, entry] : m_peer_orphanage_info) {
        // Performance optimization: only consider peers with a DoS score > 1.
        const auto dos_score = entry.GetDosScore(max_lat, max_mem);
        if (RatioLess(Ratio{1, 1}, dos_score)) {
            heap_peer_dos.emplace_back(nodeid, dos_score);
        }
    }
    static constexpr auto compare_score = [](const auto& left, const auto& right) {
        // Note: if the ratios are the same, this tiebreaks by denominator. In practice, since the
        // latency denominator (number of announcements and inputs) is always lower, this means
        // that a peer with only high latency scores will be targeted before a peer using a lot
        // of memory, even if they have the same ratios.
        const int cmp = RatioCompareNegSize(left.second, right.second);
        if (cmp != 0) return cmp < 0;
        // Tiebreak by considering the more recent peer (higher NodeId) to be worse.
        return left.first < right.first;
    };
    std::make_heap(heap_peer_dos.begin(), heap_peer_dos.end(), compare_score);

    unsigned int num_erased{0};
    // This outer loop finds the peer with the highest DoS score, which is a fraction of memory and latency scores
    // over the respective allowances. We continue until the orphanage is within global limits. That means some peers
    // might still have a DoS score > 1 at the end.
    do {
        assert(!heap_peer_dos.empty());
        // This is a max-heap, so the worst peer is at the front. pop_heap()
        // moves it to the back, and the next worst peer is moved to the front.
        std::pop_heap(heap_peer_dos.begin(), heap_peer_dos.end(), compare_score);
        const auto [worst_peer, dos_score] = std::move(heap_peer_dos.back());
        heap_peer_dos.pop_back();

        // If needs trim, then at least one peer has a DoS score higher than 1.
        assert(RatioLess(Ratio{1, 1}, dos_score));

        auto it_worst_peer = m_peer_orphanage_info.find(worst_peer);

        // This inner loop trims until this peer is no longer the DoSiest one or has a score within 1. The score 1 is
        // just a conservative fallback: once the last peer goes below ratio 1, NeedsTrim() will return false anyway.
        // We evict the oldest announcement(s) from this peer, sorting non-reconsiderable before reconsiderable.
        // The number of inner loop iterations is bounded by the total number of announcements.
        const auto dos_threshold = heap_peer_dos.empty() ? Ratio{1, 1} : heap_peer_dos.front().second;
        auto it_ann = m_orphans.get<ByPeer>().lower_bound(ByPeerView{worst_peer, false, 0});
        unsigned int num_erased_this_round{0};
        unsigned int starting_num_ann{it_worst_peer->second.m_count_announcements};
        while (NeedsTrim()) {
            if (!(it_ann != m_orphans.get<ByPeer>().end())) break;
            if (!(it_ann->m_announcer == worst_peer)) break;

            Erase<ByPeer>(it_ann++);
            num_erased += 1;
            num_erased_this_round += 1;

            // If we erased the last orphan from this peer, it_worst_peer will be invalidated.
            it_worst_peer = m_peer_orphanage_info.find(worst_peer);
            if (it_worst_peer == m_peer_orphanage_info.end() ||
                RatioCompareNegSize(it_worst_peer->second.GetDosScore(max_lat, max_mem), dos_threshold) <= 0) break;
        }
        LogPrint(BCLog::MEMPOOL, "peer=%d orphanage overflow, removed %u of %u announcements\n", worst_peer, num_erased_this_round, starting_num_ann);

        if (!NeedsTrim()) break;

        // Unless this peer is empty, put it back in the heap so we continue to consider evicting its orphans.
        // We may select this peer for evictions again if there are multiple DoSy peers.
        if (it_worst_peer != m_peer_orphanage_info.end() && it_worst_peer->second.m_count_announcements > 0) {
            heap_peer_dos.emplace_back(worst_peer, it_worst_peer->second.GetDosScore(max_lat, max_mem));
            std::push_heap(heap_peer_dos.begin(), heap_peer_dos.end(), compare_score);
        }
    } while (true);

    const auto remaining_unique_orphans{CountUniqueOrphans()};
    LogPrint(BCLog::MEMPOOL, "orphanage overflow, removed %u tx (%u announcements)\n", original_unique_txns - remaining_unique_orphans, num_erased);
}

/**
 * Trim the orphanage down to at most `max_orphans` unique orphans (chain specific, implements the
 * `-maxorphantx` option).
 *
 * Only announcements of the peer that is using the most resources are evicted, so that a peer that
 * floods the orphanage with orphans cannot evict the orphans of other peers. Announcements that are
 * the only announcement of their orphan are evicted first, because only those reduce the number of
 * unique orphans.
 */
unsigned int TxOrphanageImpl::LimitOrphanCount(size_t max_orphans)
{
    unsigned int num_erased{0};

    while (CountUniqueOrphans() > max_orphans && !m_orphans.empty()) {
        // Order the peers by the number of announcements they have (descending). The peer with the
        // most announcements is the one that exceeds its reserved share of the global limit.
        // Tiebreak by considering the more recent peer (higher NodeId) to be worse, matching the
        // upstream orphanage (see upstream's compare_score() in src/node/txorphanage.cpp).
        std::vector<NodeId> peers;
        peers.reserve(m_peer_orphanage_info.size());
        for (const auto& [nodeid, info] : m_peer_orphanage_info) peers.push_back(nodeid);
        std::sort(peers.begin(), peers.end(), [this](NodeId a, NodeId b) {
            const auto count_a = m_peer_orphanage_info.at(a).m_count_announcements;
            const auto count_b = m_peer_orphanage_info.at(b).m_count_announcements;
            return count_a != count_b ? count_a > count_b : a > b;
        });

        bool erased{false};
        for (const NodeId peer : peers) {
            auto& index_by_peer = m_orphans.get<ByPeer>();
            auto it = index_by_peer.lower_bound(ByPeerView{peer, false, 0});
            for (; it != index_by_peer.end() && it->m_announcer == peer; ++it) {
                // Only evict an announcement that is the sole announcement of its orphan: that is
                // the only way to reduce the number of unique orphans.
                if (CountAnnouncers(Wtxid::FromUint256(it->m_tx->GetWitnessHash())) != 1) continue;
                Erase<ByPeer>(it);
                ++num_erased;
                erased = true;
                break;
            }
            if (erased) break;
        }

        if (!erased) {
            // All remaining orphans are announced by more than one peer: evict the oldest
            // announcement of the peer that is using the most resources.
            const NodeId worst_peer = peers.front();
            auto& index_by_peer = m_orphans.get<ByPeer>();
            auto it = index_by_peer.lower_bound(ByPeerView{worst_peer, false, 0});
            if (it == index_by_peer.end() || it->m_announcer != worst_peer) break;
            Erase<ByPeer>(it);
            ++num_erased;
            erased = true;
        }

        if (!erased) break;
    }

    if (num_erased > 0) {
        LogPrint(BCLog::MEMPOOL, "orphanage overflow, removed %u tx due to the orphan count limit\n", num_erased);
    }
    return num_erased;
}

/**
 * Erase all announcements that are older than ORPHAN_TX_EXPIRE_TIME seconds (chain specific).
 */
unsigned int TxOrphanageImpl::ExpireOrphans(int64_t now)
{
    unsigned int num_erased{0};
    for (auto it = m_orphans.begin(); it != m_orphans.end();) {
        if (it->m_entry_time + ORPHAN_TX_EXPIRE_TIME <= now) {
            Erase<ByWtxid>(it++);
            ++num_erased;
        } else {
            ++it;
        }
    }
    if (num_erased > 0) {
        LogPrint(BCLog::MEMPOOL, "Erased %d orphan tx due to expiration\n", num_erased);
    }
    return num_erased;
}

void TxOrphanageImpl::Clear()
{
    m_orphans.clear();
    m_outpoint_to_orphan_wtxids.clear();
    m_reconsiderable_wtxids.clear();
    m_txid_to_wtxid.clear();
    m_peer_orphanage_info.clear();
    m_unique_orphans = 0;
    m_unique_orphan_usage = 0;
    m_unique_rounded_input_scores = 0;
}

std::vector<std::pair<Wtxid, NodeId>> TxOrphanageImpl::AddChildrenToWorkSet(const CTransaction& tx, FastRandomContext& rng)
{
    if (m_orphans.empty()) return {};

    std::vector<std::pair<Wtxid, NodeId>> ret;
    auto& index_by_wtxid = m_orphans.get<ByWtxid>();
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const auto it_by_prev = m_outpoint_to_orphan_wtxids.find(COutPoint(tx.GetHash(), i));
        if (it_by_prev != m_outpoint_to_orphan_wtxids.end()) {
            for (const auto& wtxid : it_by_prev->second) {
                // If a reconsiderable announcement for this wtxid already exists, skip it.
                if (m_reconsiderable_wtxids.contains(wtxid)) continue;

                // Belt and suspenders, each entry in m_outpoint_to_orphan_wtxids should always have at least 1 announcement.
                auto it = index_by_wtxid.lower_bound(ByWtxidView{wtxid, MIN_PEER});
                if (!(it != index_by_wtxid.end() && it->m_tx->GetWitnessHash() == wtxid)) continue;

                // Select a random peer to assign orphan processing, reducing wasted work if the orphan is still missing
                // inputs. However, we don't want to create an issue in which the assigned peer can purposefully stop us
                // from processing the orphan by disconnecting.
                auto it_end = index_by_wtxid.upper_bound(ByWtxidView{wtxid, MAX_PEER});
                const auto num_announcers{std::distance(it, it_end)};
                if (!(num_announcers > 0)) continue;
                std::advance(it, rng.randrange(num_announcers));

                if (!(it->m_tx->GetWitnessHash() == wtxid)) break;

                // Mark this orphan as ready to be reconsidered.
                static constexpr auto mark_reconsidered_modifier = [](auto& ann) { ann.m_reconsider = true; };
                assert(!it->m_reconsider);
                index_by_wtxid.modify(it, mark_reconsidered_modifier);
                ret.emplace_back(wtxid, it->m_announcer);
                m_reconsiderable_wtxids.insert(wtxid);

                LogPrint(BCLog::MEMPOOL, "added %s (wtxid=%s) to peer %d workset\n",
                            it->m_tx->GetHash().ToString(), wtxid.ToString(), it->m_announcer);
            }
        }
    }
    return ret;
}

bool TxOrphanageImpl::HaveTx(const Wtxid& wtxid) const
{
    auto it_lower = m_orphans.get<ByWtxid>().lower_bound(ByWtxidView{wtxid, MIN_PEER});
    return it_lower != m_orphans.get<ByWtxid>().end() && it_lower->m_tx->GetWitnessHash() == wtxid;
}

CTransactionRef TxOrphanageImpl::GetTx(const Wtxid& wtxid) const
{
    auto it_lower = m_orphans.get<ByWtxid>().lower_bound(ByWtxidView{wtxid, MIN_PEER});
    if (it_lower != m_orphans.get<ByWtxid>().end() && it_lower->m_tx->GetWitnessHash() == wtxid) return it_lower->m_tx;
    return nullptr;
}

bool TxOrphanageImpl::HaveTxFromPeer(const Wtxid& wtxid, NodeId peer) const
{
    return m_orphans.get<ByWtxid>().count(ByWtxidView{wtxid, peer}) > 0;
}

bool TxOrphanageImpl::HaveTxByTxid(const uint256& txid) const
{
    return m_txid_to_wtxid.count(txid) > 0;
}

CTransactionRef TxOrphanageImpl::GetTxByTxid(const uint256& txid) const
{
    auto it = m_txid_to_wtxid.find(txid);
    if (it == m_txid_to_wtxid.end()) return nullptr;
    return GetTx(it->second);
}

NodeId TxOrphanageImpl::GetPrimaryAnnouncer(const uint256& txid) const
{
    auto it = m_txid_to_wtxid.find(txid);
    if (it == m_txid_to_wtxid.end()) return -1;
    return PrimaryAnnouncer(it->second);
}

/** If there is a tx that can be reconsidered, return it and set it back to
 * non-reconsiderable. Otherwise, return a nullptr. */
CTransactionRef TxOrphanageImpl::GetTxToReconsider(NodeId peer)
{
    auto it = m_orphans.get<ByPeer>().lower_bound(ByPeerView{peer, true, 0});
    if (it != m_orphans.get<ByPeer>().end() && it->m_announcer == peer && it->m_reconsider) {
        // Flip m_reconsider. Even if this transaction stays in orphanage, it shouldn't be
        // reconsidered again until there is a new reason to do so.
        static constexpr auto mark_reconsidered_modifier = [](auto& ann) { ann.m_reconsider = false; };
        m_orphans.get<ByPeer>().modify(it, mark_reconsidered_modifier);
        // As there is exactly one m_reconsider announcement per reconsiderable wtxids, flipping
        // the m_reconsider flag means the wtxid is no longer reconsiderable.
        m_reconsiderable_wtxids.erase(Wtxid::FromUint256(it->m_tx->GetWitnessHash()));
        return it->m_tx;
    }
    return nullptr;
}

/** Return whether there is a tx that can be reconsidered. */
bool TxOrphanageImpl::HaveTxToReconsider(NodeId peer)
{
    auto it = m_orphans.get<ByPeer>().lower_bound(ByPeerView{peer, true, 0});
    return it != m_orphans.get<ByPeer>().end() && it->m_announcer == peer && it->m_reconsider;
}

void TxOrphanageImpl::EraseForBlock(const CBlock& block)
{
    if (m_orphans.empty()) return;

    std::set<Wtxid> wtxids_to_erase;
    for (const CTransactionRef& ptx : block.vtx) {
        const CTransaction& block_tx = *ptx;

        // Which orphan pool entries must we evict?
        for (const auto& input : block_tx.vin) {
            auto it_prev = m_outpoint_to_orphan_wtxids.find(input.prevout);
            if (it_prev != m_outpoint_to_orphan_wtxids.end()) {
                // Copy all wtxids to wtxids_to_erase.
                std::copy(it_prev->second.cbegin(), it_prev->second.cend(), std::inserter(wtxids_to_erase, wtxids_to_erase.end()));
            }
        }
    }

    unsigned int num_erased{0};
    for (const auto& wtxid : wtxids_to_erase) {
        // Don't use EraseTx here because it calls LimitOrphans and announcements deleted in that call are not reflected
        // in its return result. Waiting until the end to do LimitOrphans helps save repeated computation and allows us
        // to check that num_erased is what we expect.
        num_erased += EraseTxInternal(wtxid) ? 1 : 0;
    }

    if (num_erased != 0) {
        LogPrint(BCLog::MEMPOOL, "Erased %d orphan transaction(s) included or conflicted by block\n", num_erased);
    }
    assert(wtxids_to_erase.size() == num_erased);

    // Deletions can cause the orphanage's MaxGlobalUsage to decrease, so we may need to trim here.
    LimitOrphans();
}

std::vector<CTransactionRef> TxOrphanageImpl::GetChildrenFromSamePeer(const CTransactionRef& parent, NodeId peer) const
{
    std::vector<CTransactionRef> children_found;
    const auto& parent_txid{parent->GetHash()};

    // Iterate through all orphans from this peer, in reverse order, so that more recent
    // transactions are added first. Doing so helps avoid work when one of the orphans replaced
    // an earlier one. Since we require the NodeId to match, one peer's announcement order does
    // not bias how we process other peer's orphans.
    auto& index_by_peer = m_orphans.get<ByPeer>();
    auto it_upper = index_by_peer.upper_bound(ByPeerView{peer, true, std::numeric_limits<uint64_t>::max()});
    auto it_lower = index_by_peer.lower_bound(ByPeerView{peer, false, 0});

    while (it_upper != it_lower) {
        --it_upper;
        if (!(it_upper->m_announcer == peer)) break;
        // Check if this tx spends from parent.
        for (const auto& input : it_upper->m_tx->vin) {
            if (input.prevout.hash == parent_txid) {
                children_found.emplace_back(it_upper->m_tx);
                break;
            }
        }
    }
    return children_found;
}

std::vector<std::pair<uint256, NodeId>> TxOrphanageImpl::GetChildren(const CTransactionRef& parent) const
{
    std::vector<std::pair<uint256, NodeId>> children_found;
    if (m_orphans.empty()) return children_found;

    const auto& parent_txid{parent->GetHash()};
    std::set<Wtxid> seen;
    for (unsigned int i = 0; i < parent->vout.size(); ++i) {
        auto it_by_prev = m_outpoint_to_orphan_wtxids.find(COutPoint(parent_txid, i));
        if (it_by_prev == m_outpoint_to_orphan_wtxids.end()) continue;
        for (const auto& wtxid : it_by_prev->second) {
            // A child that spends more than one output of the parent is only returned once.
            if (!seen.insert(wtxid).second) continue;
            const CTransactionRef child = GetTx(wtxid);
            if (!child) continue;
            children_found.emplace_back(child->GetHash(), PrimaryAnnouncer(wtxid));
        }
    }
    return children_found;
}

std::vector<TxOrphanage::OrphanInfo> TxOrphanageImpl::GetOrphanTransactions() const
{
    std::vector<TxOrphanage::OrphanInfo> result;
    result.reserve(m_unique_orphans);

    auto& index_by_wtxid = m_orphans.get<ByWtxid>();
    auto it = index_by_wtxid.begin();
    std::set<NodeId> this_orphan_announcers;
    while (it != index_by_wtxid.end()) {
        this_orphan_announcers.insert(it->m_announcer);
        // If this is the last entry, or the next entry has a different wtxid, build a OrphanInfo.
        if (std::next(it) == index_by_wtxid.end() || std::next(it)->m_tx->GetWitnessHash() != it->m_tx->GetWitnessHash()) {
            result.emplace_back(it->m_tx, std::move(this_orphan_announcers));
            this_orphan_announcers.clear();
        }

        ++it;
    }
    assert(m_unique_orphans == static_cast<TxOrphanage::Count>(result.size()));

    return result;
}

void TxOrphanageImpl::SanityCheck() const
{
    std::unordered_map<NodeId, PeerDoSInfo> reconstructed_peer_info;
    std::map<Wtxid, std::pair<TxOrphanage::Usage, TxOrphanage::Count>> unique_wtxids_to_scores;
    std::set<COutPoint> all_outpoints;
    std::set<Wtxid> reconstructed_reconsiderable_wtxids;

    for (auto it = m_orphans.begin(); it != m_orphans.end(); ++it) {
        for (const auto& input : it->m_tx->vin) {
            all_outpoints.insert(input.prevout);
        }
        unique_wtxids_to_scores.emplace(Wtxid::FromUint256(it->m_tx->GetWitnessHash()), std::make_pair(it->GetMemUsage(), it->GetLatencyScore() - 1));

        auto& peer_info = reconstructed_peer_info[it->m_announcer];
        peer_info.m_total_usage += it->GetMemUsage();
        peer_info.m_count_announcements += 1;
        peer_info.m_total_latency_score += it->GetLatencyScore();

        if (it->m_reconsider) {
            auto [_, added] = reconstructed_reconsiderable_wtxids.insert(Wtxid::FromUint256(it->m_tx->GetWitnessHash()));
            // Check that there is only ever 1 announcement per wtxid with m_reconsider set.
            assert(added);
        }
    }
    assert(reconstructed_peer_info.size() == m_peer_orphanage_info.size());

    // Recalculated per-peer stats are identical to m_peer_orphanage_info
    assert(reconstructed_peer_info == m_peer_orphanage_info);

    // Recalculated set of reconsiderable wtxids must match.
    assert(m_reconsiderable_wtxids == reconstructed_reconsiderable_wtxids);

    // All outpoints exist in m_outpoint_to_orphan_wtxids, all keys in m_outpoint_to_orphan_wtxids correspond to some
    // orphan, and all wtxids referenced in m_outpoint_to_orphan_wtxids are also in m_orphans.
    // This ensures m_outpoint_to_orphan_wtxids is cleaned up.
    assert(all_outpoints.size() == m_outpoint_to_orphan_wtxids.size());
    for (const auto& [outpoint, wtxid_set] : m_outpoint_to_orphan_wtxids) {
        assert(all_outpoints.contains(outpoint));
        for (const auto& wtxid : wtxid_set) {
            assert(unique_wtxids_to_scores.contains(wtxid));
        }
    }

    // Cached m_unique_orphans value is correct.
    assert(m_orphans.size() >= m_unique_orphans);
    assert(m_orphans.size() <= m_peer_orphanage_info.size() * m_unique_orphans);
    assert(unique_wtxids_to_scores.size() == m_unique_orphans);

    const auto calculated_dedup_usage = std::accumulate(unique_wtxids_to_scores.begin(), unique_wtxids_to_scores.end(),
        TxOrphanage::Usage{0}, [](TxOrphanage::Usage sum, const auto pair) { return sum + pair.second.first; });
    assert(calculated_dedup_usage == m_unique_orphan_usage);

    // Global usage is deduplicated, should be less than or equal to the sum of all per-peer usages.
    const auto summed_peer_usage = std::accumulate(m_peer_orphanage_info.begin(), m_peer_orphanage_info.end(),
        TxOrphanage::Usage{0}, [](TxOrphanage::Usage sum, const auto pair) { return sum + pair.second.m_total_usage; });
    assert(summed_peer_usage >= m_unique_orphan_usage);

    // Cached m_unique_rounded_input_scores value is correct.
    const auto calculated_total_latency_score = std::accumulate(unique_wtxids_to_scores.begin(), unique_wtxids_to_scores.end(),
        TxOrphanage::Count{0}, [](TxOrphanage::Count sum, const auto pair) { return sum + pair.second.second; });
    assert(calculated_total_latency_score == m_unique_rounded_input_scores);

    // Global latency score is deduplicated, should be less than or equal to the sum of all per-peer latency scores.
    const auto summed_peer_latency_score = std::accumulate(m_peer_orphanage_info.begin(), m_peer_orphanage_info.end(),
        TxOrphanage::Count{0}, [](TxOrphanage::Count sum, const auto pair) { return sum + pair.second.m_total_latency_score; });
    assert(summed_peer_latency_score >= m_unique_rounded_input_scores + m_orphans.size());

    // The txid index must be consistent with the stored orphans (chain specific).
    assert(m_txid_to_wtxid.size() == static_cast<size_t>(m_unique_orphans));
    for (const auto& [txid, wtxid] : m_txid_to_wtxid) {
        assert(HaveTx(wtxid));
        assert(GetTx(wtxid)->GetHash() == txid);
    }

    assert(!NeedsTrim());
}

TxOrphanage::Count TxOrphanageImpl::MaxGlobalLatencyScore() const { return m_max_global_latency_score; }
TxOrphanage::Count TxOrphanageImpl::TotalLatencyScore() const { return m_unique_rounded_input_scores + m_orphans.size(); }
TxOrphanage::Usage TxOrphanageImpl::ReservedPeerUsage() const { return m_reserved_usage_per_peer; }
TxOrphanage::Count TxOrphanageImpl::MaxPeerLatencyScore() const { return std::max<unsigned int>(1, m_max_global_latency_score / std::max<unsigned int>(m_peer_orphanage_info.size(), 1)); }
TxOrphanage::Usage TxOrphanageImpl::MaxGlobalUsage() const { return m_reserved_usage_per_peer * std::max<int64_t>(m_peer_orphanage_info.size(), 1); }

bool TxOrphanageImpl::NeedsTrim() const
{
    return TotalLatencyScore() > MaxGlobalLatencyScore() || TotalOrphanUsage() > MaxGlobalUsage();
}
std::unique_ptr<TxOrphanage> MakeTxOrphanage() noexcept
{
    return std::make_unique<TxOrphanageImpl>();
}
std::unique_ptr<TxOrphanage> MakeTxOrphanage(TxOrphanage::Count max_global_latency_score, TxOrphanage::Usage reserved_peer_usage) noexcept
{
    return std::make_unique<TxOrphanageImpl>(max_global_latency_score, reserved_peer_usage);
}
} // namespace node
