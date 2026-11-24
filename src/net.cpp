// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/defi-config.h>
#endif

#include <net.h>

#include <banman.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/consensus.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <fs.h>
#include <netbase.h>
#include <net_permissions.h>
#include <primitives/transaction.h>
#include <scheduler.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/translation.h>

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_POLL
#include <poll.h>
#endif

#include <unordered_map>

#include <math.h>

// Dump addresses to peers.dat every 15 minutes (900s)
static constexpr int DUMP_PEERS_INTERVAL = 15 * 60;

// We add a random period time (0 to 1 seconds) to feeler connections to prevent synchronization.
#define FEELER_SLEEP_WINDOW 1

// MSG_NOSIGNAL is not available on some platforms, if it doesn't exist define it as 0
#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// MSG_DONTWAIT is not available on some platforms, if it doesn't exist define it as 0
#if !defined(MSG_DONTWAIT)
#define MSG_DONTWAIT 0
#endif

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE         = 0,
    BF_EXPLICIT     = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
};

// The set of sockets cannot be modified while waiting
// The sleep time needs to be small to avoid new sockets stalling
static const uint64_t SELECT_TIMEOUT_MILLISECONDS = 50;

const std::string NET_MESSAGE_COMMAND_OTHER = "*other*";

static const uint64_t RANDOMIZER_ID_NETGROUP = 0x6c0edd8036ef4036ULL; // SHA256("netgroup")[0:8]
static const uint64_t RANDOMIZER_ID_LOCALHOSTNONCE = 0xd93e69e2bbfa5735ULL; // SHA256("localhostnonce")[0:8]
//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
bool g_relay_txes = !DEFAULT_BLOCKSONLY;
CCriticalSection cs_mapLocalHost;
std::map<CNetAddr, LocalServiceInfo> mapLocalHost GUARDED_BY(cs_mapLocalHost);
static bool vfLimited[NET_MAX] GUARDED_BY(cs_mapLocalHost) = {};
std::string strSubVersion;


