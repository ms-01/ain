// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <addr_response_cache.h>

#include <crypto/siphash.h>
#include <random.h>
#include <util/time.h>

#include <chrono>

namespace {
//! Base lifetime of a cached "getaddr" response.
constexpr std::chrono::microseconds ADDR_RESPONSE_CACHE_LIFETIME{21 * 60 * 60 * 1000000LL};
//! Random jitter added to the lifetime, so that the cache lifetime is not fingerprintable.
constexpr std::chrono::microseconds ADDR_RESPONSE_CACHE_JITTER{6 * 60 * 60 * 1000000LL};
} // namespace

uint64_t ComputeAddrResponseNetworkKey(uint64_t seed0, uint64_t seed1,
                                       const CNetAddr& requestor,
                                       const CAddress& local_bind,
                                       bool inbound)
{
    CSipHasher hasher(seed0, seed1);
    hasher.Write(RANDOMIZER_ID_NETWORKKEY);
    // The netclass of the *requestor* identifies the network the request came
    // in on - the key must be keyed on the requestor's identity, not on ours.
    hasher.Write(static_cast<uint64_t>(requestor.GetNetClass()));
    // Our own bound socket address identifies the local listening socket.
    const std::vector<unsigned char> bind_bytes = local_bind.GetAddrBytes();
    hasher.Write(bind_bytes.data(), bind_bytes.size());
    // For outbound connections, the port of the bound address is randomly
    // assigned by the OS and would therefore not be useful for seeding.
    hasher.Write(inbound ? static_cast<uint64_t>(local_bind.GetPort()) : uint64_t{0});
    return hasher.Finalize();
}

uint64_t CAddrResponseCache::ComputeKey(const CNetAddr& requestor, const CAddress& local_bind, bool inbound) const
{
    return ComputeAddrResponseNetworkKey(m_seed0, m_seed1, requestor, local_bind, inbound);
}

std::vector<CAddress> CAddrResponseCache::Get(uint64_t key, const FetchFn& fetch)
{
    const auto current_time = GetTime<std::chrono::microseconds>();
    {
        LOCK(cs_cache);
        const auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.m_expiration > current_time) {
            return it->second.m_addrs;
        }
    }

    // The fetch is done *outside* of the cache lock: it acquires the AddrMan
    // lock internally, so holding cs_cache here would introduce a lock
    // ordering dependency between the cache and the address manager. A
    // concurrent miss may cause a duplicate fetch, which is benign (both
    // fetches return a valid sample and the last one wins).

    // Choosing a proper cache lifetime is a trade-off between the privacy leak
    // minimization and the usefulness of ADDR responses to honest users.
    //
    // Longer cache lifetime makes it more difficult for an attacker to scrape
    // enough AddrMan data to maliciously infer something useful.
    // By the time an attacker scraped enough AddrMan records, most of
    // the records should be old enough to not leak topology info by
    // e.g. analyzing real-time changes in timestamps.
    //
    // On the other hand, longer cache lifetime makes ADDR responses
    // outdated and less useful for an honest requestor, e.g. if most of nodes
    // in the ADDR response are no longer active.
    //
    // However, the churn in the network is known to be rather low. Since we
    // consider nodes to be "terrible" (see IsTerrible()) if the timestamps are
    // older than 30 days, max. 24 hours of "penalty" due to cache shouldn't make
    // any meaningful difference in terms of the freshness of the response.
    const auto jitter = std::chrono::microseconds{FastRandomContext().randrange(ADDR_RESPONSE_CACHE_JITTER.count())};
    const auto expiration = current_time + ADDR_RESPONSE_CACHE_LIFETIME + jitter;

    std::vector<CAddress> addrs = fetch();

    LOCK(cs_cache);
    auto& entry = m_entries[key];
    entry.m_addrs = std::move(addrs);
    entry.m_expiration = expiration;
    return entry.m_addrs;
}

size_t CAddrResponseCache::size() const
{
    LOCK(cs_cache);
    return m_entries.size();
}

void CAddrResponseCache::Clear()
{
    LOCK(cs_cache);
    m_entries.clear();
}
