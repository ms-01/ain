// Copyright (c) 2012-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.
#include <addrman.h>
#include <test/setup_common.h>
#include <string>
#include <boost/test/unit_test.hpp>

#include <clientversion.h>
#include <hash.h>
#include <map>
#include <memory>
#include <netbase.h>
#include <netgroup.h>
#include <random.h>
#include <streams.h>
#include <test/data/asmap.raw.h>
#include <test/data/asmap2.raw.h>

/**
 * The test fixtures are generated with the upstream tool
 * contrib/asmap/asmap-tool.py encode.
 *
 * asmap.raw maps:
 *   1.0.0.0/8 AS64500
 *   2.0.0.0/8 AS64500
 *   3.0.0.0/8 AS64501
 *   2a01:4f8::/32 AS64500
 *
 * asmap2.raw maps:
 *   1.0.0.0/8 AS64500
 *   2.0.0.0/8 AS64501
 *   3.0.0.0/8 AS64502
 *   2a01:4f8::/32 AS64500
 */
static std::shared_ptr<const NetgroupManager> AsmapManager1()
{
    return std::make_shared<const NetgroupManager>(NetgroupManager::WithLoadedAsmap(
        std::vector<unsigned char>(raw_tests::asmap_raw, raw_tests::asmap_raw + sizeof(raw_tests::asmap_raw))));
}

static std::shared_ptr<const NetgroupManager> AsmapManager2()
{
    return std::make_shared<const NetgroupManager>(NetgroupManager::WithLoadedAsmap(
        std::vector<unsigned char>(raw_tests::asmap2_raw, raw_tests::asmap2_raw + sizeof(raw_tests::asmap2_raw))));
}

class CAddrManTest : public CAddrMan
{
public:
    explicit CAddrManTest(bool makeDeterministic = true, std::shared_ptr<const NetgroupManager> netgroupman = DefaultNetgroupManager())
        : CAddrMan(netgroupman)
    {
        if (makeDeterministic) {
            //  Set addrman addr placement to be deterministic.
            MakeDeterministic();
        }
    }

    //! Ensure that bucket placement is always the same for testing purposes.
    void MakeDeterministic()
    {
        nKey.SetNull();
        insecure_rand = FastRandomContext(true);
    }

    CAddrInfo* Find(const CNetAddr& addr, int* pnId = nullptr)
    {
        LOCK(cs);
        return CAddrMan::Find(addr, pnId);
    }

    CAddrInfo* Create(const CAddress& addr, const CNetAddr& addrSource, int* pnId = nullptr)
    {
        LOCK(cs);
        return CAddrMan::Create(addr, addrSource, pnId);
    }

    void Delete(int nId)
    {
        LOCK(cs);
        CAddrMan::Delete(nId);
    }

    // Simulates connection failure so that we can test eviction of offline nodes
    void SimConnFail(CService& addr)
    {
         LOCK(cs);
         int64_t nLastSuccess = 1;
         Good_(addr, true, nLastSuccess); // Set last good connection in the deep past.

         bool count_failure = false;
         int64_t nLastTry = GetAdjustedTime()-61;
         Attempt(addr, count_failure, nLastTry);
     }
};

static CNetAddr ResolveIP(const char* ip)
{
    CNetAddr addr;
    BOOST_CHECK_MESSAGE(LookupHost(ip, addr, false), strprintf("failed to resolve: %s", ip));
    return addr;
}

static CNetAddr ResolveIP(std::string ip)
{
    return ResolveIP(ip.c_str());
}

static CService ResolveService(const char* ip, int port = 0)
{
    CService serv;
    BOOST_CHECK_MESSAGE(Lookup(ip, serv, port, false), strprintf("failed to resolve: %s:%i", ip, port));
    return serv;
}

static CService ResolveService(std::string ip, int port = 0)
{
    return ResolveService(ip.c_str(), port);
}

BOOST_FIXTURE_TEST_SUITE(addrman_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(addrman_simple)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    // Test: Does Addrman respond correctly when empty.
    BOOST_CHECK_EQUAL(addrman.size(), 0U);
    CAddrInfo addr_null = addrman.Select();
    BOOST_CHECK_EQUAL(addr_null.ToString(), "[::]:0");

    // Test: Does Addrman::Add work as expected.
    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);
    CAddrInfo addr_ret1 = addrman.Select();
    BOOST_CHECK_EQUAL(addr_ret1.ToString(), "250.1.1.1:8555");

    // Test: Does IP address deduplication work correctly.
    //  Expected dup IP should not be added.
    CService addr1_dup = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(!addrman.Add(CAddress(addr1_dup, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);


    // Test: New table has one addr and we add a diff addr we should
    //  have at least one addr.
    // Note that addrman's size cannot be tested reliably after insertion, as
    // hash collisions may occur. But we can always be sure of at least one
    // success.

    CService addr2 = ResolveService("250.1.1.2", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr2, NODE_NONE), source));
    BOOST_CHECK(addrman.size() >= 1);

    // Test: AddrMan::Clear() should empty the new table.
    addrman.Clear();
    BOOST_CHECK_EQUAL(addrman.size(), 0U);
    CAddrInfo addr_null2 = addrman.Select();
    BOOST_CHECK_EQUAL(addr_null2.ToString(), "[::]:0");

    // Test: AddrMan::Add multiple addresses works as expected
    std::vector<CAddress> vAddr;
    vAddr.push_back(CAddress(ResolveService("250.1.1.3", 8555), NODE_NONE));
    vAddr.push_back(CAddress(ResolveService("250.1.1.4", 8555), NODE_NONE));
    BOOST_CHECK(addrman.Add(vAddr, source));
    BOOST_CHECK(addrman.size() >= 1);
}