void CConnman::AddOneShot(const std::string& strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(gArgs.GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (const auto& entry : mapLocalHost)
        {
            int nScore = entry.second.nScore;
            int nReachability = entry.first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService(entry.first, entry.second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the pnSeed6 array into usable address objects.
static std::vector<CAddress> convertSeed6(const std::vector<SeedSpec6> &vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7*24*60*60;
    std::vector<CAddress> vSeedsOut;
    vSeedsOut.reserve(vSeedsIn.size());
    FastRandomContext rng;
    for (const auto& seed_in : vSeedsIn) {
        struct in6_addr ip;
        memcpy(&ip, seed_in.addr, sizeof(ip));
        CAddress addr(CService(ip, seed_in.port), GetDesirableServiceFlags(NODE_NONE));
        addr.nTime = GetTime() - rng.randrange(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr *paddrPeer, ServiceFlags nLocalServices)
{
    CAddress ret(CService(CNetAddr(),GetListenPort()), nLocalServices);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr, nLocalServices);
    }
    ret.nTime = GetAdjustedTime();
    return ret;
}

static int GetnScore(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == 0) return 0;
    return mapLocalHost[addr].nScore;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CNode *pnode)
{
    CService addrLocal = pnode->GetAddrLocal();
    return fDiscover && pnode->addr.IsRoutable() && addrLocal.IsRoutable() &&
           IsReachable(addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertiseLocal(CNode *pnode)
{
    if (fListen && pnode->fSuccessfullyConnected)
    {
        CAddress addrLocal = GetLocalAddress(&pnode->addr, pnode->GetLocalServices());
        if (gArgs.GetBoolArg("-addrmantest", false)) {
            // use IPv4 loopback during addrmantest
            addrLocal = CAddress(CService(LookupNumeric("127.0.0.1", GetListenPort())), pnode->GetLocalServices());
        }
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        FastRandomContext rng;
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
             rng.randbits((GetnScore(addrLocal) > LOCAL_MANUAL) ? 3 : 1) == 0))
        {
            addrLocal.SetIP(pnode->GetAddrLocal());
        }
        if (addrLocal.IsRoutable() || gArgs.GetBoolArg("-addrmantest", false))
        {
            LogPrint(BCLog::NET, "AdvertiseLocal: advertising address %s\n", addrLocal.ToString());
            pnode->PushAddress(addrLocal, rng);
        }
    }
    // The local privacy network services are announced as well. They have no
    // representation in the legacy (pre-BIP155) wire format, so they only
    // reach the peers that announced the support for the BIP155 address
    // relay.
    AdvertiseLocalPrivacy(pnode);
}

/**
 * The local privacy network services, i.e. the Tor v3 onion services and the
 * I2P destinations that the node knows about.
 */
static std::vector<CService> GetLocalPrivacyServices()
{
    std::vector<CService> vServices;
    LOCK(cs_mapLocalHost);
    for (const auto& entry : mapLocalHost) {
        const CNetAddr& addr = entry.first;
        if (addr.IsTorV3() || addr.IsI2P()) {
            vServices.emplace_back(addr, entry.second.nPort);
        }
    }
    return vServices;
}

// pushes our own privacy network services to a peer
void AdvertiseLocalPrivacy(CNode* pnode)
{
    FastRandomContext rng;
    for (const CService& service : GetLocalPrivacyServices()) {
        CAddress addr(service, pnode->GetLocalServices());
        addr.nTime = GetAdjustedTime();
        LogPrint(BCLog::NET, "AdvertiseLocal: advertising address %s\n", addr.ToString());
        // The address is only queued for the peers that can decode it, i.e.
        // the peers that announced the support for the BIP155 address relay.
        pnode->PushAddress(addr, rng);
    }
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (!IsReachable(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

void RemoveLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
}

void SetReachable(enum Network net, bool reachable)
{
    if (net == NET_UNROUTABLE || net == NET_INTERNAL)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = !reachable;
}

bool IsReachable(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
}

bool IsReachable(const CNetAddr &addr)
{
    return IsReachable(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

CNode* CConnman::FindNode(const CNetAddr& ip)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
      if (static_cast<CNetAddr>(pnode->addr) == ip) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const CSubNet& subNet)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (subNet.Match(static_cast<CNetAddr>(pnode->addr))) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (pnode->GetAddrName() == addrName) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const CService& addr)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (static_cast<CService>(pnode->addr) == addr) {
            return pnode;
        }
    }
    return nullptr;
}

bool CConnman::CheckIncomingNonce(uint64_t nonce)
{
    LOCK(cs_vNodes);
    for (const CNode* pnode : vNodes) {
        if (!pnode->fSuccessfullyConnected && !pnode->fInbound && pnode->GetLocalNonce() == nonce)
            return false;
    }
    return true;
}

/** Get the bind address for a socket as CAddress */
static CAddress GetBindAddress(SOCKET sock)
{
    CAddress addr_bind;
    struct sockaddr_storage sockaddr_bind;
    socklen_t sockaddr_bind_len = sizeof(sockaddr_bind);
    if (sock != INVALID_SOCKET) {
        if (!getsockname(sock, (struct sockaddr*)&sockaddr_bind, &sockaddr_bind_len)) {
            addr_bind.SetSockAddr((const struct sockaddr*)&sockaddr_bind);
        } else {
            LogPrint(BCLog::NET, "Warning: getsockname failed\n");
        }
    }
    return addr_bind;
}

/**
 * Normalize the port of an I2P address.
 *
 * I2P destinations are portless (the SAM v3.1 protocol has no notion of a
 * destination port), so the port of an I2P CService is always 0. The
 * normalization has to happen before any CService comparison (FindNode(),
 * CNode address equality, netgroups), so that "<addr>" and "<addr>:1234"
 * refer to the same peer.
 */
static CAddress NormalizeI2PAddress(const CAddress& addr)
{
    if (!addr.IsI2P()) return addr;
    return CAddress(i2p::NormalizeI2PService(addr), addr.nServices);
}

CNode* CConnman::ConnectNode(CAddress addrConnect, const char *pszDest, bool fCountFailure, bool manual_connection, bool use_v2transport)
{
    // I2P destinations are portless: normalize the port before the address is
    // compared with the addresses of the already connected peers.
    addrConnect = NormalizeI2PAddress(addrConnect);

    if (pszDest == nullptr) {
        if (IsLocal(addrConnect))
            return nullptr;

        // Look for an existing connection
        CNode* pnode = FindNode(static_cast<CService>(addrConnect));
        if (pnode)
        {
            LogPrintf("Failed to open new connection, already connected\n");
            return nullptr;
        }
    }

    /// debug print
    LogPrint(BCLog::NET, "trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString(),
        pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Resolve
    const int default_port = Params().GetDefaultPort();
    if (pszDest) {
        std::vector<CService> resolved;
        if (Lookup(pszDest, resolved,  default_port, fNameLookup && !HaveNameProxy(), 256) && !resolved.empty()) {
            addrConnect = CAddress(resolved[GetRand(resolved.size())], NODE_NONE);
            if (!addrConnect.IsValid()) {
                LogPrint(BCLog::NET, "Resolver returned invalid address %s for %s\n", addrConnect.ToString(), pszDest);
                return nullptr;
            }
            // It is possible that we already have a connection to the IP/port pszDest resolved to.
            // In that case, drop the connection that was just created, and return the existing CNode instead.
            // Also store the name we used to connect in that CNode, so that future FindNode() calls to that
            // name catch this early.
            LOCK(cs_vNodes);
            CNode* pnode = FindNode(static_cast<CService>(addrConnect));
            if (pnode)
            {
                pnode->MaybeSetAddrName(std::string(pszDest));
                LogPrintf("Failed to open new connection, already connected\n");
                return nullptr;
            }
        }
    }

    // The resolved address may carry the default port of the chain: the I2P
    // destinations are portless, so the port is normalized here as well.
    addrConnect = NormalizeI2PAddress(addrConnect);

    // Connect
    bool connected = false;
    SOCKET hSocket = INVALID_SOCKET;
    proxyType proxy;
    i2p::Connection i2p_conn;
    if (addrConnect.IsValid()) {
        bool proxyConnectionFailed = false;

        if (addrConnect.IsI2P()) {
            // The I2P transport is not a SOCKS5 proxy: the destination is
            // reached through the SAM v3 bridge of the I2P session.
            if (!m_i2p_sam_session) {
                LogPrint(BCLog::I2P, "Cannot connect to %s: no I2P SAM bridge is configured\n",
                         addrConnect.ToStringIP());
                return nullptr;
            }
            connected = m_i2p_sam_session->Connect(addrConnect, i2p_conn, proxyConnectionFailed);
            if (connected) {
                hSocket = i2p_conn.sock;
            }
        } else if (GetProxy(addrConnect.GetNetwork(), proxy)) {
            hSocket = CreateSocket(proxy.proxy);
            if (hSocket == INVALID_SOCKET) {
                return nullptr;
            }
            connected = ConnectThroughProxy(proxy, addrConnect.ToStringIP(), addrConnect.GetPort(), hSocket, nConnectTimeout, &proxyConnectionFailed);
        } else {
            // no proxy needed (none set for target network)
            hSocket = CreateSocket(addrConnect);
            if (hSocket == INVALID_SOCKET) {
                return nullptr;
            }
            connected = ConnectSocketDirectly(addrConnect, hSocket, nConnectTimeout, manual_connection);
        }
        if (!proxyConnectionFailed) {
            // If a connection to the node was attempted, and failure (if any) is not caused by a problem connecting to
            // the proxy, mark this as an attempt.
            addrman.Attempt(addrConnect, fCountFailure);
        }
    } else if (pszDest && GetNameProxy(proxy)) {
        hSocket = CreateSocket(proxy.proxy);
        if (hSocket == INVALID_SOCKET) {
            return nullptr;
        }
        std::string host;
        int port = default_port;
        SplitHostPort(std::string(pszDest), port, host);
        connected = ConnectThroughProxy(proxy, host, port, hSocket, nConnectTimeout, nullptr);
    }
    if (!connected) {
        CloseSocket(hSocket);
        return nullptr;
    }

    // Add node
    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
    // The local address of an I2P connection is our own I2P destination and
    // not the TCP endpoint of the SAM bridge: the endpoint of the bridge must
    // not leak into the peer information or into the netgroups.
    CAddress addr_bind = addrConnect.IsI2P() ? CAddress(i2p_conn.me, NODE_NONE) : GetBindAddress(hSocket);
    // For outbound connections, the port of the bound address is randomly
    // assigned by the OS and would therefore not be useful for seeding.
    const uint64_t network_key = m_addr_response_cache.ComputeKey(addrConnect, addr_bind, false);
    auto transport = CreateNodeTransport(id, /*inbound=*/false, use_v2transport);
    CNode* pnode = new CNode(id, nLocalServices, GetBestHeight(), hSocket, addrConnect, CalculateKeyedNetGroup(addrConnect), nonce, addr_bind, network_key, pszDest ? pszDest : "", false, std::move(transport));
    pnode->AddRef();

    return pnode;
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    LOCK(cs_hSocket);
    if (hSocket != INVALID_SOCKET)
    {
        LogPrint(BCLog::NET, "disconnecting peer=%d\n", id);
        CloseSocket(hSocket);
    }
}

void CConnman::AddWhitelistPermissionFlags(NetPermissionFlags& flags, const CNetAddr &addr) const {
    for (const auto& subnet : vWhitelistedRange) {
        if (subnet.m_subnet.Match(addr)) NetPermissions::AddFlag(flags, subnet.m_flags);
    }
}

std::string CNode::GetAddrName() const {
    LOCK(cs_addrName);
    return addrName;
}

void CNode::MaybeSetAddrName(const std::string& addrNameIn) {
    LOCK(cs_addrName);
    if (addrName.empty()) {
        addrName = addrNameIn;
    }
}

CService CNode::GetAddrLocal() const {
    LOCK(cs_addrLocal);
    return addrLocal;
}

void CNode::SetAddrLocal(const CService& addrLocalIn) {
    LOCK(cs_addrLocal);
    if (addrLocal.IsValid()) {
        error("Addr local already set for node: %i. Refusing to change from %s to %s", id, addrLocal.ToString(), addrLocalIn.ToString());
    } else {
        addrLocal = addrLocalIn;
    }
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(addr);
    X(addrBind);
    {
        LOCK(cs_filter);
        X(fRelayTxes);
    }
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    stats.addrName = GetAddrName();
    X(nVersion);
    {
        LOCK(cs_SubVer);
        X(cleanSubVer);
    }
    X(fInbound);
    X(m_bip152_highbandwidth_to);
    X(m_bip152_highbandwidth_from);
    X(m_wants_addrv2);
    X(m_manual_connection);
    X(nStartingHeight);
    {
        LOCK(cs_vSend);
        X(mapSendBytesPerMsgCmd);
        X(nSendBytes);
    }
    {
        LOCK(cs_vRecv);
        X(mapRecvBytesPerMsgCmd);
        X(nRecvBytes);
    }
    X(m_legacyWhitelisted);
    X(m_permissionFlags);
    {
        LOCK(cs_feeFilter);
        X(minFeeFilter);
    }
    X(nProcessedAddrs);
    X(nRatelimitedAddrs);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (Defi users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dMinPing  = (((double)nMinPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    CService addrLocalUnlocked = GetAddrLocal();
    stats.addrLocal = addrLocalUnlocked.IsValid() ? addrLocalUnlocked.ToString() : "";

    const auto transport_info = m_transport->GetInfo();
    stats.m_transport_type = transport_info.transport_type;
    stats.m_session_id = transport_info.session_id ? HexStr(transport_info.session_id->begin(), transport_info.session_id->end()) : "";
}
#undef X

std::string TransportTypeAsString(TransportProtocolType transport_type)
{
    switch (transport_type) {
    case TransportProtocolType::DETECTING: return "detecting";
    case TransportProtocolType::V1: return "v1";
    case TransportProtocolType::V2: return "v2";
    }
    assert(false);
}

V1Transport::V1Transport(const NodeId node_id) noexcept
    : m_magic_bytes{Params().MessageStartPostAMK()[0], Params().MessageStartPostAMK()[1],
                    Params().MessageStartPostAMK()[2], Params().MessageStartPostAMK()[3]},
      m_node_id{node_id},
      hdrbuf(SER_NETWORK, INIT_PROTO_VERSION), hdr(m_magic_bytes), vRecv(SER_NETWORK, INIT_PROTO_VERSION)
{
    LOCK(m_recv_mutex);
    Reset();
}

Transport::Info V1Transport::GetInfo() const noexcept
{
    return {.transport_type = TransportProtocolType::V1, .session_id = std::nullopt};
}

int V1Transport::readHeader(std::span<const uint8_t> msg_bytes)
{
    AssertLockHeld(m_recv_mutex);
    unsigned int nRemaining = CMessageHeader::HEADER_SIZE - nHdrPos;
    unsigned int nCopy = std::min<unsigned int>(nRemaining, msg_bytes.size());

    memcpy(&hdrbuf[nHdrPos], msg_bytes.data(), nCopy);
    nHdrPos += nCopy;

    if (nHdrPos < CMessageHeader::HEADER_SIZE)
        return nCopy;

    try {
        hdrbuf >> hdr;
    }
    catch (const std::exception&) {
        LogPrint(BCLog::NET, "Header error: Unable to deserialize, peer=%d\n", m_node_id);
        return -1;
    }

    if (memcmp(hdr.pchMessageStart, m_magic_bytes, CMessageHeader::MESSAGE_START_SIZE) != 0) {
        LogPrint(BCLog::NET, "PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(hdr.GetCommand()), m_node_id);
        return -1;
    }

    if (hdr.nMessageSize > MAX_DESER_SIZE || hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
        LogPrint(BCLog::NET, "Oversized message from peer=%i, disconnecting\n", m_node_id);
        return -1;
    }

    in_data = true;
    return nCopy;
}

int V1Transport::readData(std::span<const uint8_t> msg_bytes)
{
    AssertLockHeld(m_recv_mutex);
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min<unsigned int>(nRemaining, msg_bytes.size());

    if (vRecv.size() < nDataPos + nCopy) {
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    hasher.Write(msg_bytes.data(), nCopy);
    memcpy(&vRecv[nDataPos], msg_bytes.data(), nCopy);
    nDataPos += nCopy;

    return nCopy;
}

const uint256& V1Transport::GetMessageHash() const
{
    AssertLockHeld(m_recv_mutex);
    assert(CompleteInternal());
    if (data_hash.IsNull())
        hasher.Finalize(data_hash.begin());
    return data_hash;
}

CNetMessage V1Transport::GetReceivedMessage(int64_t time, bool& reject_message)
{
    reject_message = false;
    LOCK(m_recv_mutex);
    CNetMessage msg(std::move(vRecv));

    msg.m_type = hdr.GetCommand();
    msg.m_time = time;
    msg.m_message_size = hdr.nMessageSize;
    msg.m_raw_message_size = hdr.nMessageSize + CMessageHeader::HEADER_SIZE;

    uint256 hash = GetMessageHash();

    if (memcmp(hash.begin(), hdr.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): CHECKSUM ERROR expected %s was %s\n", __func__,
            SanitizeString(msg.m_type), msg.m_message_size,
            HexStr(hash.begin(), hash.begin() + CMessageHeader::CHECKSUM_SIZE),
            HexStr(hdr.pchChecksum, hdr.pchChecksum + CMessageHeader::CHECKSUM_SIZE));
        reject_message = true;
    } else if (!hdr.IsValid(m_magic_bytes)) {
        LogPrint(BCLog::NET, "PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(msg.m_type), m_node_id);
        reject_message = true;
    }

    Reset();
    return msg;
}

bool V1Transport::SetMessageToSend(CSerializedNetMsg& msg) noexcept
{
    LOCK(m_send_mutex);
    if (m_sending_header || m_bytes_sent < m_message_to_send.data.size()) return false;

    uint256 hash = Hash(msg.data.data(), msg.data.data() + msg.data.size());
    CMessageHeader hdr_out(m_magic_bytes, msg.command.c_str(), msg.data.size());
    memcpy(hdr_out.pchChecksum, hash.begin(), CMessageHeader::CHECKSUM_SIZE);

    m_header_to_send.clear();
    m_header_to_send.reserve(CMessageHeader::HEADER_SIZE);
    CVectorWriter{SER_NETWORK, INIT_PROTO_VERSION, m_header_to_send, 0, hdr_out};

    m_message_to_send = std::move(msg);
    m_sending_header = true;
    m_bytes_sent = 0;
    return true;
}

Transport::BytesToSend V1Transport::GetBytesToSend(bool have_next_message) const noexcept
{
    LOCK(m_send_mutex);
    if (m_sending_header) {
        return {std::span{m_header_to_send}.subspan(m_bytes_sent),
                have_next_message || !m_message_to_send.data.empty(),
                m_message_to_send.command};
    } else {
        return {std::span{m_message_to_send.data}.subspan(m_bytes_sent),
                have_next_message,
                m_message_to_send.command};
    }
}

void V1Transport::MarkBytesSent(size_t bytes_sent) noexcept
{
    LOCK(m_send_mutex);
    m_bytes_sent += bytes_sent;
    if (m_sending_header && m_bytes_sent == m_header_to_send.size()) {
        m_sending_header = false;
        m_bytes_sent = 0;
        m_header_to_send.clear();
    } else if (!m_sending_header && m_bytes_sent == m_message_to_send.data.size()) {
        m_bytes_sent = 0;
        m_message_to_send.data.clear();
        m_message_to_send.data.shrink_to_fit();
    }
}

size_t V1Transport::GetSendMemoryUsage() const noexcept
{
    LOCK(m_send_mutex);
    return m_header_to_send.size() + m_message_to_send.data.size();
}

namespace {

/** BIP324 short message type ID table (exact upstream slots 1..37; DFI-specific commands use 13-byte fallback). */
const std::array<std::string, BIP324_SHORTIDS_IMPLEMENTED> V2_MESSAGE_IDS = {
    "", // 0: 12 bytes follow encoding the message type like in V1
    NetMsgType::ADDR,
    NetMsgType::BLOCK,
    NetMsgType::BLOCKTXN,
    NetMsgType::CMPCTBLOCK,
    NetMsgType::FEEFILTER,
    NetMsgType::FILTERADD,
    NetMsgType::FILTERCLEAR,
    NetMsgType::FILTERLOAD,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETBLOCKTXN,
    NetMsgType::GETDATA,
    NetMsgType::GETHEADERS,
    NetMsgType::HEADERS,
    NetMsgType::INV,
    NetMsgType::MEMPOOL,
    NetMsgType::MERKLEBLOCK,
    NetMsgType::NOTFOUND,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::SENDCMPCT,
    NetMsgType::TX,
    "getcfilters",
    "cfilter",
    "getcfheaders",
    "cfheaders",
    "getcfcheckpt",
    "cfcheckpt",
    NetMsgType::ADDRV2,
    "", "", "", // Unimplemented message types 29-31
    "", "", "", "", // Unimplemented message types 32-35
    "", // Unimplemented message type 36
    "feature",
};

class V2MessageMap
{
    std::unordered_map<std::string, uint8_t> m_map;

public:
    V2MessageMap() noexcept
    {
        for (size_t i = 1; i < std::size(V2_MESSAGE_IDS); ++i) {
            if (!V2_MESSAGE_IDS[i].empty()) {
                m_map.emplace(V2_MESSAGE_IDS[i], static_cast<uint8_t>(i));
            }
        }
    }

    std::optional<uint8_t> operator()(const std::string& message_name) const noexcept
    {
        auto it = m_map.find(message_name);
        if (it == m_map.end()) return std::nullopt;
        return it->second;
    }
};

const V2MessageMap V2_MESSAGE_MAP;

std::vector<uint8_t> GenerateRandomGarbage() noexcept
{
    std::vector<uint8_t> ret;
    FastRandomContext rng;
    size_t len = rng.randrange(V2Transport::MAX_GARBAGE_LEN + 1);
    ret.resize(len);
    if (len > 0) {
        auto rand_bytes = rng.randbytes(len);
        std::copy(rand_bytes.begin(), rand_bytes.end(), ret.begin());
    }
    return ret;
}

template <typename T>
void ClearShrink(T& container) noexcept
{
    container.clear();
    container.shrink_to_fit();
}

} // namespace

std::unique_ptr<Transport> CConnman::CreateNodeTransport(NodeId id, bool inbound, bool use_v2transport) const
{
    if (use_v2transport) {
        return std::make_unique<V2Transport>(id, /*initiating=*/!inbound);
    }
    return std::make_unique<V1Transport>(id);
}

void V2Transport::StartSendingHandshake() noexcept
{
    AssertLockHeld(m_send_mutex);
    assert(m_send_state == SendState::AWAITING_KEY);
    assert(m_send_buffer.empty());
    m_send_buffer.resize(EllSwiftPubKey::size() + m_send_garbage.size());
    std::copy(std::begin(m_cipher.GetOurPubKey()), std::end(m_cipher.GetOurPubKey()), MakeWritableByteSpan(m_send_buffer).begin());
    std::copy(m_send_garbage.begin(), m_send_garbage.end(), m_send_buffer.begin() + EllSwiftPubKey::size());
}

V2Transport::V2Transport(NodeId nodeid, bool initiating, const CKey& key, std::span<const std::byte> ent32, std::vector<uint8_t> garbage) noexcept
    : m_cipher{key, ent32},
      m_initiating{initiating},
      m_nodeid{nodeid},
      m_v1_fallback{nodeid},
      m_recv_state{initiating ? RecvState::KEY : RecvState::KEY_MAYBE_V1},
      m_send_garbage{std::move(garbage)},
      m_send_state{initiating ? SendState::AWAITING_KEY : SendState::MAYBE_V1}
{
    assert(m_send_garbage.size() <= MAX_GARBAGE_LEN);
    if (initiating) {
        LOCK(m_send_mutex);
        StartSendingHandshake();
    }
}

V2Transport::V2Transport(NodeId nodeid, bool initiating) noexcept
    : V2Transport{nodeid, initiating, GenerateRandomKey(),
                  MakeByteSpan(GetRandHash()), GenerateRandomGarbage()} {}

Transport::Info V2Transport::GetInfo() const noexcept
{
    LOCK(m_recv_mutex);
    if (m_recv_state == RecvState::V1) return m_v1_fallback.GetInfo();
    if (m_recv_state == RecvState::KEY_MAYBE_V1) {
        return {.transport_type = TransportProtocolType::DETECTING, .session_id = std::nullopt};
    }
    std::optional<uint256> session_id;
    if (m_cipher) {
        uint256 sid;
        auto sid_span = m_cipher.GetSessionID();
        std::copy(UCharCast(sid_span.data()), UCharCast(sid_span.data()) + sid_span.size(), sid.begin());
        session_id = sid;
    }
    return {.transport_type = TransportProtocolType::V2, .session_id = session_id};
}

void V2Transport::SetReceiveState(RecvState recv_state) noexcept
{
    AssertLockHeld(m_recv_mutex);
    m_recv_state = recv_state;
}

void V2Transport::SetSendState(SendState send_state) noexcept
{
    AssertLockHeld(m_send_mutex);
    m_send_state = send_state;
}

bool V2Transport::ReceivedMessageComplete() const noexcept
{
    LOCK(m_recv_mutex);
    if (m_recv_state == RecvState::V1) return m_v1_fallback.ReceivedMessageComplete();
    return m_recv_state == RecvState::APP_READY;
}

void V2Transport::ProcessReceivedMaybeV1Bytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    assert(m_recv_state == RecvState::KEY_MAYBE_V1);
    std::array<uint8_t, V1_PREFIX_LEN> v1_prefix = {0, 0, 0, 0, 'v', 'e', 'r', 's', 'i', 'o', 'n', 0, 0, 0, 0, 0};
    const auto& magic = Params().MessageStartPostAMK();
    std::copy(std::begin(magic), std::end(magic), v1_prefix.begin());
    assert(m_recv_buffer.size() <= v1_prefix.size());
    if (!std::equal(m_recv_buffer.begin(), m_recv_buffer.end(), v1_prefix.begin())) {
        SetReceiveState(RecvState::KEY);
        LOCK(m_send_mutex);
        SetSendState(SendState::AWAITING_KEY);
        StartSendingHandshake();
    } else if (m_recv_buffer.size() == v1_prefix.size()) {
        LOCK(m_send_mutex);
        std::span<const uint8_t> feedback{m_recv_buffer};
        bool ret = m_v1_fallback.ReceivedBytes(feedback);
        assert(feedback.empty());
        assert(ret);
        SetReceiveState(RecvState::V1);
        SetSendState(SendState::V1);
        ClearShrink(m_recv_buffer);
        ClearShrink(m_send_buffer);
    }
}

bool V2Transport::ProcessReceivedKeyBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    assert(m_recv_state == RecvState::KEY);
    assert(m_recv_buffer.size() <= EllSwiftPubKey::size());

    static constexpr std::array<uint8_t, 12> MATCH = {'v', 'e', 'r', 's', 'i', 'o', 'n', 0, 0, 0, 0, 0};
    static constexpr size_t OFFSET = CMessageHeader::MESSAGE_START_SIZE;
    if (!m_initiating && m_recv_buffer.size() >= OFFSET + MATCH.size()) {
        if (std::equal(MATCH.begin(), MATCH.end(), m_recv_buffer.begin() + OFFSET)) {
            LogPrint(BCLog::NET, "V2 transport error: V1 peer with wrong MessageStart %s\n",
                     HexStr(m_recv_buffer.begin(), m_recv_buffer.begin() + OFFSET));
            return false;
        }
    }

    if (m_recv_buffer.size() == EllSwiftPubKey::size()) {
        EllSwiftPubKey ellswift(MakeByteSpan(m_recv_buffer));
        LOCK(m_send_mutex);
        m_cipher.Initialize(ellswift, m_initiating);

        SetReceiveState(RecvState::GARB_GARBTERM);
        m_recv_buffer.clear();

        SetSendState(SendState::READY);

        m_send_buffer.resize(m_send_buffer.size() + BIP324Cipher::GARBAGE_TERMINATOR_LEN);
        std::copy(m_cipher.GetSendGarbageTerminator().begin(),
                  m_cipher.GetSendGarbageTerminator().end(),
                  MakeWritableByteSpan(m_send_buffer).last(BIP324Cipher::GARBAGE_TERMINATOR_LEN).begin());

        m_send_buffer.resize(m_send_buffer.size() + BIP324Cipher::EXPANSION + VERSION_CONTENTS.size());
        m_cipher.Encrypt(
            VERSION_CONTENTS,
            MakeByteSpan(m_send_garbage),
            false,
            MakeWritableByteSpan(m_send_buffer).last(BIP324Cipher::EXPANSION + VERSION_CONTENTS.size()));
        ClearShrink(m_send_garbage);
    }
    return true;
}

bool V2Transport::ProcessReceivedGarbageBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    assert(m_recv_state == RecvState::GARB_GARBTERM);
    assert(m_recv_buffer.size() <= MAX_GARBAGE_LEN + BIP324Cipher::GARBAGE_TERMINATOR_LEN);
    if (m_recv_buffer.size() >= BIP324Cipher::GARBAGE_TERMINATOR_LEN) {
        auto last_bytes = MakeByteSpan(m_recv_buffer).last(BIP324Cipher::GARBAGE_TERMINATOR_LEN);
        auto expected = m_cipher.GetReceiveGarbageTerminator();
        if (std::equal(last_bytes.begin(), last_bytes.end(), expected.begin(), expected.end())) {
            m_recv_aad = std::move(m_recv_buffer);
            m_recv_aad.resize(m_recv_aad.size() - BIP324Cipher::GARBAGE_TERMINATOR_LEN);
            m_recv_buffer.clear();
            SetReceiveState(RecvState::VERSION);
        } else if (m_recv_buffer.size() == MAX_GARBAGE_LEN + BIP324Cipher::GARBAGE_TERMINATOR_LEN) {
            LogPrint(BCLog::NET, "V2 transport error: missing garbage terminator, peer=%d\n", m_nodeid);
            return false;
        }
    }
    return true;
}

bool V2Transport::ProcessReceivedPacketBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    assert(m_recv_state == RecvState::VERSION || m_recv_state == RecvState::APP);

    static constexpr size_t MAX_CONTENTS_LEN = std::min<size_t>(
        1 + CMessageHeader::COMMAND_SIZE + std::min<size_t>(MAX_DESER_SIZE, MAX_PROTOCOL_MESSAGE_LENGTH),
        MAX_V2_CONTENTS_LEN);

    if (m_recv_buffer.size() == BIP324Cipher::LENGTH_LEN) {
        m_recv_len = m_cipher.DecryptLength(MakeByteSpan(m_recv_buffer));
        if (m_recv_len > MAX_CONTENTS_LEN) {
            LogPrint(BCLog::NET, "V2 transport error: packet too large (%u bytes), peer=%d\n", m_recv_len, m_nodeid);
            return false;
        }
    } else if (m_recv_buffer.size() > BIP324Cipher::LENGTH_LEN && m_recv_buffer.size() == m_recv_len + BIP324Cipher::EXPANSION) {
        m_recv_decode_buffer.resize(m_recv_len);
        bool ignore{false};
        bool ret = m_cipher.Decrypt(
            MakeByteSpan(m_recv_buffer).subspan(BIP324Cipher::LENGTH_LEN),
            MakeByteSpan(m_recv_aad),
            ignore,
            MakeWritableByteSpan(m_recv_decode_buffer));
        if (!ret) {
            LogPrint(BCLog::NET, "V2 transport error: packet decryption failure (%u bytes), peer=%d\n", m_recv_len, m_nodeid);
            return false;
        }
        ClearShrink(m_recv_aad);

        if (!ignore) {
            switch (m_recv_state) {
            case RecvState::VERSION:
                SetReceiveState(RecvState::APP);
                break;
            case RecvState::APP:
                SetReceiveState(RecvState::APP_READY);
                break;
            default:
                assert(false);
            }
        }
        ClearShrink(m_recv_buffer);
        if (m_recv_state != RecvState::APP_READY) ClearShrink(m_recv_decode_buffer);
    }
    return true;
}

size_t V2Transport::GetMaxBytesToProcess() noexcept
{
    AssertLockHeld(m_recv_mutex);
    switch (m_recv_state) {
    case RecvState::KEY_MAYBE_V1:
        return V1_PREFIX_LEN - m_recv_buffer.size();
    case RecvState::KEY:
        return EllSwiftPubKey::size() - m_recv_buffer.size();
    case RecvState::GARB_GARBTERM:
        return 1;
    case RecvState::VERSION:
    case RecvState::APP:
        if (m_recv_buffer.size() < BIP324Cipher::LENGTH_LEN) {
            return BIP324Cipher::LENGTH_LEN - m_recv_buffer.size();
        } else {
            return BIP324Cipher::EXPANSION + m_recv_len - m_recv_buffer.size();
        }
    case RecvState::APP_READY:
        return 0;
    case RecvState::V1:
        assert(false);
        return 0;
    }
    assert(false);
    return 0;
}

bool V2Transport::ReceivedBytes(std::span<const uint8_t>& msg_bytes) noexcept
{
    static constexpr size_t MAX_RESERVE_AHEAD = 256 * 1024;

    LOCK(m_recv_mutex);
    if (m_recv_state == RecvState::V1) return m_v1_fallback.ReceivedBytes(msg_bytes);

    while (!msg_bytes.empty()) {
        size_t max_read = GetMaxBytesToProcess();
        if (m_recv_buffer.size() + std::min(msg_bytes.size(), max_read) > m_recv_buffer.capacity()) {
            switch (m_recv_state) {
            case RecvState::KEY_MAYBE_V1:
            case RecvState::KEY:
            case RecvState::GARB_GARBTERM:
                m_recv_buffer.reserve(MAX_GARBAGE_LEN + BIP324Cipher::GARBAGE_TERMINATOR_LEN);
                break;
            case RecvState::VERSION:
            case RecvState::APP: {
                size_t alloc_add = std::min(max_read, msg_bytes.size() + MAX_RESERVE_AHEAD);
                m_recv_buffer.reserve(m_recv_buffer.size() + alloc_add);
                break;
            }
            case RecvState::APP_READY:
                assert(m_recv_buffer.empty());
                break;
            case RecvState::V1:
                assert(false);
                break;
            }
        }

        max_read = std::min(msg_bytes.size(), max_read);
        m_recv_buffer.insert(m_recv_buffer.end(), msg_bytes.data(), msg_bytes.data() + max_read);
        msg_bytes = msg_bytes.subspan(max_read);

        switch (m_recv_state) {
        case RecvState::KEY_MAYBE_V1:
            ProcessReceivedMaybeV1Bytes();
            if (m_recv_state == RecvState::V1) return true;
            break;
        case RecvState::KEY:
            if (!ProcessReceivedKeyBytes()) return false;
            break;
        case RecvState::GARB_GARBTERM:
            if (!ProcessReceivedGarbageBytes()) return false;
            break;
        case RecvState::VERSION:
        case RecvState::APP:
            if (!ProcessReceivedPacketBytes()) return false;
            break;
        case RecvState::APP_READY:
            return true;
        case RecvState::V1:
            assert(false);
            break;
        }
        assert(max_read > 0);
    }

    return true;
}

std::optional<std::string> V2Transport::GetMessageType(std::span<const uint8_t>& contents) noexcept
{
    if (contents.empty()) return std::nullopt;
    uint8_t first_byte = contents[0];
    contents = contents.subspan(1);

    if (first_byte != 0) {
        if (first_byte < std::size(V2_MESSAGE_IDS) && !V2_MESSAGE_IDS[first_byte].empty()) {
            return V2_MESSAGE_IDS[first_byte];
        } else {
            return std::nullopt;
        }
    }

    if (contents.size() < CMessageHeader::COMMAND_SIZE) {
        return std::nullopt;
    }

    size_t msg_type_len{0};
    while (msg_type_len < CMessageHeader::COMMAND_SIZE && contents[msg_type_len] != 0) {
        if (contents[msg_type_len] < ' ' || contents[msg_type_len] > 0x7E) {
            return std::nullopt;
        }
        ++msg_type_len;
    }
    std::string ret{reinterpret_cast<const char*>(contents.data()), msg_type_len};
    while (msg_type_len < CMessageHeader::COMMAND_SIZE) {
        if (contents[msg_type_len] != 0) return std::nullopt;
        ++msg_type_len;
    }
    contents = contents.subspan(CMessageHeader::COMMAND_SIZE);
    return ret;
}

CNetMessage V2Transport::GetReceivedMessage(int64_t time, bool& reject_message) noexcept
{
    LOCK(m_recv_mutex);
    if (m_recv_state == RecvState::V1) return m_v1_fallback.GetReceivedMessage(time, reject_message);

    assert(m_recv_state == RecvState::APP_READY);
    std::span<const uint8_t> contents{m_recv_decode_buffer};
    auto msg_type = GetMessageType(contents);
    CNetMessage msg{CDataStream{SER_NETWORK, INIT_PROTO_VERSION}};
    msg.m_raw_message_size = m_recv_decode_buffer.size() + BIP324Cipher::EXPANSION;
    if (msg_type) {
        reject_message = false;
        msg.m_type = std::move(*msg_type);
        msg.m_time = time;
        msg.m_message_size = contents.size();
        msg.m_recv.resize(contents.size());
        if (!contents.empty()) {
            std::copy(contents.begin(), contents.end(), UCharCast(msg.m_recv.data()));
        }
    } else {
        LogPrint(BCLog::NET, "V2 transport error: invalid message type (%zu bytes contents), peer=%d\n", m_recv_decode_buffer.size(), m_nodeid);
        reject_message = true;
    }
    ClearShrink(m_recv_decode_buffer);
    SetReceiveState(RecvState::APP);
    return msg;
}

bool V2Transport::SetMessageToSend(CSerializedNetMsg& msg) noexcept
{
    LOCK(m_send_mutex);
    if (m_send_state == SendState::V1) return m_v1_fallback.SetMessageToSend(msg);
    if (!(m_send_state == SendState::READY && m_send_buffer.empty())) return false;

    auto short_message_id = V2_MESSAGE_MAP(msg.command);
    const size_t header_bytes = short_message_id ? 1 : (1 + CMessageHeader::COMMAND_SIZE);
    const size_t contents_size = header_bytes + msg.data.size();
    if (contents_size > MAX_V2_CONTENTS_LEN) {
        LogPrint(BCLog::NET, "V2 transport error: message %s too large for BIP324 packet (%zu > %zu bytes), peer=%d\n",
                 SanitizeString(msg.command), contents_size, MAX_V2_CONTENTS_LEN, m_nodeid);
        return false;
    }

    std::vector<uint8_t> contents;
    if (short_message_id) {
        contents.resize(contents_size);
        contents[0] = *short_message_id;
        std::copy(msg.data.begin(), msg.data.end(), contents.begin() + 1);
    } else {
        contents.resize(contents_size, 0);
        std::copy(msg.command.begin(), msg.command.end(), contents.data() + 1);
        std::copy(msg.data.begin(), msg.data.end(), contents.begin() + 1 + CMessageHeader::COMMAND_SIZE);
    }
    m_send_buffer.resize(contents.size() + BIP324Cipher::EXPANSION);
    m_cipher.Encrypt(MakeByteSpan(contents), {}, false, MakeWritableByteSpan(m_send_buffer));
    m_send_type = msg.command;
    ClearShrink(msg.data);
    return true;
}

Transport::BytesToSend V2Transport::GetBytesToSend(bool have_next_message) const noexcept
{
    LOCK(m_send_mutex);
    if (m_send_state == SendState::V1) return m_v1_fallback.GetBytesToSend(have_next_message);

    if (m_send_state == SendState::MAYBE_V1) assert(m_send_buffer.empty());
    assert(m_send_pos <= m_send_buffer.size());
    return {
        std::span{m_send_buffer}.subspan(m_send_pos),
        have_next_message && m_send_state == SendState::READY,
        m_send_type
    };
}

void V2Transport::MarkBytesSent(size_t bytes_sent) noexcept
{
    LOCK(m_send_mutex);
    if (m_send_state == SendState::V1) return m_v1_fallback.MarkBytesSent(bytes_sent);

    if (m_send_state == SendState::AWAITING_KEY && m_send_pos == 0 && bytes_sent > 0) {
        LogPrint(BCLog::NET, "start sending v2 handshake to peer=%d\n", m_nodeid);
    }

    m_send_pos += bytes_sent;
    assert(m_send_pos <= m_send_buffer.size());
    if (m_send_pos >= CMessageHeader::HEADER_SIZE) {
        m_sent_v1_header_worth = true;
    }
    if (m_send_pos == m_send_buffer.size()) {
        m_send_pos = 0;
        ClearShrink(m_send_buffer);
    }
}

bool V2Transport::ShouldReconnectV1() const noexcept
{
    if (!m_initiating) return false;

    LOCK(m_recv_mutex);
    if (m_recv_state != RecvState::KEY) return false;
    if (!m_recv_buffer.empty()) return false;
    {
        LOCK(m_send_mutex);
        return m_sent_v1_header_worth;
    }
}

size_t V2Transport::GetSendMemoryUsage() const noexcept
{
    LOCK(m_send_mutex);
    if (m_send_state == SendState::V1) return m_v1_fallback.GetSendMemoryUsage();
    return m_send_buffer.size() + m_send_garbage.size();
}

bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes, bool& complete)
{
    complete = false;
    int64_t nTimeMicros = GetTimeMicros();
    LOCK(cs_vRecv);
    nLastRecv = nTimeMicros / 1000000;
    nRecvBytes += nBytes;
    std::span<const uint8_t> msg_bytes{UCharCast(pch), nBytes};
    while (!msg_bytes.empty()) {
        if (!m_transport->ReceivedBytes(msg_bytes)) {
            return false;
        }
        if (m_transport->ReceivedMessageComplete()) {
            bool reject_message{false};
            CNetMessage msg = m_transport->GetReceivedMessage(nTimeMicros, reject_message);
            if (reject_message) {
                mapMsgCmdSize::iterator i = mapRecvBytesPerMsgCmd.find(NET_MESSAGE_COMMAND_OTHER);
                assert(i != mapRecvBytesPerMsgCmd.end());
                i->second += msg.m_raw_message_size;
                continue;
            }
            // Store received bytes per message command; to prevent a memory DoS, only allow valid commands
            mapMsgCmdSize::iterator i = mapRecvBytesPerMsgCmd.find(msg.m_type);
            if (i == mapRecvBytesPerMsgCmd.end())
                i = mapRecvBytesPerMsgCmd.find(NET_MESSAGE_COMMAND_OTHER);
            assert(i != mapRecvBytesPerMsgCmd.end());
            i->second += msg.m_raw_message_size;

            vRecvMsg.push_back(std::move(msg));
            complete = true;
        }
    }

    return true;
}

void CNode::SetSendVersion(int nVersionIn)
{
    // Send version may only be changed in the version message, and
    // only one version message is allowed per session. We can therefore
    // treat this value as const and even atomic as long as it's only used
    // once a version message has been successfully processed. Any attempt to
    // set this twice is an error.
    if (nSendVersion != 0) {
        error("Send version already set for node: %i. Refusing to change from %i to %i", id, nSendVersion, nVersionIn);
    } else {
        nSendVersion = nVersionIn;
    }
}

int CNode::GetSendVersion() const
{
    // The send version should always be explicitly set to
    // INIT_PROTO_VERSION rather than using this value until SetSendVersion
    // has been called.
    if (nSendVersion == 0) {
        error("Requesting unset send version for node: %i. Using %i", id, INIT_PROTO_VERSION);
        return INIT_PROTO_VERSION;
    }
    return nSendVersion;
}


size_t CConnman::SocketSendData(CNode *pnode) const EXCLUSIVE_LOCKS_REQUIRED(pnode->cs_vSend)
{
    size_t nSentSize = 0;
    bool sent_something = false;

    while (true) {
        auto [data, more, msg_type] = pnode->m_transport->GetBytesToSend(!pnode->vSendMsg.empty());
        if (data.empty() && !pnode->vSendMsg.empty()) {
            size_t front_size = pnode->vSendMsg.front().data.size();
            if (pnode->m_transport->SetMessageToSend(pnode->vSendMsg.front())) {
                pnode->vSendMsg.pop_front();
                pnode->nSendSize -= front_size;
                continue;
            } else {
                // If v2 transport rejected an oversized message (> MAX_V2_CONTENTS_LEN), disconnect cleanly.
                if (front_size + 1 + CMessageHeader::COMMAND_SIZE > V2Transport::MAX_V2_CONTENTS_LEN &&
                    pnode->m_transport->GetInfo().transport_type == TransportProtocolType::V2) {
                    pnode->CloseSocketDisconnect();
                }
                break;
            }
        }
        if (data.empty()) break;

        int nBytes = 0;
        {
            LOCK(pnode->cs_hSocket);
            if (pnode->hSocket == INVALID_SOCKET)
                break;
            nBytes = send(pnode->hSocket, reinterpret_cast<const char*>(data.data()), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (nBytes > 0) {
            pnode->nLastSend = GetSystemTimeInSeconds();
            pnode->nSendBytes += nBytes;
            nSentSize += nBytes;
            sent_something = true;
            pnode->m_transport->MarkBytesSent(nBytes);
            pnode->fPauseSend = (pnode->nSendSize + pnode->m_transport->GetSendMemoryUsage()) > nSendBufferMaxSize;
            if (static_cast<size_t>(nBytes) != data.size()) {
                // could not send full buffer; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                    LogPrintf("socket send error %s\n", NetworkErrorString(nErr));
                    pnode->CloseSocketDisconnect();
                }
            }
            break;
        }
    }

    (void)sent_something;
    return nSentSize;
}

struct NodeEvictionCandidate
{
    NodeId id;
    int64_t nTimeConnected;
    int64_t nMinPingUsecTime;
    int64_t nLastBlockTime;
    int64_t nLastTXTime;
    bool fRelevantServices;
    bool fRelayTxes;
    bool fBloomFilter;
    CAddress addr;
    uint64_t nKeyedNetGroup;
    bool prefer_evict;
};

static bool ReverseCompareNodeMinPingTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.nMinPingUsecTime > b.nMinPingUsecTime;
}

static bool ReverseCompareNodeTimeConnected(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.nTimeConnected > b.nTimeConnected;
}

static bool CompareNetGroupKeyed(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b) {
    return a.nKeyedNetGroup < b.nKeyedNetGroup;
}

static bool CompareNodeBlockTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    // There is a fall-through here because it is common for a node to have many peers which have not yet relayed a block.
    if (a.nLastBlockTime != b.nLastBlockTime) return a.nLastBlockTime < b.nLastBlockTime;
    if (a.fRelevantServices != b.fRelevantServices) return b.fRelevantServices;
    return a.nTimeConnected > b.nTimeConnected;
}

static bool CompareNodeTXTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    // There is a fall-through here because it is common for a node to have more than a few peers that have not yet relayed txn.
    if (a.nLastTXTime != b.nLastTXTime) return a.nLastTXTime < b.nLastTXTime;
    if (a.fRelayTxes != b.fRelayTxes) return b.fRelayTxes;
    if (a.fBloomFilter != b.fBloomFilter) return a.fBloomFilter;
    return a.nTimeConnected > b.nTimeConnected;
}


//! Sort an array by the specified comparator, then erase the last K elements.
template<typename T, typename Comparator>
static void EraseLastKElements(std::vector<T> &elements, Comparator comparator, size_t k)
{
    std::sort(elements.begin(), elements.end(), comparator);
    size_t eraseSize = std::min(k, elements.size());
    elements.erase(elements.end() - eraseSize, elements.end());
}

/** Try to find a connection to evict when the node is full.
 *  Extreme care must be taken to avoid opening the node to attacker
 *   triggered network partitioning.
 *  The strategy used here is to protect a small number of peers
 *   for each of several distinct characteristics which are difficult
 *   to forge.  In order to partition a node the attacker must be
 *   simultaneously better at all of them than honest peers.
 */
bool CConnman::AttemptToEvictConnection()
{
    std::vector<NodeEvictionCandidate> vEvictionCandidates;
    {
        LOCK(cs_vNodes);

        for (const CNode* node : vNodes) {
            if (node->HasPermission(PF_NOBAN))
                continue;
            if (!node->fInbound)
                continue;
            if (node->fDisconnect)
                continue;
            LOCK(node->cs_filter);
            NodeEvictionCandidate candidate = {node->GetId(), node->nTimeConnected, node->nMinPingUsecTime,
                                               node->nLastBlockTime, node->nLastTXTime,
                                               HasAllDesirableServiceFlags(node->nServices),
                                               node->fRelayTxes, node->pfilter != nullptr, node->addr, node->nKeyedNetGroup,
                                               node->m_prefer_evict};
            vEvictionCandidates.push_back(candidate);
        }
    }

    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected
    EraseLastKElements(vEvictionCandidates, CompareNetGroupKeyed, 4);
    // Protect the 8 nodes with the lowest minimum ping time.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    EraseLastKElements(vEvictionCandidates, ReverseCompareNodeMinPingTime, 8);
    // Protect 4 nodes that most recently sent us transactions.
    // An attacker cannot manipulate this metric without performing useful work.
    EraseLastKElements(vEvictionCandidates, CompareNodeTXTime, 4);
    // Protect 4 nodes that most recently sent us blocks.
    // An attacker cannot manipulate this metric without performing useful work.
    EraseLastKElements(vEvictionCandidates, CompareNodeBlockTime, 4);
    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the non-eviction implicit behavior, and precludes attacks that start later.
    EraseLastKElements(vEvictionCandidates, ReverseCompareNodeTimeConnected, vEvictionCandidates.size() / 2);

    if (vEvictionCandidates.empty()) return false;

    // If any remaining peers are preferred for eviction consider only them.
    // This happens after the other preferences since if a peer is really the best by other criteria (esp relaying blocks)
    //  then we probably don't want to evict it no matter what.
    if (std::any_of(vEvictionCandidates.begin(),vEvictionCandidates.end(),[](NodeEvictionCandidate const &n){return n.prefer_evict;})) {
        vEvictionCandidates.erase(std::remove_if(vEvictionCandidates.begin(),vEvictionCandidates.end(),
                                  [](NodeEvictionCandidate const &n){return !n.prefer_evict;}),vEvictionCandidates.end());
    }

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    uint64_t naMostConnections;
    unsigned int nMostConnections = 0;
    int64_t nMostConnectionsTime = 0;
    std::map<uint64_t, std::vector<NodeEvictionCandidate> > mapNetGroupNodes;
    for (const NodeEvictionCandidate &node : vEvictionCandidates) {
        std::vector<NodeEvictionCandidate> &group = mapNetGroupNodes[node.nKeyedNetGroup];
        group.push_back(node);
        int64_t grouptime = group[0].nTimeConnected;

        if (group.size() > nMostConnections || (group.size() == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = group.size();
            nMostConnectionsTime = grouptime;
            naMostConnections = node.nKeyedNetGroup;
        }
    }

    // Reduce to the network group with the most connections
    vEvictionCandidates = std::move(mapNetGroupNodes[naMostConnections]);

    // Disconnect from the network group with the most connections
    NodeId evicted = vEvictionCandidates.front().id;
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (pnode->GetId() == evicted) {
            pnode->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::AcceptConnection(const ListenSocket& hListenSocket) {
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    SOCKET hSocket = accept(hListenSocket.socket, (struct sockaddr*)&sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    int nMaxInbound = nMaxConnections - (nMaxOutbound + nMaxFeeler);

    if (hSocket != INVALID_SOCKET) {
        if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr)) {
            LogPrintf("Warning: Unknown socket family\n");
        }
    }

    NetPermissionFlags permissionFlags = NetPermissionFlags::PF_NONE;
    hListenSocket.AddSocketPermissionFlags(permissionFlags);
    AddWhitelistPermissionFlags(permissionFlags, addr);
    bool legacyWhitelisted = false;
    if (NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::PF_ISIMPLICIT)) {
        NetPermissions::ClearFlag(permissionFlags, PF_ISIMPLICIT);
        if (gArgs.GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) NetPermissions::AddFlag(permissionFlags, PF_FORCERELAY);
        if (gArgs.GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)) NetPermissions::AddFlag(permissionFlags, PF_RELAY);
        NetPermissions::AddFlag(permissionFlags, PF_MEMPOOL);
        NetPermissions::AddFlag(permissionFlags, PF_NOBAN);
        legacyWhitelisted = true;
    }

    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (pnode->fInbound) nInbound++;
        }
    }

    if (hSocket == INVALID_SOCKET)
    {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            LogPrintf("socket error accept failed: %s\n", NetworkErrorString(nErr));
        return;
    }

    if (!fNetworkActive) {
        LogPrintf("connection from %s dropped: not accepting new connections\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    if (!IsSelectableSocket(hSocket))
    {
        LogPrintf("connection from %s dropped: non-selectable socket\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    SetSocketNoDelay(hSocket);

    int bannedlevel = m_banman ? m_banman->IsBannedLevel(addr) : 0;

    // Don't accept connections from banned peers, but if our inbound slots aren't almost full, accept
    // if the only banning reason was an automatic misbehavior ban.
    if (!NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::PF_NOBAN) && bannedlevel > ((nInbound + 1 < nMaxInbound) ? 1 : 0))
    {
        LogPrint(BCLog::NET, "connection from %s dropped (banned)\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    if (nInbound >= nMaxInbound)
    {
        if (!AttemptToEvictConnection()) {
            // No connection to evict, disconnect the new connection
            LogPrint(BCLog::NET, "failed to find an eviction candidate - connection dropped (full)\n");
            CloseSocket(hSocket);
            return;
        }
    }

    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
    CAddress addr_bind = GetBindAddress(hSocket);
    // Inbound connections use the bind port as part of the network key, so
    // that responses are cached per local listening socket.
    const uint64_t network_key = m_addr_response_cache.ComputeKey(addr, addr_bind, true);

    ServiceFlags nodeServices = nLocalServices;
    if (NetPermissions::HasFlag(permissionFlags, PF_BLOOMFILTER)) {
        nodeServices = static_cast<ServiceFlags>(nodeServices | NODE_BLOOM);
    }
    auto transport = CreateNodeTransport(id, /*inbound=*/true, m_use_v2transport);
    CNode* pnode = new CNode(id, nodeServices, GetBestHeight(), hSocket, addr, CalculateKeyedNetGroup(addr), nonce, addr_bind, network_key, "", true, std::move(transport));
    pnode->AddRef();
    pnode->m_permissionFlags = permissionFlags;
    // If this flag is present, the user probably expect that RPC and QT report it as whitelisted (backward compatibility)
    pnode->m_legacyWhitelisted = legacyWhitelisted;
    pnode->m_prefer_evict = bannedlevel > 0;
    m_msgproc->InitializeNode(pnode);

    LogPrint(BCLog::NET, "connection from %s accepted\n", addr.ToString());

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

/**
 * Create a node from an accepted I2P connection.
 *
 * The peer address is the I2P destination of the peer, the local address is
 * our own I2P destination. The TCP endpoint of the SAM bridge is never part
 * of the node information.
 */
void CConnman::AcceptI2PConnection(i2p::Connection& conn)
{
    const CService& peer = conn.peer;

    int nInbound = 0;
    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (pnode->fInbound) nInbound++;
        }
    }
    const int nMaxInbound = nMaxConnections - (nMaxOutbound + nMaxFeeler);

    if (!fNetworkActive) {
        LogPrint(BCLog::NET, "connection from %s dropped: not accepting new connections\n", peer.ToStringIP());
        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
        return;
    }

    if (!IsSelectableSocket(conn.sock)) {
        LogPrint(BCLog::NET, "connection from %s dropped: non-selectable socket\n", peer.ToStringIP());
        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
        return;
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    SetSocketNoDelay(conn.sock);

    // The ban list has no representation for the I2P destinations (they are
    // not addressable in the legacy format), so an I2P peer can not be banned.

    if (nInbound >= nMaxInbound) {
        if (!AttemptToEvictConnection()) {
            // No connection to evict, disconnect the new connection
            LogPrint(BCLog::NET, "failed to find an eviction candidate - connection dropped (full)\n");
            CloseSocket(conn.sock);
            conn.sock = INVALID_SOCKET;
            return;
        }
    }

    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
    const CAddress addr_bind = CAddress(conn.me, NODE_NONE);
    // Inbound connections use the bind address as part of the network key, so
    // that responses are cached per local listening socket.
    const uint64_t network_key = m_addr_response_cache.ComputeKey(peer, addr_bind, true);

    const CAddress addr_peer(peer, NODE_NONE);
    CNode* pnode = new CNode(id, nLocalServices, GetBestHeight(), conn.sock, addr_peer, CalculateKeyedNetGroup(addr_peer), nonce, addr_bind, network_key, "", true);
    pnode->AddRef();
    m_msgproc->InitializeNode(pnode);

    LogPrint(BCLog::NET, "connection from %s accepted\n", peer.ToStringIP());

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
    // The ownership of the socket was transferred to the node.
    conn.sock = INVALID_SOCKET;
}

/**
 * Accept the inbound I2P connections through the SAM bridge.
 */
void CConnman::ThreadI2PAcceptIncoming()
{
    LogPrint(BCLog::I2P, "Starting the I2P accept thread\n");

    i2p::Connection conn;

    while (!interruptNet) {
        if (!m_i2p_sam_session) break;

        if (!m_i2p_sam_session->Listen(conn)) {
            if (!interruptNet.sleep_for(std::chrono::seconds(5))) break;
            continue;
        }

        if (!m_i2p_sam_session->Accept(conn)) {
            // The accept socket is not used anymore.
            if (conn.sock != INVALID_SOCKET) {
                CloseSocket(conn.sock);
                conn.sock = INVALID_SOCKET;
            }
            if (!interruptNet.sleep_for(std::chrono::seconds(5))) break;
            continue;
        }

        AcceptI2PConnection(conn);
    }

    LogPrint(BCLog::I2P, "Stopping the I2P accept thread\n");
}

void CConnman::DisconnectNodes()
{
    {
        LOCK(cs_vNodes);

        if (!fNetworkActive) {
            // Disconnect any connected nodes
            for (CNode* pnode : vNodes) {
                if (!pnode->fDisconnect) {
                    LogPrint(BCLog::NET, "Network not active, dropping peer=%d\n", pnode->GetId());
                    pnode->fDisconnect = true;
                }
            }
        }

        // Disconnect unused nodes
        std::vector<CNode*> vNodesCopy = vNodes;
        for (CNode* pnode : vNodesCopy)
        {
            if (pnode->fDisconnect)
            {
                // remove from vNodes
                vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                // release outbound grant (if any)
                pnode->grantOutbound.Release();

                // close socket and cleanup
                pnode->CloseSocketDisconnect();

                if (pnode->m_transport->ShouldReconnectV1()) {
                    LogPrint(BCLog::NET, "retrying with v1 transport for peer=%d (%s)\n", pnode->GetId(), pnode->addr.ToString());
                    m_reconnections.push_back({pnode->addr, pnode->m_manual_connection});
                }

                // hold in disconnected pool until all refs are released
                pnode->Release();
                vNodesDisconnected.push_back(pnode);
            }
        }
    }
    {
        // Delete disconnected nodes
        std::list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
        for (CNode* pnode : vNodesDisconnectedCopy)
        {
            // wait until threads are done using it
            if (pnode->GetRefCount() <= 0) {
                bool fDelete = false;
                {
                    TRY_LOCK(pnode->cs_inventory, lockInv);
                    if (lockInv) {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend) {
                            fDelete = true;
                        }
                    }
                }
                if (fDelete) {
                    vNodesDisconnected.remove(pnode);
                    DeleteNode(pnode);
                }
            }
        }
    }
}

void CConnman::NotifyNumConnectionsChanged()
{
    size_t vNodesSize;
    {
        LOCK(cs_vNodes);
        vNodesSize = vNodes.size();
    }
    if(vNodesSize != nPrevNodeCount) {
        nPrevNodeCount = vNodesSize;
        if(clientInterface)
            clientInterface->NotifyNumConnectionsChanged(vNodesSize);
    }
}

void CConnman::InactivityCheck(CNode *pnode)
{
    int64_t nTime = GetSystemTimeInSeconds();
    if (nTime - pnode->nTimeConnected > m_peer_connect_timeout)
    {
        if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
        {
            LogPrint(BCLog::NET, "socket no message in first %i seconds, %d %d from %d\n", m_peer_connect_timeout, pnode->nLastRecv != 0, pnode->nLastSend != 0, pnode->GetId());
            pnode->fDisconnect = true;
        }
        else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL)
        {
            LogPrintf("socket sending timeout: %is\n", nTime - pnode->nLastSend);
            pnode->fDisconnect = true;
        }
        else if (nTime - pnode->nLastRecv > (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90*60))
        {
            LogPrintf("socket receive timeout: %is\n", nTime - pnode->nLastRecv);
            pnode->fDisconnect = true;
        }
        else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros())
        {
            LogPrintf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
            pnode->fDisconnect = true;
        }
        else if (!pnode->fSuccessfullyConnected)
        {
            LogPrint(BCLog::NET, "version handshake timeout from %d\n", pnode->GetId());
            pnode->fDisconnect = true;
        }
    }
}

bool CConnman::GenerateSelectSet(std::set<SOCKET> &recv_set, std::set<SOCKET> &send_set, std::set<SOCKET> &error_set)
{
    for (const ListenSocket& hListenSocket : vhListenSocket) {
        recv_set.insert(hListenSocket.socket);
    }

    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
        {
            // Implement the following logic:
            // * If there is data to send, select() for sending data. As this only
            //   happens when optimistic write failed, we choose to first drain the
            //   write buffer in this case before receiving more. This avoids
            //   needlessly queueing received data, if the remote peer is not themselves
            //   receiving data. This means properly utilizing TCP flow control signalling.
            // * Otherwise, if there is space left in the receive buffer, select() for
            //   receiving data.
            // * Hand off all complete messages to the processor, to be handled without
            //   blocking here.

            bool select_recv = !pnode->fPauseRecv;
            bool select_send;
            {
                LOCK(pnode->cs_vSend);
                const auto [to_send, more, _] = pnode->m_transport->GetBytesToSend(!pnode->vSendMsg.empty());
                select_send = !to_send.empty() || more;
            }

            LOCK(pnode->cs_hSocket);
            if (pnode->hSocket == INVALID_SOCKET)
                continue;

            error_set.insert(pnode->hSocket);
            if (select_send) {
                send_set.insert(pnode->hSocket);
                continue;
            }
            if (select_recv) {
                recv_set.insert(pnode->hSocket);
            }
        }
    }

    return !recv_set.empty() || !send_set.empty() || !error_set.empty();
}

#ifdef USE_POLL
void CConnman::SocketEvents(std::set<SOCKET> &recv_set, std::set<SOCKET> &send_set, std::set<SOCKET> &error_set)
{
    std::set<SOCKET> recv_select_set, send_select_set, error_select_set;
    if (!GenerateSelectSet(recv_select_set, send_select_set, error_select_set)) {
        interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS));
        return;
    }

    std::unordered_map<SOCKET, struct pollfd> pollfds;
    for (SOCKET socket_id : recv_select_set) {
        pollfds[socket_id].fd = socket_id;
        pollfds[socket_id].events |= POLLIN;
    }

    for (SOCKET socket_id : send_select_set) {
        pollfds[socket_id].fd = socket_id;
        pollfds[socket_id].events |= POLLOUT;
    }

    for (SOCKET socket_id : error_select_set) {
        pollfds[socket_id].fd = socket_id;
        // These flags are ignored, but we set them for clarity
        pollfds[socket_id].events |= POLLERR|POLLHUP;
    }

    std::vector<struct pollfd> vpollfds;
    vpollfds.reserve(pollfds.size());
    for (auto it : pollfds) {
        vpollfds.push_back(std::move(it.second));
    }

    if (poll(vpollfds.data(), vpollfds.size(), SELECT_TIMEOUT_MILLISECONDS) < 0) return;

    if (interruptNet) return;

    for (struct pollfd pollfd_entry : vpollfds) {
        if (pollfd_entry.revents & POLLIN)            recv_set.insert(pollfd_entry.fd);
        if (pollfd_entry.revents & POLLOUT)           send_set.insert(pollfd_entry.fd);
        if (pollfd_entry.revents & (POLLERR|POLLHUP)) error_set.insert(pollfd_entry.fd);
    }
}
#else
void CConnman::SocketEvents(std::set<SOCKET> &recv_set, std::set<SOCKET> &send_set, std::set<SOCKET> &error_set)
{
    std::set<SOCKET> recv_select_set, send_select_set, error_select_set;
    if (!GenerateSelectSet(recv_select_set, send_select_set, error_select_set)) {
        interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS));
        return;
    }

    //
    // Find which sockets have data to receive
    //
    struct timeval timeout;
    timeout.tv_sec  = 0;
    timeout.tv_usec = SELECT_TIMEOUT_MILLISECONDS * 1000; // frequency to poll pnode->vSend

    fd_set fdsetRecv;
    fd_set fdsetSend;
    fd_set fdsetError;
    FD_ZERO(&fdsetRecv);
    FD_ZERO(&fdsetSend);
    FD_ZERO(&fdsetError);
    SOCKET hSocketMax = 0;

    for (SOCKET hSocket : recv_select_set) {
        FD_SET(hSocket, &fdsetRecv);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    for (SOCKET hSocket : send_select_set) {
        FD_SET(hSocket, &fdsetSend);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    for (SOCKET hSocket : error_select_set) {
        FD_SET(hSocket, &fdsetError);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    int nSelect = select(hSocketMax + 1, &fdsetRecv, &fdsetSend, &fdsetError, &timeout);

    if (interruptNet)
        return;

    if (nSelect == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
        for (unsigned int i = 0; i <= hSocketMax; i++)
            FD_SET(i, &fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        if (!interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS)))
            return;
    }

    for (SOCKET hSocket : recv_select_set) {
        if (FD_ISSET(hSocket, &fdsetRecv)) {
            recv_set.insert(hSocket);
        }
    }

    for (SOCKET hSocket : send_select_set) {
        if (FD_ISSET(hSocket, &fdsetSend)) {
            send_set.insert(hSocket);
        }
    }

    for (SOCKET hSocket : error_select_set) {
        if (FD_ISSET(hSocket, &fdsetError)) {
            error_set.insert(hSocket);
        }
    }
}
#endif

void CConnman::SocketHandler()
{
    std::set<SOCKET> recv_set, send_set, error_set;
    SocketEvents(recv_set, send_set, error_set);

    if (interruptNet) return;

    //
    // Accept new connections
    //
    for (const ListenSocket& hListenSocket : vhListenSocket)
    {
        if (hListenSocket.socket != INVALID_SOCKET && recv_set.count(hListenSocket.socket) > 0)
        {
            AcceptConnection(hListenSocket);
        }
    }

    //
    // Service each socket
    //
    std::vector<CNode*> vNodesCopy;
    {
        LOCK(cs_vNodes);
        vNodesCopy = vNodes;
        for (CNode* pnode : vNodesCopy)
            pnode->AddRef();
    }
    for (CNode* pnode : vNodesCopy)
    {
        if (interruptNet)
            return;

        //
        // Receive
        //
        bool recvSet = false;
        bool sendSet = false;
        bool errorSet = false;
        {
            LOCK(pnode->cs_hSocket);
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            recvSet = recv_set.count(pnode->hSocket) > 0;
            sendSet = send_set.count(pnode->hSocket) > 0;
            errorSet = error_set.count(pnode->hSocket) > 0;
        }
        if (recvSet || errorSet)
        {
            // typical socket buffer is 8K-64K
            char pchBuf[0x10000];
            int nBytes = 0;
            {
                LOCK(pnode->cs_hSocket);
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
            }
            if (nBytes > 0)
            {
                bool notify = false;
                if (!pnode->ReceiveMsgBytes(pchBuf, nBytes, notify))
                    pnode->CloseSocketDisconnect();
                RecordBytesRecv(nBytes);
                if (notify) {
                    size_t nSizeAdded = 0;
                    for (const auto& msg : pnode->vRecvMsg) {
                        nSizeAdded += msg.m_raw_message_size;
                    }
                    {
                        LOCK(pnode->cs_vProcessMsg);
                        pnode->vProcessMsg.splice(pnode->vProcessMsg.end(), pnode->vRecvMsg);
                        pnode->nProcessQueueSize += nSizeAdded;
                        pnode->fPauseRecv = pnode->nProcessQueueSize > nReceiveFloodSize;
                    }
                    WakeMessageHandler();
                }
            }
            else if (nBytes == 0)
            {
                // socket closed gracefully
                if (!pnode->fDisconnect) {
                    LogPrint(BCLog::NET, "socket closed\n");
                }
                pnode->CloseSocketDisconnect();
            }
            else if (nBytes < 0)
            {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    if (!pnode->fDisconnect)
                        LogPrintf("socket recv error %s\n", NetworkErrorString(nErr));
                    pnode->CloseSocketDisconnect();
                }
            }
        }

        //
        // Send
        //
        if (sendSet)
        {
            LOCK(pnode->cs_vSend);
            size_t nBytes = SocketSendData(pnode);
            if (nBytes) {
                RecordBytesSent(nBytes);
            }
        }

        InactivityCheck(pnode);
    }
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodesCopy)
            pnode->Release();
    }
}

