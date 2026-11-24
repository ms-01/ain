// Copyright (c) 2012-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>
#include <addrman.h>
#include <clientversion.h>
#include <test/setup_common.h>
#include <string>
#include <boost/test/unit_test.hpp>
#include <serialize.h>
#include <streams.h>
#include <net.h>
#include <netbase.h>
#include <chainparams.h>
#include <util/system.h>

#include <memory>

class CAddrManSerializationMock : public CAddrMan
{
public:
    virtual void Serialize(CDataStream& s) const = 0;

    //! Ensure that bucket placement is always the same for testing purposes.
    void MakeDeterministic()
    {
        nKey.SetNull();
        insecure_rand = FastRandomContext(true);
    }
};

class CAddrManUncorrupted : public CAddrManSerializationMock
{
public:
    void Serialize(CDataStream& s) const override
    {
        CAddrMan::Serialize(s);
    }
};

class CAddrManCorrupted : public CAddrManSerializationMock
{
public:
    void Serialize(CDataStream& s) const override
    {
        // Produces corrupt output that claims addrman has 20 addrs when it only has one addr.
        unsigned char nVersion = 1;
        s << nVersion;
        s << ((unsigned char)32);
        s << nKey;
        s << 10; // nNew
        s << 10; // nTried

        int nUBuckets = ADDRMAN_NEW_BUCKET_COUNT ^ (1 << 30);
        s << nUBuckets;

        CService serv;
        BOOST_CHECK(Lookup("252.1.1.1", serv, 7777, false));
        CAddress addr = CAddress(serv, NODE_NONE);
        CNetAddr resolved;
        BOOST_CHECK(LookupHost("252.2.2.2", resolved, false));
        CAddrInfo info = CAddrInfo(addr, resolved);
        s << info;
    }
};

static CDataStream AddrmanToStream(CAddrManSerializationMock& _addrman)
{
    CDataStream ssPeersIn(SER_DISK, CLIENT_VERSION);
    ssPeersIn << Params().MessageStart();
    ssPeersIn << _addrman;
    std::string str = ssPeersIn.str();
    std::vector<unsigned char> vchData(str.begin(), str.end());
    return CDataStream(vchData, SER_DISK, CLIENT_VERSION);
}

