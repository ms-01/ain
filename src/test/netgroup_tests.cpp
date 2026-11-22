// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the netgroup manager (the chain-local variant of the upstream
// Bitcoin Core src/netgroup.{h,cpp}).

#include <netgroup.h>

#include <netbase.h>
#include <test/data/asmap.raw.h>
#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(netgroup_tests, BasicTestingSetup)

static CNetAddr ResolveIP(const char* ip)
{
    CNetAddr addr;
    LookupHost(ip, addr, false);
    return addr;
}

/**
 * The test fixture is generated with the upstream tool
 * contrib/asmap/asmap-tool.py encode from the following text file:
 *
 *   1.0.0.0/8 AS64500
 *   2.0.0.0/8 AS64500
 *   3.0.0.0/8 AS64501
 *   2a01:4f8::/32 AS64500
 */
static std::vector<unsigned char> TestAsmap()
{
    return std::vector<unsigned char>(raw_tests::asmap_raw, raw_tests::asmap_raw + sizeof(raw_tests::asmap_raw));
}

static CNetAddr CreateInternal(const char* host)
{
    CNetAddr ret;
    ret.SetInternal(host);
    return ret;
}

/** The netgroup of an ASN: NET_IPV6 followed by the ASN in little endian. */
static std::vector<unsigned char> AsnGroup(uint32_t asn)
{
    std::vector<unsigned char> group;
    group.push_back(NET_IPV6);
    for (int i = 0; i < 4; ++i) {
        group.push_back((asn >> (8 * i)) & 0xFF);
    }
    return group;
}

BOOST_AUTO_TEST_CASE(netgroup_asn_group)
{
    const NetgroupManager manager = NetgroupManager::WithLoadedAsmap(TestAsmap());
    BOOST_CHECK(manager.UsingASMap());
    BOOST_CHECK(!manager.GetAsmapVersion().IsNull());

    // Two addresses of the same autonomous system share one netgroup, even if
    // they are in a different prefix.
    BOOST_CHECK(manager.GetGroup(ResolveIP("1.2.3.4")) == AsnGroup(64500));
    BOOST_CHECK(manager.GetGroup(ResolveIP("2.3.4.5")) == AsnGroup(64500));
    // The IPv6 address of the same autonomous system is in the same group.
    BOOST_CHECK(manager.GetGroup(ResolveIP("2a01:4f8::1")) == AsnGroup(64500));

    // Two different autonomous systems are in different groups.
    BOOST_CHECK(manager.GetGroup(ResolveIP("3.2.3.4")) == AsnGroup(64501));
    BOOST_CHECK(manager.GetGroup(ResolveIP("1.2.3.4")) != manager.GetGroup(ResolveIP("3.2.3.4")));

    // The addresses that embed an IPv4 address are looked up via that address.
    BOOST_CHECK(manager.GetGroup(ResolveIP("2002:102:304:9999:9999:9999:9999:9999")) == AsnGroup(64500)); // 6to4
    BOOST_CHECK(manager.GetGroup(ResolveIP("64:FF9B::102:304")) == AsnGroup(64500));                        // NAT64
    BOOST_CHECK(manager.GetGroup(ResolveIP("2001:0:9999:9999:9999:9999:FEFD:FCFB")) == AsnGroup(64500));    // Teredo

    // The mapped AS of the embedded IPv4 address is reported as well.
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("1.2.3.4")), 64500U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("2002:102:304:9999:9999:9999:9999:9999")), 64500U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("64:FF9B::102:304")), 64500U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("2001:0:9999:9999:9999:9999:FEFD:FCFB")), 64500U);
}

BOOST_AUTO_TEST_CASE(netgroup_unmapped_fallback)
{
    const NetgroupManager manager = NetgroupManager::WithLoadedAsmap(TestAsmap());

    // An address that is not covered by the map falls back to the classic
    // (prefix based) group.
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("4.3.2.1")), 0U);
    BOOST_CHECK(manager.GetGroup(ResolveIP("4.3.2.1")) == ResolveIP("4.3.2.1").GetGroup());

    // The addresses that cannot be mapped at all are never looked up.
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("127.0.0.1")), 0U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("10.0.0.1")), 0U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("5wyqrzbvrdsumnok.onion")), 0U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p")),
                      0U);
    BOOST_CHECK_EQUAL(manager.GetMappedAS(CreateInternal("baz.net")), 0U);
}

