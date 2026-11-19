// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_ADDR_RESPONSE_CACHE_H
#define DEFI_ADDR_RESPONSE_CACHE_H

#include <netaddress.h>
#include <protocol.h>
#include <sync.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

/**
 * Randomizer id used to derive the "network key" of a connection.
 *
 * Upstream parity: SHA256("networkkey")[0:8] (bitcoin-core/src/net.cpp).
 * It is a third, distinct id next to RANDOMIZER_ID_NETGROUP and
 * RANDOMIZER_ID_LOCALHOSTNONCE - reusing one of those would collapse two
 * separate key spaces.
 */
static const uint64_t RANDOMIZER_ID_NETWORKKEY = 0x0e8a2b136c592a7dULL;

/**
 * Cache for the responses to "getaddr" messages.
 *
 * Attack example: scraping addresses in real time may allow an attacker to
 * infer new connections of the victim by detecting new records with fresh
 * timestamps (per self-announcement). Serving the very same sample to every
 * requestor within the cache lifetime removes that leak.
 *
 * The cache is keyed per (requester netclass, local listening socket), so that
 * a node which is reachable over multiple networks is not linkable across
 * them and a multi-homed node does not leak through a shared cache.
 *
 * The address source is injected (FetchFn) so that the cache logic can be
 * unit tested without a live connection manager.
 */
class CAddrResponseCache
{
public:
    //! Function returning a fresh sample of addresses.
    using FetchFn = std::function<std::vector<CAddress>()>;

    explicit CAddrResponseCache(uint64_t seed0, uint64_t seed1)
        : m_seed0(seed0), m_seed1(seed1) {}

    /**
     * Return the cached address sample for `key`.
     *
     * On a cache miss or an expired entry the sample is (re-)fetched through
     * `fetch` and cached until it expires.
     */
    std::vector<CAddress> Get(uint64_t key, const FetchFn& fetch);

    /**
     * Derive the cache key of a connection.
     *
     * @param[in] requestor  The address of the peer requesting the addresses.
     * @param[in] local_bind Our own bound socket address.
     * @param[in] inbound    True for inbound connections (the bind port is
     *                       part of the key), false for outbound connections
     *                       (the ephemeral local port is forced to 0 and thus
     *                       not part of the key).
     */
    uint64_t ComputeKey(const CNetAddr& requestor, const CAddress& local_bind, bool inbound) const;

    //! Number of cached entries (test helper).
    size_t size() const;

    //! Drop all cached entries (test helper).
    void Clear();

private:
    struct CachedAddrResponse {
        std::vector<CAddress> m_addrs;
        std::chrono::microseconds m_expiration{0};
    };

    mutable Mutex cs_cache;
    std::map<uint64_t, CachedAddrResponse> m_entries;
    const uint64_t m_seed0;
    const uint64_t m_seed1;
};

/**
 * Derive the cache key of a connection.
 *
 * The key identifies the (requester netclass, local listening socket)
 * combination and is used to keep the "getaddr" response caches of different
 * networks and of different local sockets separated.
 *
 * @param[in] seed0     SipHash key part 0 (node specific).
 * @param[in] seed1     SipHash key part 1 (node specific).
 * @param[in] requestor The address of the peer requesting the addresses.
 * @param[in] local_bind Our own bound socket address.
 * @param[in] inbound   True for inbound connections (the bind port is part of
 *                      the key), false for outbound connections (the ephemeral
 *                      local port is forced to 0 and thus not part of the key).
 */
uint64_t ComputeAddrResponseNetworkKey(uint64_t seed0, uint64_t seed1,
                                       const CNetAddr& requestor,
                                       const CAddress& local_bind,
                                       bool inbound);

#endif // DEFI_ADDR_RESPONSE_CACHE_H