void CConnman::ThreadSocketHandler()
{
    while (!interruptNet)
    {
        DisconnectNodes();
        NotifyNumConnectionsChanged();
        SocketHandler();
    }
}

void CConnman::WakeMessageHandler()
{
    {
        std::lock_guard<std::mutex> lock(mutexMsgProc);
        fMsgProcWake = true;
    }
    condMsgProc.notify_one();
}






void CConnman::ThreadDNSAddressSeed()
{
    // goal: only query DNS seeds if address need is acute
    // Avoiding DNS seeds when we don't need them improves user privacy by
    //  creating fewer identifying DNS requests, reduces trust by giving seeds
    //  less influence on the network topology, and reduces traffic to the seeds.
    if ((addrman.size() > 0) &&
        (!gArgs.GetBoolArg("-forcednsseed", DEFAULT_FORCEDNSSEED))) {
        if (!interruptNet.sleep_for(std::chrono::seconds(11)))
            return;

        LOCK(cs_vNodes);
        int nRelevant = 0;
        for (const CNode* pnode : vNodes) {
            nRelevant += pnode->fSuccessfullyConnected && !pnode->fFeeler && !pnode->fOneShot && !pnode->m_manual_connection && !pnode->fInbound;
        }
        if (nRelevant >= 2) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const std::vector<std::string> &vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    for (const std::string &seed : vSeeds) {
        if (interruptNet) {
            return;
        }
        if (HaveNameProxy()) {
            AddOneShot(seed);
        } else {
            std::vector<CNetAddr> vIPs;
            std::vector<CAddress> vAdd;
            ServiceFlags requiredServiceBits = GetDesirableServiceFlags(NODE_NONE);
            std::string host = strprintf("x%x.%s", requiredServiceBits, seed);
            CNetAddr resolveSource;
            if (!resolveSource.SetInternal(host)) {
                continue;
            }
            unsigned int nMaxIPs = 256; // Limits number of IPs learned from a DNS seed
            if (LookupHost(host.c_str(), vIPs, nMaxIPs, true))
            {
                for (const CNetAddr& ip : vIPs)
                {
                    int nOneDay = 24*3600;
                    CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()), requiredServiceBits);
                    addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
                addrman.Add(vAdd, resolveSource);
            } else {
                // We now avoid directly using results from DNS Seeds which do not support service bit filtering,
                // instead using them as a oneshot to get nodes with our desired service bits.
                AddOneShot(seed);
            }
        }
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}












void CConnman::DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrint(BCLog::NET, "Flushed %d addresses to peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void CConnman::ProcessOneShot()
{
    std::string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        OpenNetworkConnection(addr, false, &grant, strDest.c_str(), true);
    }
}

bool CConnman::GetTryNewOutboundPeer()
{
    return m_try_another_outbound_peer;
}

void CConnman::SetTryNewOutboundPeer(bool flag)
{
    m_try_another_outbound_peer = flag;
    LogPrint(BCLog::NET, "net: setting try another outbound peer=%s\n", flag ? "true" : "false");
}

// Return the number of peers we have over our outbound connection limit
// Exclude peers that are marked for disconnect, or are going to be
// disconnected soon (eg one-shots and feelers)
// Also exclude peers that haven't finished initial connection handshake yet
// (so that we don't decide we're over our desired connection limit, and then
// evict some peer that has finished the handshake)
int CConnman::GetExtraOutboundCount()
{
    int nOutbound = 0;
    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (!pnode->fInbound && !pnode->m_manual_connection && !pnode->fFeeler && !pnode->fDisconnect && !pnode->fOneShot && pnode->fSuccessfullyConnected) {
                ++nOutbound;
            }
        }
    }
    return std::max(nOutbound - nMaxOutbound, 0);
}