BOOST_AUTO_TEST_CASE(addrman_privacy_addresses_stored)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    // A Tor v3 onion address has no legacy (pre-BIP155) representation. It is
    // stored in the BIP155-capable format of the addrman and it is never
    // serialized into the legacy wire format.
    const std::string onion = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion";
    CService onion_service = ResolveService(onion, 8555);
    BOOST_CHECK(onion_service.IsTorV3());
    CAddress onion_addr(onion_service, NODE_NONE);
    onion_addr.nTime = GetAdjustedTime();
    BOOST_CHECK(addrman.Add(onion_addr, source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);

    // The same holds for I2P addresses.
    const std::string i2p = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p";
    CService i2p_service = ResolveService(i2p, 8555);
    BOOST_CHECK(i2p_service.IsI2P());
    CAddress i2p_addr(i2p_service, NODE_NONE);
    i2p_addr.nTime = GetAdjustedTime();
    BOOST_CHECK(addrman.Add(i2p_addr, source));
    BOOST_CHECK_EQUAL(addrman.size(), 2U);

    // A legacy Tor v2 address still has a v1 representation and is stored.
    CService onion_v2 = ResolveService("6hzph5hv6337r6p2.onion", 8555);
    BOOST_CHECK(onion_v2.IsTor());
    BOOST_CHECK(!onion_v2.IsTorV3());
    CAddress onion_v2_addr(onion_v2, NODE_NONE);
    onion_v2_addr.nTime = GetAdjustedTime();
    BOOST_CHECK(addrman.Add(onion_v2_addr, source));
    BOOST_CHECK_EQUAL(addrman.size(), 3U);

    // The addresses are reported by the address manager.
    const std::vector<CAddress> vAddr = addrman.GetAddr(std::nullopt, 1000, 100);
    BOOST_CHECK_EQUAL(vAddr.size(), 3U);
    bool found_onion{false};
    bool found_i2p{false};
    for (const CAddress& addr : vAddr) {
        found_onion |= addr.ToStringIP() == onion;
        found_i2p |= addr.ToStringIP() == i2p;
    }
    BOOST_CHECK(found_onion);
    BOOST_CHECK(found_i2p);
}

BOOST_AUTO_TEST_CASE(addrman_serialization_privacy_roundtrip)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");
    CNetAddr onion_source = ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion");

    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    CService onion = ResolveService("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion", 8555);
    BOOST_CHECK(addrman.Add(CAddress(onion, NODE_NONE), source));
    CService i2p = ResolveService("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p", 8555);
    // The source of an entry determines its bucket, so a privacy address can
    // be the source of an entry as well.
    BOOST_CHECK(addrman.Add(CAddress(i2p, NODE_NONE), onion_source));

    // The addrman contains addresses that need the BIP155 format.
    BOOST_CHECK_EQUAL(addrman.GetFileFormat(), ADDRMAN_FILE_FORMAT_ADDRV2);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << addrman;
    const std::vector<unsigned char> vBytes(std::begin(ss), std::end(ss));
    // The format byte of the file.
    BOOST_CHECK_EQUAL(vBytes[0], ADDRMAN_FILE_FORMAT_ADDRV2);

    // The addresses survive a round trip. The re-serialization is byte
    // identical, so the entries, their sources and their bucket positions are
    // preserved.
    CDataStream ssIn(vBytes, SER_DISK, CLIENT_VERSION);
    CAddrMan addrman2;
    ssIn >> addrman2;
    BOOST_CHECK_EQUAL(addrman2.size(), addrman.size());
    BOOST_CHECK_EQUAL(addrman2.GetFileFormat(), ADDRMAN_FILE_FORMAT_ADDRV2);

    CDataStream ssOut(SER_DISK, CLIENT_VERSION);
    ssOut << addrman2;
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(std::begin(ss), std::end(ss))),
                      HexStr(std::vector<unsigned char>(std::begin(ssOut), std::end(ssOut))));
}

BOOST_AUTO_TEST_CASE(addrman_legacy_entry_format)
{
    // The entries of the legacy format must stay byte-identical: the disk
    // version word (the client version), the timestamp, the service flags,
    // the 16 byte address, the port, the 16 byte source, the timestamp of the
    // last success and the number of attempts.
    CService addr = ResolveService("1.2.3.4", 1234);
    CNetAddr source = ResolveIP("5.6.7.8");
    CAddrInfo info{CAddress(addr, NODE_NETWORK), source};

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << info;

    std::vector<unsigned char> expected;
    const auto append_int = [&expected](int64_t n, size_t nBytes) {
        for (size_t i = 0; i < nBytes; ++i) {
            expected.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xff));
        }
    };
    append_int(CLIENT_VERSION, 4);
    append_int(100000000, 4); // the timestamp of a new address
    append_int(static_cast<int64_t>(NODE_NETWORK), 8);
    // the IPv4-mapped IPv6 form of 1.2.3.4
    const std::vector<unsigned char> vAddr{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4};
    expected.insert(expected.end(), vAddr.begin(), vAddr.end());
    // the port, in big endian
    expected.push_back(0x04);
    expected.push_back(0xd2);
    // the source, in the IPv4-mapped IPv6 form
    const std::vector<unsigned char> vSource{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 5, 6, 7, 8};
    expected.insert(expected.end(), vSource.begin(), vSource.end());
    append_int(0, 8); // the timestamp of the last success
    append_int(0, 4); // the number of attempts

    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(std::begin(ss), std::end(ss))), HexStr(expected));
}

