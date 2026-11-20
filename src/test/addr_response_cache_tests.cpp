// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <addr_response_cache.h>
#include <test/setup_common.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>

#include <string>
#include <vector>

namespace {

CNetAddr MakeIPv4(const char* ip)
{
    struct in_addr addr;
    BOOST_REQUIRE(inet_pton(AF_INET, ip, &addr) == 1);
    return CNetAddr(addr);
}

CNetAddr MakeIPv6(const char* ip)
{
    struct in6_addr addr;
    BOOST_REQUIRE(inet_pton(AF_INET6, ip, &addr) == 1);
    return CNetAddr(addr);
}

CAddress MakeAddress(const CNetAddr& ip, unsigned short port)
{
    return CAddress(CService(ip, port), NODE_NETWORK);
}

//! A deterministic, non-empty sample of addresses.
std::vector<CAddress> MakeSample(uint16_t base)
{
    std::vector<CAddress> addrs;
    for (uint16_t i = 0; i < 5; ++i) {
        addrs.emplace_back(MakeAddress(MakeIPv4("1.2.3.4"), static_cast<unsigned short>(base + i)));
    }
    return addrs;
}

//! Human readable representation of an address sample, used for comparisons.
std::vector<std::string> Describe(const std::vector<CAddress>& addrs)
{
    std::vector<std::string> out;
    out.reserve(addrs.size());
    for (const CAddress& addr : addrs) {
        out.push_back(addr.ToStringIPPort());
    }
    return out;
}

constexpr uint64_t SEED0{0x0123456789abcdefULL};
constexpr uint64_t SEED1{0xfedcba9876543210ULL};

} // namespace

BOOST_FIXTURE_TEST_SUITE(addr_response_cache_tests, BasicTestingSetup)

//! U1: a cache hit returns the identical sample and does not re-fetch.
BOOST_AUTO_TEST_CASE(cache_hit)
{
    CAddrResponseCache cache(SEED0, SEED1);
    const CNetAddr requestor = MakeIPv4("8.8.8.8");
    const CAddress local_bind = MakeAddress(MakeIPv4("127.0.0.1"), 12345);
    const uint64_t key = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, local_bind, true);

    int fetches{0};
    const auto fetch = [&]() {
        ++fetches;
        return MakeSample(1000);
    };

    const std::vector<CAddress> first = cache.Get(key, fetch);
    const std::vector<CAddress> second = cache.Get(key, fetch);

    BOOST_CHECK_EQUAL(fetches, 1);
    BOOST_CHECK_EQUAL(cache.size(), 1U);
    BOOST_CHECK(Describe(first) == Describe(second));
    BOOST_CHECK_EQUAL(first.size(), 5U);
}

//! U2: keys of different requester netclasses, different local sockets and
//! different connection directions are separated.
BOOST_AUTO_TEST_CASE(key_separation)
{
    CAddrResponseCache cache(SEED0, SEED1);
    const CNetAddr ipv4_requestor = MakeIPv4("8.8.8.8");
    const CNetAddr ipv6_requestor = MakeIPv6("2001:db8::1");
    const CAddress bind_a = MakeAddress(MakeIPv4("127.0.0.1"), 12345);
    const CAddress bind_b = MakeAddress(MakeIPv4("127.0.0.1"), 12346);

    const uint64_t key_v4 = ComputeAddrResponseNetworkKey(SEED0, SEED1, ipv4_requestor, bind_a, true);
    const uint64_t key_v6 = ComputeAddrResponseNetworkKey(SEED0, SEED1, ipv6_requestor, bind_a, true);
    const uint64_t key_other_socket = ComputeAddrResponseNetworkKey(SEED0, SEED1, ipv4_requestor, bind_b, true);
    const uint64_t key_outbound = ComputeAddrResponseNetworkKey(SEED0, SEED1, ipv4_requestor, bind_a, false);

    BOOST_CHECK(key_v4 != key_v6);
    BOOST_CHECK(key_v4 != key_other_socket);
    BOOST_CHECK(key_v4 != key_outbound);

    int fetches{0};
    const auto fetch = [&]() {
        ++fetches;
        return MakeSample(1000);
    };

    cache.Get(key_v4, fetch);
    cache.Get(key_v6, fetch);
    cache.Get(key_other_socket, fetch);
    cache.Get(key_outbound, fetch);

    BOOST_CHECK_EQUAL(fetches, 4);
    BOOST_CHECK_EQUAL(cache.size(), 4U);
}