void CConnman::ThreadOpenConnections(const std::vector<std::string> connect)
{
    // Connect to specific addresses
    if (!connect.empty())
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            for (const std::string& strAddr : connect)
            {
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, nullptr, strAddr.c_str(), false, false, true);
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                        return;
                }
            }
            if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                return;
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();

    // Minimum time before next feeler connection (in microseconds).
    int64_t nNextFeeler = PoissonNextSend(nStart*1000*1000, FEELER_INTERVAL);
    while (!interruptNet)
    {
        ProcessOneShot();

        if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
            return;

        CSemaphoreGrant grant(*semOutbound);
        if (interruptNet)
            return;

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                CNetAddr local;
                local.SetInternal("fixedseeds");
                addrman.Add(convertSeed6(Params().FixedSeeds()), local);
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        int nOutbound = 0;
        std::set<std::vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            for (const CNode* pnode : vNodes) {
                if (!pnode->fInbound && !pnode->m_manual_connection) {
                    // Netgroups for inbound and addnode peers are not excluded because our goal here
                    // is to not use multiple of our limited outbound slots on a single netgroup
                    // but inbound and addnode peers do not use our outbound slots.  Inbound peers
                    // also have the added issue that they're attacker controlled and could be used
                    // to prevent us from connecting to particular hosts if we used them here.
                    setConnected.insert(m_netgroupman->GetGroup(pnode->addr));
                    nOutbound++;
                }
            }
        }

        // Feeler Connections
        //
        // Design goals:
        //  * Increase the number of connectable addresses in the tried table.
        //
        // Method:
        //  * Choose a random address from new and attempt to connect to it if we can connect
        //    successfully it is added to tried.
        //  * Start attempting feeler connections only after node finishes making outbound
        //    connections.
        //  * Only make a feeler connection once every few minutes.
        //
        bool fFeeler = false;

        if (nOutbound >= nMaxOutbound && !GetTryNewOutboundPeer()) {
            int64_t nTime = GetTimeMicros(); // The current time right now (in microseconds).
            if (nTime > nNextFeeler) {
                nNextFeeler = PoissonNextSend(nTime, FEELER_INTERVAL);
                fFeeler = true;
            } else {
                continue;
            }
        }

        addrman.ResolveCollisions();

        int64_t nANow = GetAdjustedTime();
        int nTries = 0;
        while (!interruptNet)
        {
            CAddrInfo addr = addrman.SelectTriedCollision();

            // SelectTriedCollision returns an invalid address if it is empty.
            if (!fFeeler || !addr.IsValid()) {
                addr = addrman.Select(fFeeler);
            }

            // Require outbound connections, other than feelers, to be to distinct network groups
            if (!fFeeler && setConnected.count(m_netgroupman->GetGroup(addr))) {
                break;
            }

            // if we selected an invalid or local address, restart
            if (!addr.IsValid() || IsLocal(addr)) {
                break;
            }

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (!IsReachable(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // for non-feelers, require all the services we'll want,
            // for feelers, only require they be a full node (only because most
            // SPV clients don't have a good address DB available)
            if (!fFeeler && !HasAllDesirableServiceFlags(addr.nServices)) {
                continue;
            } else if (fFeeler && !MayHaveUsefulAddressDB(addr.nServices)) {
                continue;
            }

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid()) {

            if (fFeeler) {
                // Add small amount of random noise before connection to avoid synchronization.
                int randsleep = GetRandInt(FEELER_SLEEP_WINDOW * 1000);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(randsleep)))
                    return;
                LogPrint(BCLog::NET, "Making feeler connection to %s\n", addrConnect.ToString());
            }

            OpenNetworkConnection(addrConnect, (int)setConnected.size() >= std::min(nMaxConnections - 1, 2), &grant, nullptr, false, fFeeler);
        }
    }
}

