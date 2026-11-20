// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the JSON banlist persistence (banlist.json) and the
// migration from the legacy binary banlist.dat.

#include <banman.h>
#include <chainparams.h>
#include <clientversion.h>
#include <fs.h>
#include <hash.h>
#include <netaddress.h>
#include <netbase.h>
#include <serialize.h>
#include <streams.h>
#include <util/system.h>
#include <util/time.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static fs::path BanlistJsonPath()
{
    return GetDataDir() / "banlist.json";
}

static fs::path BanlistDatPath()
{
    return GetDataDir() / "banlist.dat";
}

static std::string ReadFile(const fs::path& path)
{
    std::ifstream file(fs::PathToString(path));
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void WriteFile(const fs::path& path, const std::string& contents)
{
    std::ofstream file(fs::PathToString(path), std::ios::out | std::ios::trunc);
    file << contents;
}

static CSubNet SubNetFromString(const std::string& str)
{
    CSubNet subnet;
    BOOST_REQUIRE(LookupSubNet(str.c_str(), subnet));
    return subnet;
}

// Fixture that makes sure that no banlist files leak into other test cases.
class BanmanTest : public TestingSetup
{
public:
    BanmanTest() : TestingSetup()
    {
        fs::remove(BanlistJsonPath());
        fs::remove(BanlistDatPath());
    }
    ~BanmanTest()
    {
        fs::remove(BanlistJsonPath());
        fs::remove(BanlistDatPath());
    }
};

BOOST_FIXTURE_TEST_SUITE(banman_tests, BanmanTest)

// The JSON banlist is written to banlist.json and can be read back,
// including the chain specific ban_reason extension.
BOOST_AUTO_TEST_CASE(banlist_json_roundtrip)
{
    banmap_t bans;
    bans.insert_or_assign(SubNetFromString("1.2.3.4"), CBanEntry(1000, BanReasonManuallyAdded));
    bans.insert_or_assign(SubNetFromString("10.0.0.0/8"), CBanEntry(2000, BanReasonNodeMisbehaving));

    CBanDB db(GetDataDir() / "banlist");
    BOOST_REQUIRE(db.Write(bans));
    BOOST_CHECK(fs::exists(BanlistJsonPath()));
    // The legacy file is never written.
    BOOST_CHECK(!fs::exists(BanlistDatPath()));

    banmap_t loaded;
    bool dirty = false;
    BOOST_REQUIRE(db.Read(loaded, dirty));
    BOOST_CHECK(!dirty);
    BOOST_CHECK_EQUAL(loaded.size(), 2U);

    auto it = loaded.find(SubNetFromString("1.2.3.4"));
    BOOST_REQUIRE(it != loaded.end());
    BOOST_CHECK_EQUAL(it->second.nCreateTime, 1000);
    BOOST_CHECK_EQUAL(it->second.nBanUntil, 0);
    BOOST_CHECK_EQUAL(static_cast<int>(it->second.banReason), static_cast<int>(BanReasonManuallyAdded));
    BOOST_CHECK_EQUAL(it->second.banReasonToString(), "manually added");

    it = loaded.find(SubNetFromString("10.0.0.0/8"));
    BOOST_REQUIRE(it != loaded.end());
    BOOST_CHECK_EQUAL(it->second.nCreateTime, 2000);
    BOOST_CHECK_EQUAL(static_cast<int>(it->second.banReason), static_cast<int>(BanReasonNodeMisbehaving));
    BOOST_CHECK_EQUAL(it->second.banReasonToString(), "node misbehaving");
}

// Onion addresses are stored in their string form and survive the round-trip.
BOOST_AUTO_TEST_CASE(banlist_json_onion_roundtrip)
{
    const std::string onion = "6hzph5hv6337r6p2.onion";
    banmap_t bans;
    bans.insert_or_assign(SubNetFromString(onion), CBanEntry(1234, BanReasonManuallyAdded));

    CBanDB db(GetDataDir() / "banlist");
    BOOST_REQUIRE(db.Write(bans));

    const std::string contents = ReadFile(BanlistJsonPath());
    BOOST_CHECK(contents.find("banned_nets") != std::string::npos);
    BOOST_CHECK(contents.find(onion) != std::string::npos);

    banmap_t loaded;
    bool dirty = false;
    BOOST_REQUIRE(db.Read(loaded, dirty));
    BOOST_CHECK_EQUAL(loaded.size(), 1U);

    auto it = loaded.find(SubNetFromString(onion));
    BOOST_REQUIRE(it != loaded.end());
    BOOST_CHECK_EQUAL(it->second.nCreateTime, 1234);
    BOOST_CHECK_EQUAL(static_cast<int>(it->second.banReason), static_cast<int>(BanReasonManuallyAdded));
}

// An entry without the chain specific ban_reason (as written by upstream) is
// accepted and mapped to BanReasonUnknown.
BOOST_AUTO_TEST_CASE(banlist_json_without_ban_reason)
{
    WriteFile(BanlistJsonPath(),
        "{\n"
        "  \"banned_nets\": [\n"
        "    {\n"
        "      \"version\": 1,\n"
        "      \"address\": \"5.6.7.8/32\",\n"
        "      \"ban_created\": 42,\n"
        "      \"banned_until\": 4242\n"
        "    }\n"
        "  ]\n"
        "}\n");

    CBanDB db(GetDataDir() / "banlist");
    banmap_t loaded;
    bool dirty = false;
    BOOST_REQUIRE(db.Read(loaded, dirty));
    BOOST_CHECK(!dirty);
    BOOST_CHECK_EQUAL(loaded.size(), 1U);

    auto it = loaded.find(SubNetFromString("5.6.7.8"));
    BOOST_REQUIRE(it != loaded.end());
    BOOST_CHECK_EQUAL(it->second.nCreateTime, 42);
    BOOST_CHECK_EQUAL(it->second.nBanUntil, 4242);
    BOOST_CHECK_EQUAL(static_cast<int>(it->second.banReason), static_cast<int>(BanReasonUnknown));
    BOOST_CHECK_EQUAL(it->second.banReasonToString(), "unknown");
}

// Invalid entries are dropped individually, valid entries survive and the file
// is rewritten in a clean state (upstream #22362).
BOOST_AUTO_TEST_CASE(banlist_json_invalid_entries)
{
    WriteFile(BanlistJsonPath(),
        "{\n"
        "  \"banned_nets\": [\n"
        "    {\n"
        "      \"version\": 1,\n"
        "      \"address\": \"not-an-address\",\n"
        "      \"ban_created\": 42,\n"
        "      \"banned_until\": 4242\n"
        "    },\n"
        "    {\n"
        "      \"version\": 1,\n"
        "      \"address\": \"9.9.9.9/32\",\n"
        "      \"ban_created\": 1,\n"
        "      \"banned_until\": 2\n"
        "    },\n"
        "    {\n"
        "      \"version\": 2,\n"
        "      \"address\": \"8.8.8.8/32\",\n"
        "      \"ban_created\": 1,\n"
        "      \"banned_until\": 2\n"
        "    }\n"
        "  ]\n"
        "}\n");

    CBanDB db(GetDataDir() / "banlist");
    banmap_t loaded;
    bool dirty = false;
    BOOST_REQUIRE(db.Read(loaded, dirty));
    // Two entries were dropped, so the file has to be rewritten.
    BOOST_CHECK(dirty);
    BOOST_CHECK_EQUAL(loaded.size(), 1U);
    BOOST_CHECK(loaded.find(SubNetFromString("9.9.9.9")) != loaded.end());
}

// A completely unparsable file leads to an empty banlist, the file is
// recreated and no ban is lost silently.
BOOST_AUTO_TEST_CASE(banlist_json_broken_file)
{
    WriteFile(BanlistJsonPath(), "{ this is not json");

    CBanDB db(GetDataDir() / "banlist");
    banmap_t loaded;
    bool dirty = false;
    BOOST_CHECK(!db.Read(loaded, dirty));
    BOOST_CHECK(dirty);
    BOOST_CHECK(loaded.empty());
}

// The legacy binary banlist.dat is read once as a migration fallback, is never
// written again and is never removed.
BOOST_AUTO_TEST_CASE(banlist_dat_migration)
{
    // Write a legacy banlist.dat using the binary format of the old CBanDB.
    {
        banmap_t legacy;
        legacy.insert_or_assign(SubNetFromString("1.2.3.4"), CBanEntry(1000, BanReasonManuallyAdded));
        CAutoFile fileout(fsbridge::fopen(BanlistDatPath(), "wb"), SER_DISK, CLIENT_VERSION);
        CHashWriter hasher(SER_DISK, CLIENT_VERSION);
        fileout << Params().MessageStart() << legacy;
        hasher << Params().MessageStart() << legacy;
        fileout << hasher.GetHash();
        fileout.fclose();
    }
    BOOST_REQUIRE(fs::exists(BanlistDatPath()));

    CBanDB db(GetDataDir() / "banlist");
    banmap_t loaded;
    bool dirty = false;
    BOOST_REQUIRE(db.Read(loaded, dirty));
    // The migrated bans have to be flushed to the JSON file.
    BOOST_CHECK(dirty);
    BOOST_CHECK_EQUAL(loaded.size(), 1U);
    BOOST_CHECK(loaded.find(SubNetFromString("1.2.3.4")) != loaded.end());

    // The legacy file is left untouched.
    BOOST_CHECK(fs::exists(BanlistDatPath()));

    // Once the JSON file exists, the legacy file is ignored.
    BOOST_REQUIRE(db.Write(loaded));
    banmap_t loaded2;
    bool dirty2 = false;
    BOOST_REQUIRE(db.Read(loaded2, dirty2));
    BOOST_CHECK_EQUAL(loaded2.size(), 1U);
}

// Tor v3 onion addresses and I2P destinations have no representation in the
// legacy (pre-BIP155) subnet model the banlist is built on. Banning them is
// rejected explicitly instead of silently creating a ban entry that can never
// match an incoming address.
BOOST_AUTO_TEST_CASE(ban_privacy_network_address_rejected)
{
    BanMan banman(GetDataDir() / "banlist", nullptr, DEFAULT_MISBEHAVING_BANTIME);

    const std::string onion_v3 = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion";
    CNetAddr onion_addr;
    BOOST_REQUIRE(LookupHost(onion_v3.c_str(), onion_addr, false));
    BOOST_CHECK(onion_addr.IsTorV3());
    BOOST_CHECK(!onion_addr.IsAddrV1Compatible());
    // The address cannot be expressed as a subnet.
    BOOST_CHECK(!CSubNet(onion_addr).IsValid());

    banman.Ban(onion_addr, BanReasonManuallyAdded);
    BOOST_CHECK(!banman.IsBanned(onion_addr));
    banmap_t bans;
    banman.GetBanned(bans);
    BOOST_CHECK(bans.empty());

    const std::string i2p = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p";
    CNetAddr i2p_addr;
    BOOST_REQUIRE(LookupHost(i2p.c_str(), i2p_addr, false));
    BOOST_CHECK(i2p_addr.IsI2P());
    BOOST_CHECK(!i2p_addr.IsAddrV1Compatible());

    banman.Ban(i2p_addr, BanReasonManuallyAdded);
    BOOST_CHECK(!banman.IsBanned(i2p_addr));
    banman.GetBanned(bans);
    BOOST_CHECK(bans.empty());

    // A legacy Tor v2 (OnionCat) address still has a legacy representation and
    // can be banned as before.
    const std::string onion_v2 = "6hzph5hv6337r6p2.onion";
    CNetAddr onion_v2_addr;
    BOOST_REQUIRE(LookupHost(onion_v2.c_str(), onion_v2_addr, false));
    BOOST_CHECK(onion_v2_addr.IsTor());
    BOOST_CHECK(!onion_v2_addr.IsTorV3());
    BOOST_CHECK(onion_v2_addr.IsAddrV1Compatible());

    banman.Ban(onion_v2_addr, BanReasonManuallyAdded);
    BOOST_CHECK(banman.IsBanned(onion_v2_addr));
}

BOOST_AUTO_TEST_SUITE_END()
