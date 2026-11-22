// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the ASMap parser (a C++17 port of the upstream Bitcoin Core
// src/util/asmap.{h,cpp}).

#include <util/asmap.h>

#include <netbase.h>
#include <test/data/asmap.raw.h>
#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(asmap_tests, BasicTestingSetup)

static CNetAddr ResolveIP(const char* ip)
{
    CNetAddr addr;
    BOOST_CHECK_MESSAGE(LookupHost(ip, addr, false), ip);
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

/** The 16 byte lookup key of an address (IPv4 addresses are mapped into ::ffff:0:0/96). */
static std::vector<unsigned char> LookupKey(const std::string& ip)
{
    const std::vector<unsigned char> bytes = ResolveIP(ip.c_str()).GetAddrBytes();
    BOOST_REQUIRE_EQUAL(bytes.size(), 16U);
    return bytes;
}

BOOST_AUTO_TEST_CASE(asmap_sanity_check)
{
    // The fixture is a valid ASMap.
    BOOST_CHECK(CheckStandardAsmap(TestAsmap()));

    // An empty map is not valid.
    BOOST_CHECK(!CheckStandardAsmap({}));
    BOOST_CHECK(!SanityCheckAsmap({}, 128));

    // Garbage is not valid.
    const std::vector<unsigned char> garbage(64, 0xff);
    BOOST_CHECK(!SanityCheckAsmap(garbage, 128));

    // A truncated map is not valid.
    std::vector<unsigned char> truncated = TestAsmap();
    truncated.resize(truncated.size() / 2);
    BOOST_CHECK(!SanityCheckAsmap(truncated, 128));

    // A map with trailing garbage is not valid.
    std::vector<unsigned char> extended = TestAsmap();
    extended.resize(extended.size() + 8, 0x00);
    BOOST_CHECK(!SanityCheckAsmap(extended, 128));
}

BOOST_AUTO_TEST_CASE(asmap_interpret)
{
    const std::vector<unsigned char> asmap = TestAsmap();

    // The longest prefix match determines the ASN.
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("1.2.3.4")), 64500U);
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("1.255.255.255")), 64500U);
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("2.3.4.5")), 64500U);
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("3.2.3.4")), 64501U);

    // The IPv6 range is mapped as well.
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("2a01:4f8::1")), 64500U);
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("2a01:4f8:ffff::ffff")), 64500U);

    // Addresses that are not covered by the map are unmapped (ASN 0).
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("4.3.2.1")), 0U);
    BOOST_CHECK_EQUAL(Interpret(asmap, LookupKey("2a02::1")), 0U);
}

BOOST_AUTO_TEST_CASE(asmap_version)
{
    // The version of an empty map is the null hash.
    BOOST_CHECK(AsmapVersion({}).IsNull());

    const std::vector<unsigned char> asmap = TestAsmap();
    const uint256 version = AsmapVersion(asmap);
    BOOST_CHECK(!version.IsNull());

    // The version only depends on the bytes of the map.
    BOOST_CHECK(AsmapVersion(asmap) == version);

    // A different map has a different version.
    std::vector<unsigned char> other = asmap;
    other[0] ^= 0xff;
    BOOST_CHECK(AsmapVersion(other) != version);
}

BOOST_AUTO_TEST_SUITE_END()