std::vector<AddedNodeInfo> CConnman::GetAddedNodeInfo()
{
    std::vector<AddedNodeInfo> ret;

    std::list<std::string> lAddresses(0);
    {
        LOCK(cs_vAddedNodes);
        ret.reserve(vAddedNodes.size());
        std::copy(vAddedNodes.cbegin(), vAddedNodes.cend(), std::back_inserter(lAddresses));
    }


    // Build a map of all already connected addresses (by IP:port and by name) to inbound/outbound and resolved CService
    std::map<CService, bool> mapConnected;
    std::map<std::string, std::pair<bool, CService>> mapConnectedByName;
    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (pnode->addr.IsValid()) {
                mapConnected[pnode->addr] = pnode->fInbound;
            }
            std::string addrName = pnode->GetAddrName();
            if (!addrName.empty()) {
                mapConnectedByName[std::move(addrName)] = std::make_pair(pnode->fInbound, static_cast<const CService&>(pnode->addr));
            }
        }
    }

    for (const std::string& strAddNode : lAddresses) {
        CService service(LookupNumeric(strAddNode.c_str(), Params().GetDefaultPort()));
        AddedNodeInfo addedNode{strAddNode, CService(), false, false};
        if (service.IsValid()) {
            // strAddNode is an IP:port
            auto it = mapConnected.find(service);
            if (it != mapConnected.end()) {
                addedNode.resolvedAddress = service;
                addedNode.fConnected = true;
                addedNode.fInbound = it->second;
            }
        } else {
            // strAddNode is a name
            auto it = mapConnectedByName.find(strAddNode);
            if (it != mapConnectedByName.end()) {
                addedNode.resolvedAddress = it->second.second;
                addedNode.fConnected = true;
                addedNode.fInbound = it->second.first;
            }
        }
        ret.emplace_back(std::move(addedNode));
    }

    return ret;
}