BOOST_FIXTURE_TEST_SUITE(net_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(cnode_listen_port)
{
    // test default
    unsigned short port = GetListenPort();
    BOOST_CHECK(port == Params().GetDefaultPort());
    // test set port
    unsigned short altPort = 12345;
    BOOST_CHECK(gArgs.SoftSetArg("-port", std::to_string(altPort)));
    port = GetListenPort();
    BOOST_CHECK(port == altPort);
}

BOOST_AUTO_TEST_CASE(caddrdb_read)
{
    CAddrManUncorrupted addrmanUncorrupted;
    addrmanUncorrupted.MakeDeterministic();

    CService addr1, addr2, addr3;
    BOOST_CHECK(Lookup("250.7.1.1", addr1, 8555, false));
    // TODO (pvl): CAUTION: Identify source of failure when port is set to 9999
    // as the addrman1.size() below ends up only as 2 instead of 3 - started after
    // port change PR
    BOOST_CHECK(Lookup("250.7.2.2", addr2, 9998, false));
    BOOST_CHECK(Lookup("250.7.3.3", addr3, 9999, false));

    // Add three addresses to new table.
    CService source;
    BOOST_CHECK(Lookup("252.5.1.1", source, 8555, false));
    BOOST_CHECK(addrmanUncorrupted.Add(CAddress(addr1, NODE_NONE), source));
    BOOST_CHECK(addrmanUncorrupted.Add(CAddress(addr2, NODE_NONE), source));
    BOOST_CHECK(addrmanUncorrupted.Add(CAddress(addr3, NODE_NONE), source));

    // Test that the de-serialization does not throw an exception.
    CDataStream ssPeers1 = AddrmanToStream(addrmanUncorrupted);
    bool exceptionThrown = false;
    CAddrMan addrman1;

    BOOST_CHECK(addrman1.size() == 0);
    try {
        unsigned char pchMsgTmp[4];
        ssPeers1 >> pchMsgTmp;
        ssPeers1 >> addrman1;
    } catch (const std::exception&) {
        exceptionThrown = true;
    }

    BOOST_CHECK(addrman1.size() == 3);
    BOOST_CHECK(exceptionThrown == false);

    // Test that CAddrDB::Read creates an addrman with the correct number of addrs.
    CDataStream ssPeers2 = AddrmanToStream(addrmanUncorrupted);

    CAddrMan addrman2;
    CAddrDB adb;
    BOOST_CHECK(addrman2.size() == 0);
    BOOST_CHECK(adb.Read(addrman2, ssPeers2));
    BOOST_CHECK(addrman2.size() == 3);
}


BOOST_AUTO_TEST_CASE(caddrdb_read_corrupted)
{
    CAddrManCorrupted addrmanCorrupted;
    addrmanCorrupted.MakeDeterministic();

    // Test that the de-serialization of corrupted addrman throws an exception.
    CDataStream ssPeers1 = AddrmanToStream(addrmanCorrupted);
    bool exceptionThrown = false;
    CAddrMan addrman1;
    BOOST_CHECK(addrman1.size() == 0);
    try {
        unsigned char pchMsgTmp[4];
        ssPeers1 >> pchMsgTmp;
        ssPeers1 >> addrman1;
    } catch (const std::exception&) {
        exceptionThrown = true;
    }
    // Even through de-serialization failed addrman is not left in a clean state.
    BOOST_CHECK(addrman1.size() == 1);
    BOOST_CHECK(exceptionThrown);

    // Test that CAddrDB::Read leaves addrman in a clean state if de-serialization fails.
    CDataStream ssPeers2 = AddrmanToStream(addrmanCorrupted);

    CAddrMan addrman2;
    CAddrDB adb;
    BOOST_CHECK(addrman2.size() == 0);
    BOOST_CHECK(!adb.Read(addrman2, ssPeers2));
    BOOST_CHECK(addrman2.size() == 0);
}

BOOST_AUTO_TEST_CASE(caddrdb_read_privacy_addresses)
{
    CAddrManUncorrupted addrmanUncorrupted;
    addrmanUncorrupted.MakeDeterministic();

    CService source;
    BOOST_CHECK(Lookup("252.5.1.1", source, 8555, false));

    // A Tor v3 onion address has no legacy (pre-BIP155) representation. It is
    // stored in the BIP155-capable format of the address database.
    CService onion;
    BOOST_CHECK(Lookup("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion", onion, 8555, false));
    CAddress onion_addr(onion, NODE_NONE);
    onion_addr.nTime = GetAdjustedTime();
    BOOST_CHECK(addrmanUncorrupted.Add(onion_addr, source));
    BOOST_CHECK_EQUAL(addrmanUncorrupted.GetFileFormat(), ADDRMAN_FILE_FORMAT_ADDRV2);

    CDataStream ssPeers = AddrmanToStream(addrmanUncorrupted);
    CAddrMan addrman;
    CAddrDB adb;
    BOOST_CHECK(adb.Read(addrman, ssPeers));
    BOOST_CHECK_EQUAL(addrman.size(), 1U);

    const std::vector<CAddress> vAddr = addrman.GetAddr(std::nullopt, 1000, 100);
    BOOST_CHECK_EQUAL(vAddr.size(), 1U);
    BOOST_CHECK(vAddr[0].IsTorV3());
    BOOST_CHECK_EQUAL(vAddr[0].ToStringIP(), "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion");
}

BOOST_AUTO_TEST_CASE(cnode_simple_test)
{
    SOCKET hSocket = INVALID_SOCKET;
    NodeId id = 0;
    int height = 0;

    in_addr ipv4Addr;
    ipv4Addr.s_addr = 0xa0b0c001;

    CAddress addr = CAddress(CService(ipv4Addr, 7777), NODE_NETWORK);
    std::string pszDest;
    bool fInboundIn = false;

    // Test that fFeeler is false by default.
    std::unique_ptr<CNode> pnode1 = std::make_unique<CNode>(id++, NODE_NETWORK, height, hSocket, addr, 0, 0, CAddress(), 0, pszDest, fInboundIn ? ConnectionType::INBOUND : ConnectionType::OUTBOUND_FULL_RELAY);
    BOOST_CHECK(pnode1->fInbound == false);
    BOOST_CHECK(pnode1->fFeeler == false);

    fInboundIn = true;
    std::unique_ptr<CNode> pnode2 = std::make_unique<CNode>(id++, NODE_NETWORK, height, hSocket, addr, 1, 1, CAddress(), 0, pszDest, fInboundIn ? ConnectionType::INBOUND : ConnectionType::OUTBOUND_FULL_RELAY);
    BOOST_CHECK(pnode2->fInbound == true);
    BOOST_CHECK(pnode2->fFeeler == false);
}

BOOST_AUTO_TEST_CASE(cnode_addr_compatibility)
{
    SOCKET hSocket = INVALID_SOCKET;
    int height = 0;

    in_addr ipv4Addr;
    ipv4Addr.s_addr = 0xa0b0c001;

    CAddress addr = CAddress(CService(ipv4Addr, 7777), NODE_NETWORK);
    std::string pszDest;
    bool fInboundIn = false;

    std::unique_ptr<CNode> pnode = std::make_unique<CNode>(0, NODE_NETWORK, height, hSocket, addr, 0, 0, CAddress(), 0, pszDest, fInboundIn ? ConnectionType::INBOUND : ConnectionType::OUTBOUND_FULL_RELAY);

    CService v1_service;
    BOOST_CHECK(Lookup("250.1.1.1", v1_service, 8555, false));
    CAddress v1_addr(v1_service, NODE_NETWORK);

    CService onion_service;
    BOOST_CHECK(Lookup("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion", onion_service, 8555, false));
    CAddress onion_addr(onion_service, NODE_NETWORK);
    BOOST_CHECK(onion_addr.IsTorV3());
    BOOST_CHECK(!onion_addr.IsAddrV1Compatible());

    // A legacy peer can receive the addresses that have a legacy
    // (pre-BIP155) representation.
    BOOST_CHECK(pnode->IsAddrCompatible(v1_addr));
    BOOST_CHECK(!pnode->IsAddrCompatible(onion_addr));

    // A peer that announced the support for the BIP155 address relay can
    // receive all the addresses.
    pnode->m_wants_addrv2 = true;
    BOOST_CHECK(pnode->IsAddrCompatible(v1_addr));
    BOOST_CHECK(pnode->IsAddrCompatible(onion_addr));
}

// prior to PR #14728, this test triggers an undefined behavior
BOOST_AUTO_TEST_CASE(ipv4_peer_with_ipv6_addrMe_test)
{
    // set up local addresses; all that's necessary to reproduce the bug is
    // that a normal IPv4 address is among the entries, but if this address is
    // !IsRoutable the undefined behavior is easier to trigger deterministically
    {
        LOCK(cs_mapLocalHost);
        in_addr ipv4AddrLocal;
        ipv4AddrLocal.s_addr = 0x0100007f;
        CNetAddr addr = CNetAddr(ipv4AddrLocal);
        LocalServiceInfo lsi;
        lsi.nScore = 23;
        lsi.nPort = 42;
        mapLocalHost[addr] = lsi;
    }

    // create a peer with an IPv4 address
    in_addr ipv4AddrPeer;
    ipv4AddrPeer.s_addr = 0xa0b0c001;
    CAddress addr = CAddress(CService(ipv4AddrPeer, 7777), NODE_NETWORK);
    std::unique_ptr<CNode> pnode = std::make_unique<CNode>(0, NODE_NETWORK, 0, INVALID_SOCKET, addr, 0, 0, CAddress{}, 0, std::string{}, ConnectionType::OUTBOUND_FULL_RELAY);
    pnode->fSuccessfullyConnected.store(true);

    // the peer claims to be reaching us via IPv6
    in6_addr ipv6AddrLocal;
    memset(ipv6AddrLocal.s6_addr, 0, 16);
    ipv6AddrLocal.s6_addr[0] = 0xcc;
    CAddress addrLocal = CAddress(CService(ipv6AddrLocal, 7777), NODE_NETWORK);
    pnode->SetAddrLocal(addrLocal);

    // before patch, this causes undefined behavior detectable with clang's -fsanitize=memory
    AdvertiseLocal(&*pnode);

    // suppress no-checks-run warning; if this test fails, it's by triggering a sanitizer
    BOOST_CHECK(1);
}


BOOST_AUTO_TEST_CASE(LimitedAndReachable_Network)
{
    BOOST_CHECK_EQUAL(IsReachable(NET_IPV4), true);
    BOOST_CHECK_EQUAL(IsReachable(NET_IPV6), true);
    BOOST_CHECK_EQUAL(IsReachable(NET_ONION), true);

    SetReachable(NET_IPV4, false);
    SetReachable(NET_IPV6, false);
    SetReachable(NET_ONION, false);

    BOOST_CHECK_EQUAL(IsReachable(NET_IPV4), false);
    BOOST_CHECK_EQUAL(IsReachable(NET_IPV6), false);
    BOOST_CHECK_EQUAL(IsReachable(NET_ONION), false);

    SetReachable(NET_IPV4, true);
    SetReachable(NET_IPV6, true);
    SetReachable(NET_ONION, true);

    BOOST_CHECK_EQUAL(IsReachable(NET_IPV4), true);
    BOOST_CHECK_EQUAL(IsReachable(NET_IPV6), true);
    BOOST_CHECK_EQUAL(IsReachable(NET_ONION), true);
}

BOOST_AUTO_TEST_CASE(LimitedAndReachable_NetworkCaseUnroutableAndInternal)
{
    BOOST_CHECK_EQUAL(IsReachable(NET_UNROUTABLE), true);
    BOOST_CHECK_EQUAL(IsReachable(NET_INTERNAL), true);

    SetReachable(NET_UNROUTABLE, false);
    SetReachable(NET_INTERNAL, false);

    BOOST_CHECK_EQUAL(IsReachable(NET_UNROUTABLE), true); // Ignored for both networks
    BOOST_CHECK_EQUAL(IsReachable(NET_INTERNAL), true);
}

CNetAddr UtilBuildAddress(unsigned char p1, unsigned char p2, unsigned char p3, unsigned char p4)
{
    unsigned char ip[] = {p1, p2, p3, p4};

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sockaddr_in)); // initialize the memory block
    memcpy(&(sa.sin_addr), &ip, sizeof(ip));
    return CNetAddr(sa.sin_addr);
}