BOOST_AUTO_TEST_CASE(addrman_file_format_switch)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    // An addrman without privacy addresses is written in the legacy format,
    // so that the file stays readable by the nodes that do not support the
    // BIP155 format.
    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.GetFileFormat(), ADDRMAN_FILE_FORMAT_LEGACY);

    CDataStream ssLegacy(SER_DISK, CLIENT_VERSION);
    ssLegacy << addrman;
    const std::vector<unsigned char> vLegacy(std::begin(ssLegacy), std::end(ssLegacy));
    BOOST_CHECK_EQUAL(vLegacy[0], ADDRMAN_FILE_FORMAT_LEGACY);

    // Load the legacy file and learn a privacy address: the file is written in
    // the BIP155-capable format from now on and the entries round trip.
    CDataStream ssIn(vLegacy, SER_DISK, CLIENT_VERSION);
    CAddrMan addrman2;
    ssIn >> addrman2;
    BOOST_CHECK_EQUAL(addrman2.size(), 1U);
    BOOST_CHECK_EQUAL(addrman2.GetFileFormat(), ADDRMAN_FILE_FORMAT_LEGACY);

    const std::string onion = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion";
    CService onion_service = ResolveService(onion, 8555);
    CAddress onion_addr(onion_service, NODE_NONE);
    onion_addr.nTime = GetAdjustedTime();
    BOOST_CHECK(addrman2.Add(onion_addr, source));
    BOOST_CHECK_EQUAL(addrman2.GetFileFormat(), ADDRMAN_FILE_FORMAT_ADDRV2);

    CDataStream ssV2(SER_DISK, CLIENT_VERSION);
    ssV2 << addrman2;
    const std::vector<unsigned char> vV2(std::begin(ssV2), std::end(ssV2));
    BOOST_CHECK_EQUAL(vV2[0], ADDRMAN_FILE_FORMAT_ADDRV2);

    CDataStream ssV2In(vV2, SER_DISK, CLIENT_VERSION);
    CAddrMan addrman3;
    ssV2In >> addrman3;
    BOOST_CHECK_EQUAL(addrman3.size(), 2U);
    const std::vector<CAddress> vAddr = addrman3.GetAddr(std::nullopt, 1000, 100);
    bool found{false};
    for (const CAddress& addr : vAddr) {
        found |= addr.ToStringIP() == onion;
    }
    BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(addrman_invalid_files)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");
    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << addrman;
    const std::vector<unsigned char> vBytes(std::begin(ss), std::end(ss));

    // A file with an unknown format byte is rejected.
    {
        std::vector<unsigned char> vInvalid = vBytes;
        vInvalid[0] = 4;
        CDataStream ssIn(vInvalid, SER_DISK, CLIENT_VERSION);
        CAddrMan addrman2;
        BOOST_CHECK_THROW(ssIn >> addrman2, InvalidAddrManVersionError);
    }

    // A truncated file is rejected.
    {
        const std::vector<unsigned char> vInvalid(vBytes.begin(), vBytes.begin() + vBytes.size() / 2);
        CDataStream ssIn(vInvalid, SER_DISK, CLIENT_VERSION);
        CAddrMan addrman2;
        BOOST_CHECK_THROW(ssIn >> addrman2, std::ios_base::failure);
    }

    // An entry with an unknown network id is rejected.
    {
        CService onion = ResolveService("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion", 8555);
        CAddrInfo info{CAddress(onion, NODE_NONE), source};
        CDataStream ssEntry(SER_DISK, CLIENT_VERSION);
        info.SerializeAddrV2(ssEntry);
        std::vector<unsigned char> vEntry(std::begin(ssEntry), std::end(ssEntry));
        // The network id follows the disk version word, the timestamp and the
        // service flags.
        vEntry[9] = 42;
        CDataStream ssIn(vEntry, SER_DISK, CLIENT_VERSION);
        CAddrInfo info2;
        BOOST_CHECK_THROW(info2.UnserializeAddrV2(ssIn), std::ios_base::failure);
    }

    // An entry with an oversized address is rejected before the address is
    // read, so that a corrupt file cannot cause an unbounded allocation.
    {
        CDataStream ssEntry(SER_DISK, CLIENT_VERSION);
        const uint32_t nMarker = static_cast<uint32_t>(CLIENT_VERSION) | DISK_VERSION_ADDRV2;
        ssEntry << nMarker;
        const uint32_t nTime = 100000000;
        ssEntry << nTime;
        ssEntry << uint64_t{0};
        ssEntry << uint8_t{BIP155_TORV3};
        ::WriteCompactSize(ssEntry, MAX_ADDRV2_SIZE + 1);
        const std::vector<unsigned char> vEntry(std::begin(ssEntry), std::end(ssEntry));
        CDataStream ssIn(vEntry, SER_DISK, CLIENT_VERSION);
        CAddrInfo info2;
        BOOST_CHECK_THROW(info2.UnserializeAddrV2(ssIn), std::ios_base::failure);
    }
}