void CConnman::ThreadOpenAddedConnections()
{
    while (true)
    {
        // Process any pending v2->v1 fallback reconnections.
        std::list<ReconnectionInfo> reconnections;
        {
            LOCK(cs_vNodes);
            reconnections.splice(reconnections.end(), m_reconnections);
        }
        for (const auto& info : reconnections) {
            OpenNetworkConnection(info.addr, false, nullptr, nullptr, false, false, info.manual_connection, /*use_v2transport=*/false);
        }

        CSemaphoreGrant grant(*semAddnode);
        std::vector<AddedNodeInfo> vInfo = GetAddedNodeInfo();
        bool tried = false;
        for (const AddedNodeInfo& info : vInfo) {
            if (!info.fConnected) {
                if (!grant.TryAcquire()) {
                    // If we've used up our semaphore and need a new one, let's not wait here since while we are waiting
                    // the addednodeinfo state might change.
                    break;
                }
                tried = true;
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, &grant, info.strAddedNode.c_str(), false, false, true);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                    return;
            }
        }
        // Retry every 60 seconds if a connection was attempted, otherwise two seconds
        if (!interruptNet.sleep_for(std::chrono::seconds(tried ? 60 : 2)))
            return;
    }
}

// if successful, this moves the passed grant to the constructed node
void CConnman::OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant *grantOutbound, const char *pszDest, bool fOneShot, bool fFeeler, bool manual_connection, std::optional<bool> use_v2transport)
{
    //
    // Initiate outbound network connection
    //
    if (interruptNet) {
        return;
    }
    if (!fNetworkActive) {
        return;
    }
    if (!pszDest) {
        if (IsLocal(addrConnect) ||
            FindNode(static_cast<CNetAddr>(addrConnect)) || (m_banman && m_banman->IsBanned(addrConnect)) ||
            FindNode(addrConnect.ToStringIPPort()))
            return;
    } else if (FindNode(std::string(pszDest)))
        return;

    CNode* pnode = ConnectNode(addrConnect, pszDest, fCountFailure, manual_connection, use_v2transport.value_or(m_use_v2transport));

    if (!pnode)
        return;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    if (fOneShot)
        pnode->fOneShot = true;
    if (fFeeler)
        pnode->fFeeler = true;
    if (manual_connection)
        pnode->m_manual_connection = true;

    m_msgproc->InitializeNode(pnode);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

void CConnman::ThreadMessageHandler()
{
    while (!flagInterruptMsgProc)
    {
        std::vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            for (CNode* pnode : vNodesCopy) {
                pnode->AddRef();
            }
        }

        bool fMoreWork = false;

        for (CNode* pnode : vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            bool fMoreNodeWork = m_msgproc->ProcessMessages(pnode, flagInterruptMsgProc);
            fMoreWork |= (fMoreNodeWork && !pnode->fPauseSend);
            if (flagInterruptMsgProc)
                return;
            // Send messages
            {
                LOCK(pnode->cs_sendProcessing);
                m_msgproc->SendMessages(pnode);
            }

            if (flagInterruptMsgProc)
                return;
        }

        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodesCopy)
                pnode->Release();
        }

        WAIT_LOCK(mutexMsgProc, lock);
        if (!fMoreWork) {
            condMsgProc.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(100), [this] { return fMsgProcWake; });
        }
        fMsgProcWake = false;
    }
}