//! U3: for outbound connections the ephemeral local port is not part of the
//! key, so the cache cannot grow per connection.
BOOST_AUTO_TEST_CASE(outbound_port_rule)
{
    const CNetAddr requestor = MakeIPv4("8.8.8.8");
    const CAddress bind_a = MakeAddress(MakeIPv4("127.0.0.1"), 40001);
    const CAddress bind_b = MakeAddress(MakeIPv4("127.0.0.1"), 40002);

    const uint64_t key_a = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, bind_a, false);
    const uint64_t key_b = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, bind_b, false);
    BOOST_CHECK_EQUAL(key_a, key_b);

    // The same holds for inbound connections, where the bind port *is* part of
    // the key (different local listening sockets).
    const uint64_t inbound_a = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, bind_a, true);
    const uint64_t inbound_b = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, bind_b, true);
    BOOST_CHECK(inbound_a != inbound_b);
}

//! U4: an expired entry is refreshed.
BOOST_AUTO_TEST_CASE(expiry)
{
    const int64_t start_time{1600000000};
    SetMockTime(start_time);

    CAddrResponseCache cache(SEED0, SEED1);
    const CNetAddr requestor = MakeIPv4("8.8.8.8");
    const CAddress local_bind = MakeAddress(MakeIPv4("127.0.0.1"), 12345);
    const uint64_t key = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, local_bind, true);

    int fetches{0};
    const auto fetch = [&]() {
        ++fetches;
        return MakeSample(static_cast<uint16_t>(1000 * fetches));
    };

    const std::vector<CAddress> first = cache.Get(key, fetch);
    BOOST_CHECK_EQUAL(fetches, 1);

    // Still cached well before the maximum lifetime (21h + 6h jitter).
    SetMockTime(start_time + 3600);
    const std::vector<CAddress> cached = cache.Get(key, fetch);
    BOOST_CHECK_EQUAL(fetches, 1);
    BOOST_CHECK(Describe(first) == Describe(cached));

    // Expired after the maximum lifetime.
    SetMockTime(start_time + 27 * 3600 + 1);
    const std::vector<CAddress> refreshed = cache.Get(key, fetch);
    BOOST_CHECK_EQUAL(fetches, 2);
    BOOST_CHECK(Describe(first) != Describe(refreshed));
    BOOST_CHECK_EQUAL(cache.size(), 1U);

    SetMockTime(0);
}

//! U5: the map does not grow beyond the number of distinct keys.
BOOST_AUTO_TEST_CASE(bounded_map)
{
    CAddrResponseCache cache(SEED0, SEED1);
    int fetches{0};
    const auto fetch = [&]() {
        ++fetches;
        return MakeSample(1000);
    };

    for (int i = 0; i < 100; ++i) {
        cache.Get(42, fetch);
    }
    BOOST_CHECK_EQUAL(cache.size(), 1U);
    BOOST_CHECK_EQUAL(fetches, 1);

    for (uint64_t i = 0; i < 10; ++i) {
        cache.Get(1000 + i, fetch);
    }
    BOOST_CHECK_EQUAL(cache.size(), 11U);

    cache.Clear();
    BOOST_CHECK_EQUAL(cache.size(), 0U);
}

//! U6: the key derivation is deterministic and node specific.
BOOST_AUTO_TEST_CASE(key_determinism)
{
    const CNetAddr requestor = MakeIPv4("8.8.8.8");
    const CAddress local_bind = MakeAddress(MakeIPv4("127.0.0.1"), 12345);

    const uint64_t key = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, local_bind, true);
    const uint64_t key_again = ComputeAddrResponseNetworkKey(SEED0, SEED1, requestor, local_bind, true);
    BOOST_CHECK_EQUAL(key, key_again);

    // A different node seed yields a different key (per node randomization).
    const uint64_t key_other_seed = ComputeAddrResponseNetworkKey(SEED0 ^ 1ULL, SEED1, requestor, local_bind, true);
    BOOST_CHECK(key != key_other_seed);
}

BOOST_AUTO_TEST_SUITE_END()