BOOST_AUTO_TEST_CASE(LimitedAndReachable_CNetAddr)
{
    CNetAddr addr = UtilBuildAddress(0x001, 0x001, 0x001, 0x001); // 1.1.1.1

    SetReachable(NET_IPV4, true);
    BOOST_CHECK_EQUAL(IsReachable(addr), true);

    SetReachable(NET_IPV4, false);
    BOOST_CHECK_EQUAL(IsReachable(addr), false);

    SetReachable(NET_IPV4, true); // have to reset this, because this is stateful.
}


BOOST_AUTO_TEST_CASE(LocalAddress_BasicLifecycle)
{
    CService addr = CService(UtilBuildAddress(0x002, 0x001, 0x001, 0x001), 1000); // 2.1.1.1:1000

    SetReachable(NET_IPV4, true);

    BOOST_CHECK_EQUAL(IsLocal(addr), false);
    BOOST_CHECK_EQUAL(AddLocal(addr, 1000), true);
    BOOST_CHECK_EQUAL(IsLocal(addr), true);

    RemoveLocal(addr);
    BOOST_CHECK_EQUAL(IsLocal(addr), false);
}

BOOST_AUTO_TEST_CASE(mainnet_dns_seeds)
{
    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto& seeds = chainParams->DNSSeeds();

    BOOST_CHECK(std::find(seeds.begin(), seeds.end(), "seed.defichain.live") != seeds.end());
    BOOST_CHECK(std::find(seeds.begin(), seeds.end(), "seed.mydeficha.in") != seeds.end());
    BOOST_CHECK(std::find(seeds.begin(), seeds.end(), "seed.defichain.io") == seeds.end());
}