bool CConnman::BindListenPort(const CService& addrBind, std::string& strError, NetPermissionFlags permissions, std::optional<uint16_t> &actualBoundPort)
{
    strError = "";
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    SOCKET hListenSocket = CreateSocket(addrBind);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (sockopt_arg_type)&nOne, sizeof(int));

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (sockopt_arg_type)&nOne, sizeof(int));
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. %s is probably already running.").translated, addrBind.ToString(), PACKAGE_NAME);
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)").translated, addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }

    // Retrieve and log the actual port being used
    actualBoundPort = GetAndPrintActualPort(hListenSocket, *actualBoundPort == 0, AutoPort::P2P, addrBind.ToStringIP());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)").translated, NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }

    vhListenSocket.push_back(ListenSocket(hListenSocket, permissions));

    if (addrBind.IsRoutable() && fDiscover && (permissions & PF_NOBAN) == 0)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void Discover()
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        std::vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr, 0, true))
        {
            for (const CNetAddr &addr : vaddr)
            {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#elif (HAVE_DECL_GETIFADDRS && HAVE_DECL_FREEIFADDRS)
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif
}

void CConnman::SetNetworkActive(bool active)
{
    LogPrint(BCLog::NET, "SetNetworkActive: %s\n", active);

    if (fNetworkActive == active) {
        return;
    }

    fNetworkActive = active;

    uiInterface.NotifyNetworkActiveChanged(fNetworkActive);
}

CConnman::CConnman(uint64_t nSeed0In, uint64_t nSeed1In, std::shared_ptr<const NetgroupManager> netgroupman)
    : m_netgroupman(netgroupman ? netgroupman : DefaultNetgroupManager()),
      addrman(m_netgroupman),
      nSeed0(nSeed0In),
      nSeed1(nSeed1In),
      m_addr_response_cache(nSeed0In, nSeed1In)
{
    SetTryNewOutboundPeer(false);

    Options connOptions;
    Init(connOptions);
}

NodeId CConnman::GetNewNodeId()
{
    return nLastNodeId.fetch_add(1, std::memory_order_relaxed);
}


bool CConnman::Bind(const CService &addr, unsigned int flags, NetPermissionFlags permissions, std::optional<uint16_t> &actualBoundPort) {
    if (!(flags & BF_EXPLICIT) && !IsReachable(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError, permissions, actualBoundPort)) {
        if ((flags & BF_REPORT_ERROR) && clientInterface) {
            clientInterface->ThreadSafeMessageBox(strError, "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }
    return true;
}

bool CConnman::InitBinds(const std::vector<CService>& binds, const std::vector<NetWhitebindPermissions>& whiteBinds)
{
    bool fBound = false;
    
    std::optional<uint16_t> actualBoundPort{};
    for (const auto& addrBind : binds) {
        fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR), NetPermissionFlags::PF_NONE, actualBoundPort);
    }
    for (const auto& addrBind : whiteBinds) {
        fBound |= Bind(addrBind.m_service, (BF_EXPLICIT | BF_REPORT_ERROR), addrBind.m_flags, actualBoundPort);
    }
    if (binds.empty() && whiteBinds.empty()) {
        struct in_addr inaddr_any;
        inaddr_any.s_addr = INADDR_ANY;
        struct in6_addr inaddr6_any = IN6ADDR_ANY_INIT;   
        unsigned short bind_port = GetListenPort();
        if (const auto autoPort = gArgs.GetArg("-ports", ""); autoPort == "auto") {
            bind_port = 0;
        }
        fBound |= Bind(CService(inaddr6_any, bind_port), BF_NONE, NetPermissionFlags::PF_NONE, actualBoundPort);
        if (actualBoundPort) {
            bind_port = *actualBoundPort;
        }
        fBound |= Bind(CService(inaddr_any, bind_port), !fBound ? BF_REPORT_ERROR : BF_NONE, NetPermissionFlags::PF_NONE, actualBoundPort);
    }
    return fBound;
}

bool CConnman::Start(CScheduler& scheduler, const Options& connOptions)
{
    Init(connOptions);

    {
        LOCK(cs_totalBytesRecv);
        nTotalBytesRecv = 0;
    }
    {
        LOCK(cs_totalBytesSent);
        nTotalBytesSent = 0;
        nMaxOutboundTotalBytesSentInCycle = 0;
        nMaxOutboundCycleStartTime = 0;
    }

    if (fListen && !InitBinds(connOptions.vBinds, connOptions.vWhiteBinds)) {
        if (clientInterface) {
            clientInterface->ThreadSafeMessageBox(
                _("Failed to listen on any port. Use -listen=0 if you want this.").translated,
                "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }

    for (const auto& strDest : connOptions.vSeedNodes) {
        AddOneShot(strDest);
    }

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading P2P addresses...").translated);
    }
    // Load addresses from peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (adb.Read(addrman))
            LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman.size(), GetTimeMillis() - nStart);
        else {
            addrman.Clear(); // Addrman can be in an inconsistent state after failure, reset it
            LogPrintf("Invalid or missing peers.dat; recreating\n");
            DumpAddresses();
        }
    }

    uiInterface.InitMessage(_("Starting network threads...").translated);

    fAddressesInitialized = true;

    if (semOutbound == nullptr) {
        // initialize semaphore
        semOutbound = std::make_unique<CSemaphore>(std::min((nMaxOutbound + nMaxFeeler), nMaxConnections));
    }
    if (semAddnode == nullptr) {
        // initialize semaphore
        semAddnode = std::make_unique<CSemaphore>(nMaxAddnode);
    }

    //
    // Start threads
    //
    assert(m_msgproc);
    InterruptSocks5(false);
    interruptNet.reset();
    flagInterruptMsgProc = false;

    // The I2P SAM session is created lazily (on the first I2P connection
    // attempt): an unreachable SAM bridge must not prevent the node from
    // starting.
    if (connOptions.m_i2p_sam.IsValid()) {
        m_i2p_sam_session = std::make_unique<i2p::sam::Session>(connOptions.m_i2p_private_key_path, connOptions.m_i2p_sam, &interruptNet);
        LogPrint(BCLog::I2P, "Using the I2P SAM bridge %s\n", connOptions.m_i2p_sam.ToStringIPPort());
    }

    {
        LOCK(mutexMsgProc);
        fMsgProcWake = false;
    }

    // Send and receive from sockets, accept connections
    threadSocketHandler = std::thread(&TraceThread<std::function<void()> >, "net", std::function<void()>(std::bind(&CConnman::ThreadSocketHandler, this)));

    // Accept the inbound I2P connections through the SAM bridge
    if (m_i2p_sam_session && connOptions.m_i2p_accept_incoming) {
        threadI2PAcceptIncoming = std::thread(&TraceThread<std::function<void()> >, "i2paccept", std::function<void()>(std::bind(&CConnman::ThreadI2PAcceptIncoming, this)));
    }

    if (!gArgs.GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadDNSAddressSeed = std::thread(&TraceThread<std::function<void()> >, "dnsseed", std::function<void()>(std::bind(&CConnman::ThreadDNSAddressSeed, this)));

    // Initiate outbound connections from -addnode
    threadOpenAddedConnections = std::thread(&TraceThread<std::function<void()> >, "addcon", std::function<void()>(std::bind(&CConnman::ThreadOpenAddedConnections, this)));

    if (connOptions.m_use_addrman_outgoing && !connOptions.m_specified_outgoing.empty()) {
        if (clientInterface) {
            clientInterface->ThreadSafeMessageBox(
                _("Cannot provide specific connections and have addrman find outgoing connections at the same.").translated,
                "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }
    if (connOptions.m_use_addrman_outgoing || !connOptions.m_specified_outgoing.empty())
        threadOpenConnections = std::thread(&TraceThread<std::function<void()> >, "opencon", std::function<void()>(std::bind(&CConnman::ThreadOpenConnections, this, connOptions.m_specified_outgoing)));

    // Process messages
    threadMessageHandler = std::thread(&TraceThread<std::function<void()> >, "msghand", std::function<void()>(std::bind(&CConnman::ThreadMessageHandler, this)));

    // Dump network addresses
    scheduler.scheduleEvery(std::bind(&CConnman::DumpAddresses, this), DUMP_PEERS_INTERVAL * 1000);

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup() {}

    ~CNetCleanup()
    {
#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
};
static CNetCleanup instance_of_cnetcleanup;

void CConnman::Interrupt()
{
    {
        std::lock_guard<std::mutex> lock(mutexMsgProc);
        flagInterruptMsgProc = true;
    }
    condMsgProc.notify_all();

    interruptNet();
    InterruptSocks5(true);

    if (semOutbound) {
        for (int i=0; i<(nMaxOutbound + nMaxFeeler); i++) {
            semOutbound->post();
        }
    }

    if (semAddnode) {
        for (int i=0; i<nMaxAddnode; i++) {
            semAddnode->post();
        }
    }
}

void CConnman::Stop()
{
    if (threadI2PAcceptIncoming.joinable())
        threadI2PAcceptIncoming.join();
    // The destructor of the session closes the control socket.
    m_i2p_sam_session.reset();

    if (threadMessageHandler.joinable())
        threadMessageHandler.join();
    if (threadOpenConnections.joinable())
        threadOpenConnections.join();
    if (threadOpenAddedConnections.joinable())
        threadOpenAddedConnections.join();
    if (threadDNSAddressSeed.joinable())
        threadDNSAddressSeed.join();
    if (threadSocketHandler.joinable())
        threadSocketHandler.join();

    if (fAddressesInitialized)
    {
        DumpAddresses();
        fAddressesInitialized = false;
    }

    // Close sockets
    for (CNode* pnode : vNodes)
        pnode->CloseSocketDisconnect();
    for (ListenSocket& hListenSocket : vhListenSocket)
        if (hListenSocket.socket != INVALID_SOCKET)
            if (!CloseSocket(hListenSocket.socket))
                LogPrintf("CloseSocket(hListenSocket) failed with error %s\n", NetworkErrorString(WSAGetLastError()));

    // clean up some globals (to help leak detection)
    for (CNode *pnode : vNodes) {
        DeleteNode(pnode);
    }
    for (CNode *pnode : vNodesDisconnected) {
        DeleteNode(pnode);
    }
    vNodes.clear();
    vNodesDisconnected.clear();
    vhListenSocket.clear();
    semOutbound.reset();
    semAddnode.reset();
}

void CConnman::DeleteNode(CNode* pnode)
{
    assert(pnode);
    bool fUpdateConnectionTime = false;
    m_msgproc->FinalizeNode(pnode->GetId(), fUpdateConnectionTime);
    if(fUpdateConnectionTime) {
        addrman.Connected(pnode->addr);
    }
    delete pnode;
}

CConnman::~CConnman()
{
    Interrupt();
    Stop();
}

size_t CConnman::GetAddressCount() const
{
    return addrman.size();
}

void CConnman::SetServices(const CService &addr, ServiceFlags nServices)
{
    addrman.SetServices(addr, nServices);
}

void CConnman::MarkAddressGood(const CAddress& addr)
{
    addrman.Good(addr);
}

void CConnman::AddNewAddresses(const std::vector<CAddress>& vAddr, const CAddress& addrFrom, int64_t nTimePenalty)
{
    addrman.Add(vAddr, addrFrom, nTimePenalty);
}

std::vector<CAddress> CConnman::GetAddressesUnsafe(std::optional<Network> network, size_t max_addresses, size_t max_pct)
{
    return addrman.GetAddr(network, max_addresses, max_pct);
}

std::vector<CAddress> CConnman::GetAddresses(CNode& requestor)
{
    return m_addr_response_cache.Get(requestor.m_network_key, [this]() {
        // The cached sample is capped at MAX_ADDR_TO_SEND, so that the random
        // replacement branch of CNode::PushAddress() is never triggered and the
        // response is identical for every requestor within the cache lifetime.
        return GetAddressesUnsafe(std::nullopt, MAX_ADDR_TO_SEND, ADDRMAN_GETADDR_MAX_PCT);
    });
}

std::map<enum Network, int64_t> CConnman::GetAddressCountsByNetwork() const
{
    return addrman.CountByNetwork();
}

bool CConnman::AddNode(const std::string& strNode)
{
    LOCK(cs_vAddedNodes);
    for (const std::string& it : vAddedNodes) {
        if (strNode == it) return false;
    }

    vAddedNodes.push_back(strNode);
    return true;
}

bool CConnman::RemoveAddedNode(const std::string& strNode)
{
    LOCK(cs_vAddedNodes);
    for(std::vector<std::string>::iterator it = vAddedNodes.begin(); it != vAddedNodes.end(); ++it) {
        if (strNode == *it) {
            vAddedNodes.erase(it);
            return true;
        }
    }
    return false;
}

size_t CConnman::GetNodeCount(NumConnections flags)
{
    LOCK(cs_vNodes);
    if (flags == CConnman::CONNECTIONS_ALL) // Shortcut if we want total
        return vNodes.size();

    int nNum = 0;
    for (const auto& pnode : vNodes) {
        if (flags & (pnode->fInbound ? CONNECTIONS_IN : CONNECTIONS_OUT)) {
            nNum++;
        }
    }

    return nNum;
}

void CConnman::GetNodeStats(std::vector<CNodeStats>& vstats)
{
    vstats.clear();
    LOCK(cs_vNodes);
    vstats.reserve(vNodes.size());
    for (CNode* pnode : vNodes) {
        vstats.emplace_back();
        pnode->copyStats(vstats.back());
    }
}

bool CConnman::DisconnectNode(const std::string& strNode)
{
    LOCK(cs_vNodes);
    if (CNode* pnode = FindNode(strNode)) {
        pnode->fDisconnect = true;
        return true;
    }
    return false;
}

bool CConnman::DisconnectNode(const CSubNet& subnet)
{
    bool disconnected = false;
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (subnet.Match(pnode->addr)) {
            pnode->fDisconnect = true;
            disconnected = true;
        }
    }
    return disconnected;
}

bool CConnman::DisconnectNode(const CNetAddr& addr)
{
    return DisconnectNode(CSubNet(addr));
}

bool CConnman::DisconnectNode(NodeId id)
{
    LOCK(cs_vNodes);
    for(CNode* pnode : vNodes) {
        if (id == pnode->GetId()) {
            pnode->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::RecordBytesRecv(uint64_t bytes)
{
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CConnman::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;

    uint64_t now = GetTime();
    if (nMaxOutboundCycleStartTime + nMaxOutboundTimeframe < now)
    {
        // timeframe expired, reset cycle
        nMaxOutboundCycleStartTime = now;
        nMaxOutboundTotalBytesSentInCycle = 0;
    }

    // TODO, exclude whitebind peers
    nMaxOutboundTotalBytesSentInCycle += bytes;
}

void CConnman::SetMaxOutboundTarget(uint64_t limit)
{
    LOCK(cs_totalBytesSent);
    nMaxOutboundLimit = limit;
}

uint64_t CConnman::GetMaxOutboundTarget()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundLimit;
}

uint64_t CConnman::GetMaxOutboundTimeframe()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundTimeframe;
}

uint64_t CConnman::GetMaxOutboundTimeLeftInCycle()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    if (nMaxOutboundCycleStartTime == 0)
        return nMaxOutboundTimeframe;

    uint64_t cycleEndTime = nMaxOutboundCycleStartTime + nMaxOutboundTimeframe;
    uint64_t now = GetTime();
    return (cycleEndTime < now) ? 0 : cycleEndTime - GetTime();
}

void CConnman::SetMaxOutboundTimeframe(uint64_t timeframe)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundTimeframe != timeframe)
    {
        // reset measure-cycle in case of changing
        // the timeframe
        nMaxOutboundCycleStartTime = GetTime();
    }
    nMaxOutboundTimeframe = timeframe;
}

bool CConnman::OutboundTargetReached(bool historicalBlockServingLimit)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return false;

    if (historicalBlockServingLimit)
    {
        // keep a large enough buffer to at least relay each block once
        uint64_t timeLeftInCycle = GetMaxOutboundTimeLeftInCycle();
        uint64_t buffer = timeLeftInCycle / 600 * MAX_BLOCK_SERIALIZED_SIZE;
        if (buffer >= nMaxOutboundLimit || nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit - buffer)
            return true;
    }
    else if (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit)
        return true;

    return false;
}

uint64_t CConnman::GetOutboundTargetBytesLeft()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    return (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit) ? 0 : nMaxOutboundLimit - nMaxOutboundTotalBytesSentInCycle;
}

uint64_t CConnman::GetTotalBytesRecv()
{
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CConnman::GetTotalBytesSent()
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

ServiceFlags CConnman::GetLocalServices() const
{
    return nLocalServices;
}

void CConnman::SetBestHeight(int height)
{
    nBestHeight.store(height, std::memory_order_release);
}

int CConnman::GetBestHeight() const
{
    return nBestHeight.load(std::memory_order_acquire);
}

unsigned int CConnman::GetReceiveFloodSize() const { return nReceiveFloodSize; }

CNode::CNode(NodeId idIn, ServiceFlags nLocalServicesIn, int nMyStartingHeightIn, SOCKET hSocketIn, const CAddress& addrIn, uint64_t nKeyedNetGroupIn, uint64_t nLocalHostNonceIn, const CAddress& addrBindIn, uint64_t networkKeyIn, const std::string& addrNameIn, bool fInboundIn, std::unique_ptr<Transport> transport)
    : m_transport(transport ? std::move(transport) : std::make_unique<V1Transport>(idIn)),
    nTimeConnected(GetSystemTimeInSeconds()),
    addr(addrIn),
    addrBind(addrBindIn),
    fInbound(fInboundIn),
    nKeyedNetGroup(nKeyedNetGroupIn),
    m_network_key(networkKeyIn),
    addrKnown(5000, 0.001),
    filterInventoryKnown(50000, 0.000001),
    id(idIn),
    nLocalHostNonce(nLocalHostNonceIn),
    nLocalServices(nLocalServicesIn),
    nMyStartingHeight(nMyStartingHeightIn)
{
    hSocket = hSocketIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    hashContinue = uint256();
    filterInventoryKnown.reset();
    pfilter = std::make_unique<CBloomFilter>();

    for (const std::string &msg : getAllNetMessageTypes())
        mapRecvBytesPerMsgCmd[msg] = 0;
    mapRecvBytesPerMsgCmd[NET_MESSAGE_COMMAND_OTHER] = 0;

    if (fLogIPs) {
        LogPrint(BCLog::NET, "Added connection to %s peer=%d\n", addrName, id);
    } else {
        LogPrint(BCLog::NET, "Added connection peer=%d\n", id);
    }

    nAddrTokenTimestamp = GetTimeMicros();
}

CNode::~CNode()
{
    CloseSocket(hSocket);
}

bool CConnman::NodeFullyConnected(const CNode* pnode)
{
    return pnode && pnode->fSuccessfullyConnected && !pnode->fDisconnect;
}

void CConnman::PushMessage(CNode* pnode, CSerializedNetMsg&& msg)
{
    size_t nMessageSize = msg.data.size();
    size_t nTotalSize = nMessageSize + CMessageHeader::HEADER_SIZE;
    LogPrint(BCLog::NET, "sending %s (%d bytes) peer=%d\n", SanitizeString(msg.command.c_str()), nMessageSize, pnode->GetId());

    size_t nBytesSent = 0;
    {
        LOCK(pnode->cs_vSend);
        const auto [to_send, more, _] = pnode->m_transport->GetBytesToSend(!pnode->vSendMsg.empty());
        bool optimisticSend = to_send.empty() && !more && pnode->vSendMsg.empty();

        // log total amount of bytes per command
        pnode->mapSendBytesPerMsgCmd[msg.command] += nTotalSize;
        pnode->nSendSize += nMessageSize;
        pnode->vSendMsg.push_back(std::move(msg));

        if (pnode->nSendSize + pnode->m_transport->GetSendMemoryUsage() > nSendBufferMaxSize)
            pnode->fPauseSend = true;

        // If write queue empty, attempt "optimistic write"
        if (optimisticSend)
            nBytesSent = SocketSendData(pnode);
    }
    if (nBytesSent)
        RecordBytesSent(nBytesSent);
}

bool CConnman::ForNode(NodeId id, std::function<bool(CNode* pnode)> func)
{
    CNode* found = nullptr;
    LOCK(cs_vNodes);
    for (auto&& pnode : vNodes) {
        if(pnode->GetId() == id) {
            found = pnode;
            break;
        }
    }
    return found != nullptr && NodeFullyConnected(found) && func(found);
}

int64_t CConnman::PoissonNextSendInbound(int64_t now, int average_interval_seconds)
{
    if (m_next_send_inv_to_incoming < now) {
        // If this function were called from multiple threads simultaneously
        // it would possible that both update the next send variable, and return a different result to their caller.
        // This is not possible in practice as only the net processing thread invokes this function.
        m_next_send_inv_to_incoming = PoissonNextSend(now, average_interval_seconds);
    }
    return m_next_send_inv_to_incoming;
}

int64_t PoissonNextSend(int64_t now, int average_interval_seconds)
{
    return now + (int64_t)(log1p(GetRand(1ULL << 48) * -0.0000000000000035527136788 /* -1/2^48 */) * average_interval_seconds * -1000000.0 + 0.5);
}

CSipHasher CConnman::GetDeterministicRandomizer(uint64_t id) const
{
    return CSipHasher(nSeed0, nSeed1).Write(id);
}

uint64_t CConnman::CalculateKeyedNetGroup(const CAddress& ad) const
{
    std::vector<unsigned char> vchNetGroup(m_netgroupman->GetGroup(ad));

    return GetDeterministicRandomizer(RANDOMIZER_ID_NETGROUP).Write(vchNetGroup.data(), vchNetGroup.size()).Finalize();
}