BOOST_AUTO_TEST_CASE(addrman_asmap_bucketing)
{
    const CNetAddr source = ResolveIP("252.2.2.2");
    const CService addr1 = ResolveService("1.2.3.4", 8555);
    const CService addr2 = ResolveService("2.3.4.5", 8555);

    // An addrman that uses an ASMap writes the format with the bucketing
    // fingerprint.
    CAddrManTest addrman(true, AsmapManager1());
    BOOST_CHECK_EQUAL(addrman.GetFileFormat(), ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK(addrman.Add(CAddress(addr2, NODE_NONE), source));

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << addrman;
    const std::vector<unsigned char> vBytes(std::begin(ss), std::end(ss));
    BOOST_CHECK_EQUAL(vBytes[0], ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);

    // Without an ASMap the classic format is written.
    CAddrManTest classic(true);
    BOOST_CHECK_EQUAL(classic.GetFileFormat(), ADDRMAN_FILE_FORMAT_LEGACY);
    BOOST_CHECK(classic.Add(CAddress(addr1, NODE_NONE), source));
    CDataStream ssClassic(SER_DISK, CLIENT_VERSION);
    ssClassic << classic;
    const std::vector<unsigned char> vClassic(std::begin(ssClassic), std::end(ssClassic));
    BOOST_CHECK_EQUAL(vClassic[0], ADDRMAN_FILE_FORMAT_LEGACY);

    // Loading a file that was written with the same ASMap restores the
    // bucketing: the file is written back identically.
    {
        CAddrManTest restored(true, AsmapManager1());
        CDataStream ssIn(vBytes, SER_DISK, CLIENT_VERSION);
        ssIn >> restored;
        BOOST_CHECK_EQUAL(restored.size(), 2U);
        CDataStream ssOut(SER_DISK, CLIENT_VERSION);
        ssOut << restored;
        const std::vector<unsigned char> vOut(std::begin(ssOut), std::end(ssOut));
        BOOST_CHECK(vOut == vBytes);
    }

    // Loading the file with a different ASMap re-buckets the entries: the
    // entries are preserved, but the buckets (and the fingerprint) change.
    {
        CAddrManTest restored(true, AsmapManager2());
        CDataStream ssIn(vBytes, SER_DISK, CLIENT_VERSION);
        ssIn >> restored;
        BOOST_CHECK_EQUAL(restored.size(), 2U);
        CDataStream ssOut(SER_DISK, CLIENT_VERSION);
        ssOut << restored;
        const std::vector<unsigned char> vOut(std::begin(ssOut), std::end(ssOut));
        BOOST_CHECK(vOut != vBytes);
        BOOST_CHECK_EQUAL(vOut[0], ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);
    }

    // Loading the file without an ASMap re-buckets the entries back to the
    // classic grouping; the file is written in one of the classic formats
    // again.
    {
        CAddrManTest restored(true);
        CDataStream ssIn(vBytes, SER_DISK, CLIENT_VERSION);
        ssIn >> restored;
        BOOST_CHECK_EQUAL(restored.size(), 2U);
        CDataStream ssOut(SER_DISK, CLIENT_VERSION);
        ssOut << restored;
        const std::vector<unsigned char> vOut(std::begin(ssOut), std::end(ssOut));
        BOOST_CHECK(vOut != vBytes);
        BOOST_CHECK(vOut[0] == ADDRMAN_FILE_FORMAT_LEGACY || vOut[0] == ADDRMAN_FILE_FORMAT_ADDRV2);
    }

    // Loading a file that was written without an ASMap with an ASMap
    // configured re-buckets the entries as well.
    {
        CAddrManTest restored(true, AsmapManager1());
        CDataStream ssIn(vClassic, SER_DISK, CLIENT_VERSION);
        ssIn >> restored;
        BOOST_CHECK_EQUAL(restored.size(), 1U);
        CDataStream ssOut(SER_DISK, CLIENT_VERSION);
        ssOut << restored;
        const std::vector<unsigned char> vOut(std::begin(ssOut), std::end(ssOut));
        BOOST_CHECK_EQUAL(vOut[0], ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);
        BOOST_CHECK(vOut != vClassic);
    }
}

BOOST_AUTO_TEST_CASE(addrman_asmap_tried_table)
{
    const CNetAddr source = ResolveIP("252.2.2.2");
    const CService addr1 = ResolveService("1.2.3.4", 8555);

    CAddrManTest addrman(true, AsmapManager1());
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    addrman.Good(CAddress(addr1, NODE_NONE));
    // The entry is in the tried table.
    BOOST_CHECK_EQUAL(addrman.Select(false).ToString(), "1.2.3.4:8555");

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << addrman;

    // The tried table is recomputed on load (with the current grouping), so
    // the entry is still there.
    CAddrManTest restored(true, AsmapManager1());
    CDataStream ssIn(std::vector<unsigned char>(std::begin(ss), std::end(ss)), SER_DISK, CLIENT_VERSION);
    ssIn >> restored;
    BOOST_CHECK_EQUAL(restored.size(), 1U);
    BOOST_CHECK_EQUAL(restored.Select(false).ToString(), "1.2.3.4:8555");
}

BOOST_AUTO_TEST_CASE(addrman_asmap_rebucket_collision)
{
    // The bucket of a new entry is a function of its netgroup and the netgroup
    // of its source, so all the addresses of one AS that are announced by one
    // source end up in the same bucket. If there are more of them than the
    // bucket has positions, their positions collide.
    //
    // The fixture maps 1.0.0.0/8 to AS64500, so all the addresses below share
    // one netgroup.
    const CNetAddr source = ResolveIP("252.2.2.2");
    CAddrManTest addrman(true, AsmapManager1());
    for (int i = 1; i <= 255; ++i) {
        const std::string addr = "1.0.0." + std::to_string(i);
        BOOST_CHECK(addrman.Add(CAddress(ResolveService(addr, 8555), NODE_NONE), source));
    }
    // Only the addresses that got a bucket position are kept.
    BOOST_CHECK(addrman.size() > 0);
    BOOST_CHECK(addrman.size() <= 255U);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << addrman;
    const std::vector<unsigned char> vBytes(std::begin(ss), std::end(ss));
    BOOST_CHECK_EQUAL(vBytes[0], ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);

    // Read the file with a different ASMap: the bucketing method changed, so
    // the new table is re-bucketed. The re-bucketing assigns one bucket per
    // entry, so the entries of one netgroup collide on the bucket positions.
    // The entries that get no bucket must be pruned, so that the number of the
    // written entries matches the number of the entries of the address
    // manager (otherwise the file cannot be read back).
    CAddrManTest rebucketed(true, AsmapManager2());
    CDataStream ssIn(vBytes, SER_DISK, CLIENT_VERSION);
    BOOST_CHECK_NO_THROW(ssIn >> rebucketed);
    BOOST_CHECK(rebucketed.size() > 0);
    // The entries that got no bucket during the re-bucketing were pruned.
    BOOST_CHECK(rebucketed.size() <= addrman.size());

    // The re-bucketed address manager must be written consistently and the
    // written file must be readable again.
    CDataStream ssOut(SER_DISK, CLIENT_VERSION);
    ssOut << rebucketed;
    const std::vector<unsigned char> vOut(std::begin(ssOut), std::end(ssOut));
    BOOST_CHECK_EQUAL(vOut[0], ADDRMAN_FILE_FORMAT_ADDRV2_ASMAP);

    CAddrManTest restored(true, AsmapManager2());
    CDataStream ssIn2(vOut, SER_DISK, CLIENT_VERSION);
    BOOST_CHECK_NO_THROW(ssIn2 >> restored);
    BOOST_CHECK_EQUAL(restored.size(), rebucketed.size());
    BOOST_CHECK(restored.size() > 0);

    // The same for the removal of the ASMap: the entries are re-bucketed with
    // the classic grouping, the file is written in a classic format and it
    // must be readable again.
    CAddrManTest classic(true);
    CDataStream ssIn3(vBytes, SER_DISK, CLIENT_VERSION);
    BOOST_CHECK_NO_THROW(ssIn3 >> classic);
    CDataStream ssOut3(SER_DISK, CLIENT_VERSION);
    ssOut3 << classic;
    const std::vector<unsigned char> vOut3(std::begin(ssOut3), std::end(ssOut3));
    BOOST_CHECK(vOut3[0] == ADDRMAN_FILE_FORMAT_LEGACY || vOut3[0] == ADDRMAN_FILE_FORMAT_ADDRV2);

    CAddrManTest restored_classic(true);
    CDataStream ssIn4(vOut3, SER_DISK, CLIENT_VERSION);
    BOOST_CHECK_NO_THROW(ssIn4 >> restored_classic);
    BOOST_CHECK_EQUAL(restored_classic.size(), classic.size());
}

BOOST_AUTO_TEST_CASE(addrman_ports)
{
    CAddrManTest addrman;
    CNetAddr source = ResolveIP("252.2.2.2");

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    // Test 7; Addr with same IP but diff port does not replace existing addr.
    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);

    CService addr1_port = ResolveService("250.1.1.1", 8334);
    BOOST_CHECK(!addrman.Add(CAddress(addr1_port, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);
    CAddrInfo addr_ret2 = addrman.Select();
    BOOST_CHECK_EQUAL(addr_ret2.ToString(), "250.1.1.1:8555");

    // Test: Add same IP but diff port to tried table, it doesn't get added.
    //  Perhaps this is not ideal behavior but it is the current behavior.
    addrman.Good(CAddress(addr1_port, NODE_NONE));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);
    bool newOnly = true;
    CAddrInfo addr_ret3 = addrman.Select(newOnly);
    BOOST_CHECK_EQUAL(addr_ret3.ToString(), "250.1.1.1:8555");
}


BOOST_AUTO_TEST_CASE(addrman_select)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    // Test: Select from new with 1 addr in new.
    CService addr1 = ResolveService("250.1.1.1", 8555);
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);

    bool newOnly = true;
    CAddrInfo addr_ret1 = addrman.Select(newOnly);
    BOOST_CHECK_EQUAL(addr_ret1.ToString(), "250.1.1.1:8555");

    // Test: move addr to tried, select from new expected nothing returned.
    addrman.Good(CAddress(addr1, NODE_NONE));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);
    CAddrInfo addr_ret2 = addrman.Select(newOnly);
    BOOST_CHECK_EQUAL(addr_ret2.ToString(), "[::]:0");

    CAddrInfo addr_ret3 = addrman.Select();
    BOOST_CHECK_EQUAL(addr_ret3.ToString(), "250.1.1.1:8555");

    BOOST_CHECK_EQUAL(addrman.size(), 1U);


    // Add three addresses to new table.
    CService addr2 = ResolveService("250.3.1.1", 8555);
    CService addr3 = ResolveService("250.3.2.2", 9999);
    CService addr4 = ResolveService("250.3.3.3", 9999);

    BOOST_CHECK(addrman.Add(CAddress(addr2, NODE_NONE), ResolveService("250.3.1.1", 8555)));
    BOOST_CHECK(addrman.Add(CAddress(addr3, NODE_NONE), ResolveService("250.3.1.1", 8555)));
    BOOST_CHECK(addrman.Add(CAddress(addr4, NODE_NONE), ResolveService("250.4.1.1", 8555)));

    // Add three addresses to tried table.
    CService addr5 = ResolveService("250.4.4.4", 8555);
    CService addr6 = ResolveService("250.4.5.5", 7777);
    CService addr7 = ResolveService("250.4.6.6", 8555);

    BOOST_CHECK(addrman.Add(CAddress(addr5, NODE_NONE), ResolveService("250.3.1.1", 8555)));
    addrman.Good(CAddress(addr5, NODE_NONE));
    BOOST_CHECK(addrman.Add(CAddress(addr6, NODE_NONE), ResolveService("250.3.1.1", 8555)));
    addrman.Good(CAddress(addr6, NODE_NONE));
    BOOST_CHECK(addrman.Add(CAddress(addr7, NODE_NONE), ResolveService("250.1.1.3", 8555)));
    addrman.Good(CAddress(addr7, NODE_NONE));

    // Test: 6 addrs + 1 addr from last test = 7.
    BOOST_CHECK_EQUAL(addrman.size(), 7U);

    // Test: Select pulls from new and tried regardless of port number.
    std::set<uint16_t> ports;
    for (int i = 0; i < 20; ++i) {
        ports.insert(addrman.Select().GetPort());
    }
    BOOST_CHECK_EQUAL(ports.size(), 3U);
}