BOOST_AUTO_TEST_CASE(gentxid_and_wtx_inv_test)
{
    uint256 h1 = InsecureRand256();
    uint256 h2 = InsecureRand256();

    GenTxid g_txid = GenTxid::Txid(h1);
    GenTxid g_wtxid = GenTxid::Wtxid(h1);
    BOOST_CHECK(!g_txid.IsWtxid());
    BOOST_CHECK(g_wtxid.IsWtxid());
    BOOST_CHECK(g_txid.GetHash() == h1);
    BOOST_CHECK(g_wtxid.GetHash() == h1);
    BOOST_CHECK(!(g_txid == g_wtxid));
    BOOST_CHECK(g_txid < g_wtxid);

    CInv inv_tx(MSG_TX, h1);
    CInv inv_wtx(MSG_WTX, h2);
    CInv inv_wit_tx(MSG_WITNESS_TX, h1);
    CInv inv_blk(MSG_BLOCK, h1);

    BOOST_CHECK(inv_tx.IsMsgTx() && !inv_tx.IsMsgWtx() && inv_tx.IsGenTxMsg());
    BOOST_CHECK(!inv_wtx.IsMsgTx() && inv_wtx.IsMsgWtx() && inv_wtx.IsGenTxMsg());
    BOOST_CHECK(inv_wit_tx.IsMsgWitnessTx() && inv_wit_tx.IsGenTxMsg());
    BOOST_CHECK(!inv_blk.IsGenTxMsg());
    BOOST_CHECK_EQUAL(inv_wtx.GetCommand(), "wtx");
    BOOST_CHECK(ToGenTxid(inv_tx) == GenTxid::Txid(h1));
    BOOST_CHECK(ToGenTxid(inv_wtx) == GenTxid::Wtxid(h2));
    BOOST_CHECK(ToGenTxid(inv_wit_tx) == GenTxid::Txid(h1));
}