BOOST_AUTO_TEST_CASE(netgroup_classic_fallback_is_byte_identical)
{
    // The classic grouping is the one of CNetAddr::GetGroup(); it must not be
    // changed by the manager.
    const NetgroupManager no_asmap = NetgroupManager::NoAsmap();
    const NetgroupManager with_asmap = NetgroupManager::WithLoadedAsmap(TestAsmap());

    const std::vector<CNetAddr> addresses{
        ResolveIP("127.0.0.1"),                                  // local
        CreateInternal("baz.net"),                              // internal
        ResolveIP("10.0.0.1"),                                  // RFC1918, unroutable
        ResolveIP("169.254.1.1"),                               // RFC3927, unroutable
        ResolveIP("257.0.0.1"),                                 // invalid
        ResolveIP("1.2.3.4"),                                   // IPv4
        ResolveIP("::FFFF:0:102:304"),                          // RFC6145
        ResolveIP("64:FF9B::102:304"),                          // RFC6052
        ResolveIP("2002:102:304:9999:9999:9999:9999:9999"),     // RFC3964
        ResolveIP("2001:0:9999:9999:9999:9999:FEFD:FCFB"),      // RFC4380
        ResolveIP("5wyqrzbvrdsumnok.onion"),                    // Tor v2
        ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion"), // Tor v3
        ResolveIP("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p"),   // I2P
        ResolveIP("2001:470:abcd:9999:9999:9999:9999:9999"),    // he.net
        ResolveIP("2001:2001:9999:9999:9999:9999:9999:9999"),   // IPv6
    };

    for (const CNetAddr& addr : addresses) {
        // Without an ASMap the classic group is returned, byte for byte.
        BOOST_CHECK(no_asmap.GetGroup(addr) == addr.GetGroup());
        // With an ASMap the addresses that are not mapped keep the classic
        // group, byte for byte.
        if (with_asmap.GetMappedAS(addr) == 0) {
            BOOST_CHECK(with_asmap.GetGroup(addr) == addr.GetGroup());
        }
    }
}

BOOST_AUTO_TEST_CASE(netgroup_no_asmap)
{
    const NetgroupManager manager = NetgroupManager::NoAsmap();
    BOOST_CHECK(!manager.UsingASMap());
    BOOST_CHECK(manager.GetAsmapVersion().IsNull());

    // Without an ASMap no address is mapped.
    BOOST_CHECK_EQUAL(manager.GetMappedAS(ResolveIP("1.2.3.4")), 0U);
    BOOST_CHECK(manager.GetGroup(ResolveIP("1.2.3.4")) == ResolveIP("1.2.3.4").GetGroup());
}

BOOST_AUTO_TEST_CASE(netgroup_diversity)
{
    // The diversity of the outbound connections is based on the netgroup: the
    // addresses of one autonomous system occupy exactly one slot of the
    // diversity set, even if they are in different prefixes.
    const NetgroupManager manager = NetgroupManager::WithLoadedAsmap(TestAsmap());

    std::set<std::vector<unsigned char>> diversity_set;
    diversity_set.insert(manager.GetGroup(ResolveIP("1.2.3.4")));
    diversity_set.insert(manager.GetGroup(ResolveIP("2.3.4.5")));
    diversity_set.insert(manager.GetGroup(ResolveIP("2a01:4f8::1")));
    BOOST_CHECK_EQUAL(diversity_set.size(), 1U);

    diversity_set.insert(manager.GetGroup(ResolveIP("3.2.3.4")));
    BOOST_CHECK_EQUAL(diversity_set.size(), 2U);

    // Without the ASMap the two /16s are two different groups.
    const NetgroupManager no_asmap = NetgroupManager::NoAsmap();
    std::set<std::vector<unsigned char>> classic_set;
    classic_set.insert(no_asmap.GetGroup(ResolveIP("1.2.3.4")));
    classic_set.insert(no_asmap.GetGroup(ResolveIP("2.3.4.5")));
    classic_set.insert(no_asmap.GetGroup(ResolveIP("2a01:4f8::1")));
    BOOST_CHECK_EQUAL(classic_set.size(), 3U);
}

BOOST_AUTO_TEST_SUITE_END()