BOOST_AUTO_TEST_CASE(addrman_new_collisions)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    for (unsigned int i = 1; i < 18; i++) {
        CService addr = ResolveService("250.1.1." + std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));

        //Test: No collision in new table yet.
        BOOST_CHECK_EQUAL(addrman.size(), i);
    }

    //Test: new table collision!
    CService addr1 = ResolveService("250.1.1.18");
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 17U);

    CService addr2 = ResolveService("250.1.1.19");
    BOOST_CHECK(addrman.Add(CAddress(addr2, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 18U);
}

BOOST_AUTO_TEST_CASE(addrman_tried_collisions)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    for (unsigned int i = 1; i < 80; i++) {
        CService addr = ResolveService("250.1.1." + std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
        addrman.Good(CAddress(addr, NODE_NONE));

        //Test: No collision in tried table yet.
        BOOST_CHECK_EQUAL(addrman.size(), i);
    }

    //Test: tried table collision!
    CService addr1 = ResolveService("250.1.1.80");
    BOOST_CHECK(addrman.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 79U);

    CService addr2 = ResolveService("250.1.1.81");
    BOOST_CHECK(addrman.Add(CAddress(addr2, NODE_NONE), source));
    BOOST_CHECK_EQUAL(addrman.size(), 80U);
}

BOOST_AUTO_TEST_CASE(addrman_find)
{
    CAddrManTest addrman;

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    CAddress addr1 = CAddress(ResolveService("250.1.2.1", 8555), NODE_NONE);
    CAddress addr2 = CAddress(ResolveService("250.1.2.1", 9999), NODE_NONE);
    CAddress addr3 = CAddress(ResolveService("251.255.2.1", 8555), NODE_NONE);

    CNetAddr source1 = ResolveIP("250.1.2.1");
    CNetAddr source2 = ResolveIP("250.1.2.2");

    BOOST_CHECK(addrman.Add(addr1, source1));
    BOOST_CHECK(!addrman.Add(addr2, source2));
    BOOST_CHECK(addrman.Add(addr3, source1));

    // Test: ensure Find returns an IP matching what we searched on.
    CAddrInfo* info1 = addrman.Find(addr1);
    BOOST_REQUIRE(info1);
    BOOST_CHECK_EQUAL(info1->ToString(), "250.1.2.1:8555");

    // Test 18; Find does not discriminate by port number.
    CAddrInfo* info2 = addrman.Find(addr2);
    BOOST_REQUIRE(info2);
    BOOST_CHECK_EQUAL(info2->ToString(), info1->ToString());

    // Test: Find returns another IP matching what we searched on.
    CAddrInfo* info3 = addrman.Find(addr3);
    BOOST_REQUIRE(info3);
    BOOST_CHECK_EQUAL(info3->ToString(), "251.255.2.1:8555");
}

BOOST_AUTO_TEST_CASE(addrman_create)
{
    CAddrManTest addrman;

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    CAddress addr1 = CAddress(ResolveService("250.1.2.1", 8555), NODE_NONE);
    CNetAddr source1 = ResolveIP("250.1.2.1");

    int nId;
    CAddrInfo* pinfo = addrman.Create(addr1, source1, &nId);

    // Test: The result should be the same as the input addr.
    BOOST_CHECK_EQUAL(pinfo->ToString(), "250.1.2.1:8555");

    CAddrInfo* info2 = addrman.Find(addr1);
    BOOST_CHECK_EQUAL(info2->ToString(), "250.1.2.1:8555");
}


BOOST_AUTO_TEST_CASE(addrman_delete)
{
    CAddrManTest addrman;

    BOOST_CHECK_EQUAL(addrman.size(), 0U);

    CAddress addr1 = CAddress(ResolveService("250.1.2.1", 8555), NODE_NONE);
    CNetAddr source1 = ResolveIP("250.1.2.1");

    int nId;
    addrman.Create(addr1, source1, &nId);

    // Test: Delete should actually delete the addr.
    BOOST_CHECK_EQUAL(addrman.size(), 1U);
    addrman.Delete(nId);
    BOOST_CHECK_EQUAL(addrman.size(), 0U);
    CAddrInfo* info2 = addrman.Find(addr1);
    BOOST_CHECK(info2 == nullptr);
}

BOOST_AUTO_TEST_CASE(addrman_getaddr)
{
    CAddrManTest addrman;

    // Test: Sanity check, GetAddr should never return anything if addrman
    //  is empty.
    BOOST_CHECK_EQUAL(addrman.size(), 0U);
    std::vector<CAddress> vAddr1 = addrman.GetAddr();
    BOOST_CHECK_EQUAL(vAddr1.size(), 0U);

    CAddress addr1 = CAddress(ResolveService("250.250.2.1", 8555), NODE_NONE);
    addr1.nTime = GetAdjustedTime(); // Set time so isTerrible = false
    CAddress addr2 = CAddress(ResolveService("250.251.2.2", 9999), NODE_NONE);
    addr2.nTime = GetAdjustedTime();
    CAddress addr3 = CAddress(ResolveService("251.252.2.3", 8555), NODE_NONE);
    addr3.nTime = GetAdjustedTime();
    CAddress addr4 = CAddress(ResolveService("252.253.3.4", 8555), NODE_NONE);
    addr4.nTime = GetAdjustedTime();
    CAddress addr5 = CAddress(ResolveService("252.254.4.5", 8555), NODE_NONE);
    addr5.nTime = GetAdjustedTime();
    CNetAddr source1 = ResolveIP("250.1.2.1");
    CNetAddr source2 = ResolveIP("250.2.3.3");

    // Test: Ensure GetAddr works with new addresses.
    BOOST_CHECK(addrman.Add(addr1, source1));
    BOOST_CHECK(addrman.Add(addr2, source2));
    BOOST_CHECK(addrman.Add(addr3, source1));
    BOOST_CHECK(addrman.Add(addr4, source2));
    BOOST_CHECK(addrman.Add(addr5, source1));

    // GetAddr returns 23% of addresses, 23% of 5 is 1 rounded down.
    BOOST_CHECK_EQUAL(addrman.GetAddr().size(), 1U);

    // Test: Ensure GetAddr works with new and tried addresses.
    addrman.Good(CAddress(addr1, NODE_NONE));
    addrman.Good(CAddress(addr2, NODE_NONE));
    BOOST_CHECK_EQUAL(addrman.GetAddr().size(), 1U);

    // Test: Ensure GetAddr still returns 23% when addrman has many addrs.
    for (unsigned int i = 1; i < (8 * 256); i++) {
        int octet1 = i % 256;
        int octet2 = i >> 8 % 256;
        std::string strAddr = std::to_string(octet1) + "." + std::to_string(octet2) + ".1.23";
        CAddress addr = CAddress(ResolveService(strAddr), NODE_NONE);

        // Ensure that for all addrs in addrman, isTerrible == false.
        addr.nTime = GetAdjustedTime();
        addrman.Add(addr, ResolveIP(strAddr));
        if (i % 8 == 0)
            addrman.Good(addr);
    }
    std::vector<CAddress> vAddr = addrman.GetAddr();

    size_t percent23 = (addrman.size() * 23) / 100;
    BOOST_CHECK_EQUAL(vAddr.size(), percent23);
    BOOST_CHECK_EQUAL(vAddr.size(), 461U);
    // (Addrman.size() < number of addresses added) due to address collisions.
    BOOST_CHECK_EQUAL(addrman.size(), 2006U);
}


BOOST_AUTO_TEST_CASE(caddrinfo_get_tried_bucket)
{
    CAddrManTest addrman;

    CAddress addr1 = CAddress(ResolveService("250.1.1.1", 8555), NODE_NONE);
    CAddress addr2 = CAddress(ResolveService("250.1.1.1", 9999), NODE_NONE);

    CNetAddr source1 = ResolveIP("250.1.1.1");


    CAddrInfo info1 = CAddrInfo(addr1, source1);

    uint256 nKey1 = (uint256)(CHashWriter(SER_GETHASH, 0) << 1).GetHash();
    uint256 nKey2 = (uint256)(CHashWriter(SER_GETHASH, 0) << 2).GetHash();

    // TODO (pvl): Verify hash again. Changed from 17 to 77 after port change
    BOOST_CHECK_EQUAL(info1.GetTriedBucket(nKey1), 77);

    // Test: Make sure key actually randomizes bucket placement. A fail on
    //  this test could be a security issue.
    BOOST_CHECK(info1.GetTriedBucket(nKey1) != info1.GetTriedBucket(nKey2));

    // Test: Two addresses with same IP but different ports can map to
    //  different buckets because they have different keys.
    CAddrInfo info2 = CAddrInfo(addr2, source1);

    BOOST_CHECK(info1.GetKey() != info2.GetKey());
    BOOST_CHECK(info1.GetTriedBucket(nKey1) != info2.GetTriedBucket(nKey1));

    std::set<int> buckets;
    for (int i = 0; i < 255; i++) {
        CAddrInfo infoi = CAddrInfo(
            CAddress(ResolveService("250.1.1." + std::to_string(i)), NODE_NONE),
            ResolveIP("250.1.1." + std::to_string(i)));
        int bucket = infoi.GetTriedBucket(nKey1);
        buckets.insert(bucket);
    }
    // Test: IP addresses in the same group (\16 prefix for IPv4) should
    //  never get more than 8 buckets
    BOOST_CHECK_EQUAL(buckets.size(), 8U);

    buckets.clear();
    for (int j = 0; j < 255; j++) {
        CAddrInfo infoj = CAddrInfo(
            CAddress(ResolveService("250." + std::to_string(j) + ".1.1"), NODE_NONE),
            ResolveIP("250." + std::to_string(j) + ".1.1"));
        int bucket = infoj.GetTriedBucket(nKey1);
        buckets.insert(bucket);
    }
    // Test: IP addresses in the different groups should map to more than
    //  8 buckets.
    BOOST_CHECK_EQUAL(buckets.size(), 160U);
}

BOOST_AUTO_TEST_CASE(caddrinfo_get_new_bucket)
{
    CAddrManTest addrman;

    CAddress addr1 = CAddress(ResolveService("250.1.2.1", 8555), NODE_NONE);
    CAddress addr2 = CAddress(ResolveService("250.1.2.1", 9999), NODE_NONE);

    CNetAddr source1 = ResolveIP("250.1.2.1");

    CAddrInfo info1 = CAddrInfo(addr1, source1);

    uint256 nKey1 = (uint256)(CHashWriter(SER_GETHASH, 0) << 1).GetHash();
    uint256 nKey2 = (uint256)(CHashWriter(SER_GETHASH, 0) << 2).GetHash();

    // Test: Make sure the buckets are what we expect
    BOOST_CHECK_EQUAL(info1.GetNewBucket(nKey1), 786);
    BOOST_CHECK_EQUAL(info1.GetNewBucket(nKey1, source1), 786);

    // Test: Make sure key actually randomizes bucket placement. A fail on
    //  this test could be a security issue.
    BOOST_CHECK(info1.GetNewBucket(nKey1) != info1.GetNewBucket(nKey2));

    // Test: Ports should not affect bucket placement in the addr
    CAddrInfo info2 = CAddrInfo(addr2, source1);
    BOOST_CHECK(info1.GetKey() != info2.GetKey());
    BOOST_CHECK_EQUAL(info1.GetNewBucket(nKey1), info2.GetNewBucket(nKey1));

    std::set<int> buckets;
    for (int i = 0; i < 255; i++) {
        CAddrInfo infoi = CAddrInfo(
            CAddress(ResolveService("250.1.1." + std::to_string(i)), NODE_NONE),
            ResolveIP("250.1.1." + std::to_string(i)));
        int bucket = infoi.GetNewBucket(nKey1);
        buckets.insert(bucket);
    }
    // Test: IP addresses in the same group (\16 prefix for IPv4) should
    //  always map to the same bucket.
    BOOST_CHECK_EQUAL(buckets.size(), 1U);

    buckets.clear();
    for (int j = 0; j < 4 * 255; j++) {
        CAddrInfo infoj = CAddrInfo(CAddress(
                                        ResolveService(
                                            std::to_string(250 + (j / 255)) + "." + std::to_string(j % 256) + ".1.1"), NODE_NONE),
            ResolveIP("251.4.1.1"));
        int bucket = infoj.GetNewBucket(nKey1);
        buckets.insert(bucket);
    }
    // Test: IP addresses in the same source groups should map to no more
    //  than 64 buckets.
    BOOST_CHECK(buckets.size() <= 64);

    buckets.clear();
    for (int p = 0; p < 255; p++) {
        CAddrInfo infoj = CAddrInfo(
            CAddress(ResolveService("250.1.1.1"), NODE_NONE),
            ResolveIP("250." + std::to_string(p) + ".1.1"));
        int bucket = infoj.GetNewBucket(nKey1);
        buckets.insert(bucket);
    }
    // Test: IP addresses in the different source groups should map to more
    //  than 64 buckets.
    BOOST_CHECK(buckets.size() > 64);
}


BOOST_AUTO_TEST_CASE(addrman_selecttriedcollision)
{
    CAddrManTest addrman;

    BOOST_CHECK(addrman.size() == 0);

    // Empty addrman should return blank addrman info.
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");

    // Add twenty two addresses.
    CNetAddr source = ResolveIP("252.2.2.2");
    for (unsigned int i = 1; i < 23; i++) {
        CService addr = ResolveService("250.1.1."+std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
        addrman.Good(addr);

        // No collisions yet.
        BOOST_CHECK(addrman.size() == i);
        BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
    }

    // Ensure Good handles duplicates well.
    for (unsigned int i = 1; i < 23; i++) {
        CService addr = ResolveService("250.1.1."+std::to_string(i));
        addrman.Good(addr);

        BOOST_CHECK(addrman.size() == 22);
        BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
    }

}

BOOST_AUTO_TEST_CASE(addrman_noevict)
{
    CAddrManTest addrman;

    // Add twenty two addresses.
    CNetAddr source = ResolveIP("252.2.2.2");
    for (unsigned int i = 1; i < 23; i++) {
        CService addr = ResolveService("250.1.1."+std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
        addrman.Good(addr);

        // No collision yet.
        BOOST_CHECK(addrman.size() == i);
        BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
    }

    // Collision between 23 and 19.
    CService addr23 = ResolveService("250.1.1.23");
    BOOST_CHECK(addrman.Add(CAddress(addr23, NODE_NONE), source));
    addrman.Good(addr23);

    BOOST_CHECK(addrman.size() == 23);
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "250.1.1.19:0");

    // 23 should be discarded and 19 not evicted.
    addrman.ResolveCollisions();
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");

    // Lets create two collisions.
    for (unsigned int i = 24; i < 33; i++) {
        CService addr = ResolveService("250.1.1."+std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
        addrman.Good(addr);

        BOOST_CHECK(addrman.size() == i);
        BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
    }

    // Cause a collision.
    CService addr33 = ResolveService("250.1.1.33");
    BOOST_CHECK(addrman.Add(CAddress(addr33, NODE_NONE), source));
    addrman.Good(addr33);
    BOOST_CHECK(addrman.size() == 33);

    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "250.1.1.27:0");

    // Cause a second collision.
    BOOST_CHECK(!addrman.Add(CAddress(addr23, NODE_NONE), source));
    addrman.Good(addr23);
    BOOST_CHECK(addrman.size() == 33);

    BOOST_CHECK(addrman.SelectTriedCollision().ToString() != "[::]:0");
    addrman.ResolveCollisions();
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
}

BOOST_AUTO_TEST_CASE(addrman_evictionworks)
{
    CAddrManTest addrman;

    BOOST_CHECK(addrman.size() == 0);

    // Empty addrman should return blank addrman info.
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");

    // Add twenty two addresses.
    CNetAddr source = ResolveIP("252.2.2.2");
    for (unsigned int i = 1; i < 23; i++) {
        CService addr = ResolveService("250.1.1."+std::to_string(i));
        BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
        addrman.Good(addr);

        // No collision yet.
        BOOST_CHECK(addrman.size() == i);
        BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
    }

    // Collision between 23 and 19.
    CService addr = ResolveService("250.1.1.23");
    BOOST_CHECK(addrman.Add(CAddress(addr, NODE_NONE), source));
    addrman.Good(addr);

    BOOST_CHECK(addrman.size() == 23);
    CAddrInfo info = addrman.SelectTriedCollision();
    BOOST_CHECK(info.ToString() == "250.1.1.19:0");

    // Ensure test of address fails, so that it is evicted.
    addrman.SimConnFail(info);

    // Should swap 23 for 19.
    addrman.ResolveCollisions();
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");

    // If 23 was swapped for 19, then this should cause no collisions.
    BOOST_CHECK(!addrman.Add(CAddress(addr, NODE_NONE), source));
    addrman.Good(addr);

    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");

    // If we insert 19 is should collide with 23.
    CService addr19 = ResolveService("250.1.1.19");
    BOOST_CHECK(!addrman.Add(CAddress(addr19, NODE_NONE), source));
    addrman.Good(addr19);

    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "250.1.1.23:0");

    addrman.ResolveCollisions();
    BOOST_CHECK(addrman.SelectTriedCollision().ToString() == "[::]:0");
}


BOOST_AUTO_TEST_CASE(addrman_count_by_network)
{
    CAddrManTest addrman;

    CNetAddr source = ResolveIP("252.2.2.2");

    // An empty addrman reports zero for every routable network.
    std::map<enum Network, int64_t> counts = addrman.CountByNetwork();
    BOOST_CHECK_EQUAL(counts.size(), 3U);
    BOOST_CHECK_EQUAL(counts[NET_IPV4], 0);
    BOOST_CHECK_EQUAL(counts[NET_IPV6], 0);
    BOOST_CHECK_EQUAL(counts[NET_ONION], 0);

    // Two IPv4 addresses.
    BOOST_CHECK(addrman.Add(CAddress(ResolveService("250.1.1.1", 8555), NODE_NONE), source));
    BOOST_CHECK(addrman.Add(CAddress(ResolveService("250.1.1.2", 8555), NODE_NONE), source));

    // One IPv6 address.
    BOOST_CHECK(addrman.Add(CAddress(ResolveService("2606:4700:4700::1111", 8555), NODE_NONE), source));

    // One onion address.
    CService onion = ResolveService("6hzph5hv6337r6p2.onion", 8555);
    BOOST_CHECK(onion.IsValid());
    BOOST_CHECK(addrman.Add(CAddress(onion, NODE_NONE), source));

    // Non-routable addresses are rejected and therefore never counted.
    BOOST_CHECK(!addrman.Add(CAddress(ResolveService("127.0.0.1", 8555), NODE_NONE), source));

    counts = addrman.CountByNetwork();
    BOOST_CHECK_EQUAL(counts.size(), 3U);
    BOOST_CHECK_EQUAL(counts[NET_IPV4], 2);
    BOOST_CHECK_EQUAL(counts[NET_IPV6], 1);
    BOOST_CHECK_EQUAL(counts[NET_ONION], 1);

    // The breakdown always sums up to the total number of known addresses.
    int64_t total{0};
    for (const auto& entry : counts) {
        total += entry.second;
    }
    BOOST_CHECK_EQUAL(total, static_cast<int64_t>(addrman.size()));

    // Removing an address updates the counter again.
    addrman.Clear();
    counts = addrman.CountByNetwork();
    BOOST_CHECK_EQUAL(counts[NET_IPV4], 0);
    BOOST_CHECK_EQUAL(counts[NET_IPV6], 0);
    BOOST_CHECK_EQUAL(counts[NET_ONION], 0);
}

BOOST_AUTO_TEST_SUITE_END()