BOOST_AUTO_TEST_CASE(connection_type_strings_and_node_predicates)
{
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::INBOUND), "inbound");
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::OUTBOUND_FULL_RELAY), "outbound-full-relay");
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::MANUAL), "manual");
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::FEELER), "feeler");
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::BLOCK_RELAY), "block-relay-only");
    BOOST_CHECK_EQUAL(ConnectionTypeAsString(ConnectionType::ADDR_FETCH), "addr-fetch");

    CAddress addr(CService(UtilBuildAddress(0x001, 0x002, 0x003, 0x004), 8555), NODE_NONE);

    CNode n_inbound(0, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::INBOUND);
    BOOST_CHECK(n_inbound.IsInboundConn() && n_inbound.fInbound);
    BOOST_CHECK(!n_inbound.IsFullOutboundConn() && !n_inbound.IsBlockOnlyConn() && !n_inbound.IsManualConn() && !n_inbound.IsFeelerConn() && !n_inbound.IsAddrFetchConn());

    CNode n_full(1, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::OUTBOUND_FULL_RELAY);
    BOOST_CHECK(n_full.IsFullOutboundConn() && !n_full.fInbound && !n_full.m_manual_connection && !n_full.fFeeler && !n_full.fOneShot);

    CNode n_block(2, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::BLOCK_RELAY);
    BOOST_CHECK(n_block.IsBlockOnlyConn() && !n_block.fInbound && !n_block.IsFullOutboundConn() && !n_block.m_manual_connection && !n_block.fFeeler && !n_block.fOneShot);
    CNodeStats stats;
    n_block.copyStats(stats);
    BOOST_CHECK(stats.m_conn_type == ConnectionType::BLOCK_RELAY);

    CNode n_manual(3, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::MANUAL);
    BOOST_CHECK(n_manual.IsManualConn() && n_manual.m_manual_connection && !n_manual.fInbound);

    CNode n_feeler(4, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::FEELER);
    BOOST_CHECK(n_feeler.IsFeelerConn() && n_feeler.fFeeler && !n_feeler.fInbound);

    CNode n_addr(5, NODE_NONE, 0, INVALID_SOCKET, addr, 0, 0, CAddress(), 0, "", ConnectionType::ADDR_FETCH);
    BOOST_CHECK(n_addr.IsAddrFetchConn() && n_addr.fOneShot && !n_addr.fInbound);
}

BOOST_AUTO_TEST_CASE(select_outbound_connection_type_order)
{
    // 1. Full-relay has highest priority when below target
    auto t = SelectOutboundConnectionType(/*nOutboundFullRelay=*/0, /*nMaxOutboundFullRelay=*/8, /*nOutboundBlockRelay=*/0, /*nMaxOutboundBlockRelay=*/2, /*try_new_outbound=*/true, /*feeler_due=*/true);
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK(*t == ConnectionType::OUTBOUND_FULL_RELAY);

    // 2. Once full-relay is full, block-relay-only is selected before extra full-relay or feeler
    t = SelectOutboundConnectionType(8, 8, 0, 2, true, true);
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK(*t == ConnectionType::BLOCK_RELAY);

    // 3. Default-off (nMaxOutboundBlockRelay == 0) never selects BLOCK_RELAY
    t = SelectOutboundConnectionType(8, 8, 0, 0, false, false);
    BOOST_CHECK(!t.has_value());
    t = SelectOutboundConnectionType(8, 8, 0, 0, true, true);
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK(*t == ConnectionType::OUTBOUND_FULL_RELAY);

    // 4. Once both full-relay and block-relay are full, try_new_outbound selects OUTBOUND_FULL_RELAY
    t = SelectOutboundConnectionType(8, 8, 2, 2, true, true);
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK(*t == ConnectionType::OUTBOUND_FULL_RELAY);

    // 5. When no extra outbound is requested, feeler_due selects FEELER
    t = SelectOutboundConnectionType(8, 8, 2, 2, false, true);
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK(*t == ConnectionType::FEELER);

    // 6. Otherwise nothing to open
    t = SelectOutboundConnectionType(8, 8, 2, 2, false, false);
    BOOST_CHECK(!t.has_value());
}

BOOST_AUTO_TEST_CASE(connman_outbound_slot_clamping)
{
    // Default-off: nMaxOutboundBlockRelay = 0
    {
        CConnman connman(0x1337, 0x1337);
        CConnman::Options opts;
        opts.nMaxConnections = 125;
        opts.nMaxOutboundFullRelay = 8;
        opts.nMaxOutboundBlockRelay = 0;
        opts.nMaxFeeler = 1;
        connman.Init(opts);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundFullRelay(), 8);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundBlockRelay(), 0);
        BOOST_CHECK_EQUAL(connman.GetMaxInbound(), 116);
    }

    // Opt-in block-relay-only: reduces inbound budget accordingly
    {
        CConnman connman(0x1337, 0x1337);
        CConnman::Options opts;
        opts.nMaxConnections = 125;
        opts.nMaxOutboundFullRelay = 8;
        opts.nMaxOutboundBlockRelay = 2;
        opts.nMaxFeeler = 1;
        connman.Init(opts);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundFullRelay(), 8);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundBlockRelay(), 2);
        BOOST_CHECK_EQUAL(connman.GetMaxInbound(), 114);
    }

    // Small nMaxConnections clamps block-relay-only to remaining budget (accounting for nMaxFeeler)
    {
        CConnman connman(0x1337, 0x1337);
        CConnman::Options opts;
        opts.nMaxConnections = 9;
        opts.nMaxOutboundFullRelay = 8;
        opts.nMaxOutboundBlockRelay = 4;
        opts.nMaxFeeler = 1;
        connman.Init(opts);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundFullRelay(), 8);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundBlockRelay(), 0);
        BOOST_CHECK_EQUAL(connman.GetMaxInbound(), 0);
    }

    // Tight nMaxConnections=10 leaves 1 slot for block-relay-only and 0 for inbound (never negative)
    {
        CConnman connman(0x1337, 0x1337);
        CConnman::Options opts;
        opts.nMaxConnections = 10;
        opts.nMaxOutboundFullRelay = 8;
        opts.nMaxOutboundBlockRelay = 4;
        opts.nMaxFeeler = 1;
        connman.Init(opts);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundFullRelay(), 8);
        BOOST_CHECK_EQUAL(connman.GetMaxOutboundBlockRelay(), 1);
        BOOST_CHECK_EQUAL(connman.GetMaxInbound(), 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
