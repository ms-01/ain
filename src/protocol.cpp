// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <protocol.h>

#include <util/system.h>
#include <util/strencodings.h>

#include <algorithm>
#include <vector>

#ifndef WIN32
# include <arpa/inet.h>
#endif

static std::atomic<bool> g_initial_block_download_completed(false);

namespace NetMsgType {
const char *VERSION="version";
const char *VERACK="verack";
const char *ADDR="addr";
const char *ADDRV2="addrv2";
const char *SENDADDRV2="sendaddrv2";
const char *INV="inv";
const char *GETDATA="getdata";
const char *MERKLEBLOCK="merkleblock";
const char *GETBLOCKS="getblocks";
const char *GETHEADERS="getheaders";
const char *GETANCHORAUTHS="getauths";
const char *TX="tx";
const char *HEADERS="headers";
const char *BLOCK="block";
const char *GETADDR="getaddr";
const char *MEMPOOL="mempool";
const char *PING="ping";
const char *PONG="pong";
const char *NOTFOUND="notfound";
const char *FILTERLOAD="filterload";
const char *FILTERADD="filteradd";
const char *FILTERCLEAR="filterclear";
const char *SENDHEADERS="sendheaders";
const char *FEEFILTER="feefilter";
const char *SENDCMPCT="sendcmpct";
const char *CMPCTBLOCK="cmpctblock";
const char *GETBLOCKTXN="getblocktxn";
const char *BLOCKTXN="blocktxn";
const char *ANCHORAUTH="anchorauth";
const char *ANCHORCONFIRM="anchorconf"; // Renamed from anchorcnf for Dakota fork, new message type. Old type ignored.
const char *WTXIDRELAY="wtxidrelay";
} // namespace NetMsgType

/** All known message types. Keep this in the same order as the list of
 * messages above and in protocol.h.
 */
const static std::string allNetMessageTypes[] = {
    NetMsgType::VERSION,
    NetMsgType::VERACK,
    NetMsgType::ADDR,
    NetMsgType::ADDRV2,
    NetMsgType::SENDADDRV2,
    NetMsgType::INV,
    NetMsgType::GETDATA,
    NetMsgType::MERKLEBLOCK,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETHEADERS,
    NetMsgType::GETANCHORAUTHS,
    NetMsgType::TX,
    NetMsgType::HEADERS,
    NetMsgType::BLOCK,
    NetMsgType::GETADDR,
    NetMsgType::MEMPOOL,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::NOTFOUND,
    NetMsgType::FILTERLOAD,
    NetMsgType::FILTERADD,
    NetMsgType::FILTERCLEAR,
    NetMsgType::SENDHEADERS,
    NetMsgType::FEEFILTER,
    NetMsgType::SENDCMPCT,
    NetMsgType::CMPCTBLOCK,
    NetMsgType::GETBLOCKTXN,
    NetMsgType::BLOCKTXN,
    NetMsgType::ANCHORAUTH,
    NetMsgType::ANCHORCONFIRM,
    NetMsgType::WTXIDRELAY,
};
const static std::vector<std::string> allNetMessageTypesVec(allNetMessageTypes, allNetMessageTypes+ARRAYLEN(allNetMessageTypes));

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn)
{
    memcpy(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE);
    memset(pchCommand, 0, sizeof(pchCommand));
    nMessageSize = -1;
    memset(pchChecksum, 0, CHECKSUM_SIZE);
}

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn)
{
    memcpy(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE);
    memset(pchCommand, 0, sizeof(pchCommand));
    strncpy(pchCommand, pszCommand, COMMAND_SIZE);
    nMessageSize = nMessageSizeIn;
    memset(pchChecksum, 0, CHECKSUM_SIZE);
}

std::string CMessageHeader::GetCommand() const
{
    return std::string(pchCommand, pchCommand + strnlen(pchCommand, COMMAND_SIZE));
}

bool CMessageHeader::IsValid(const MessageStartChars& pchMessageStartIn) const
{
    // Check start string
    if (memcmp(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE) != 0)
        return false;

    // Check the command string for errors
    for (const char* p1 = pchCommand; p1 < pchCommand + COMMAND_SIZE; p1++)
    {
        if (*p1 == 0)
        {
            // Must be all zeros after the first zero
            for (; p1 < pchCommand + COMMAND_SIZE; p1++)
                if (*p1 != 0)
                    return false;
        }
        else if (*p1 < ' ' || *p1 > 0x7E)
            return false;
    }

    // Message size
    if (nMessageSize > MAX_DESER_SIZE)
    {
        LogPrintf("CMessageHeader::IsValid(): (%s, %u bytes) nMessageSize > MAX_SIZE\n", GetCommand(), nMessageSize);
        return false;
    }

    return true;
}


ServiceFlags GetDesirableServiceFlags(ServiceFlags services) {
    if ((services & NODE_NETWORK_LIMITED) && g_initial_block_download_completed) {
        return ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS);
    }
    return ServiceFlags(NODE_NETWORK | NODE_WITNESS);
}

void SetServiceFlagsIBDCache(bool state) {
    g_initial_block_download_completed = state;
}


CAddress::CAddress() : CService()
{
    Init();
}

CAddress::CAddress(CService ipIn, ServiceFlags nServicesIn) : CService(ipIn)
{
    Init();
    nServices = nServicesIn;
}

void CAddress::Init()
{
    nServices = NODE_NONE;
    nTime = 100000000;
}

CInv::CInv()
{
    type = 0;
    hash.SetNull();
}

CInv::CInv(int typeIn, const uint256& hashIn) : type(typeIn), hash(hashIn) {}

bool operator<(const CInv& a, const CInv& b)
{
    return (a.type < b.type || (a.type == b.type && a.hash < b.hash));
}

std::string CInv::GetCommand() const
{
    std::string cmd;
    if (type & MSG_WITNESS_FLAG)
        cmd.append("witness-");
    int masked = type & MSG_TYPE_MASK;
    switch (masked)
    {
    case MSG_TX:             return cmd.append(NetMsgType::TX);
    case MSG_BLOCK:          return cmd.append(NetMsgType::BLOCK);
    case MSG_FILTERED_BLOCK: return cmd.append(NetMsgType::MERKLEBLOCK);
    case MSG_CMPCT_BLOCK:    return cmd.append(NetMsgType::CMPCTBLOCK);
    case MSG_ANCHOR_AUTH:    return cmd.append(NetMsgType::ANCHORAUTH);
    case MSG_ANCHOR_CONFIRM: return cmd.append(NetMsgType::ANCHORCONFIRM);
    case MSG_WTX:            return cmd.append("wtx");
    default:
        throw std::out_of_range(strprintf("CInv::GetCommand(): type=%d unknown type", type));
    }
}

std::string CInv::ToString() const
{
    try {
        return strprintf("%s %s", GetCommand(), hash.ToString());
    } catch(const std::out_of_range &) {
        return strprintf("0x%08x %s", type, hash.ToString());
    }
}

const std::vector<std::string> &getAllNetMessageTypes()
{
    return allNetMessageTypesVec;
}

/**
 * The result of the decoding of a single entry of an "addrv2" message.
 */
enum class AddrV2EntryResult {
    /** The entry was decoded successfully. */
    OK,
    /** The entry is invalid, but it was skipped completely. The following
     *  entries of the message can still be decoded. */
    SKIPPED,
    /** The entry cannot be skipped completely, so the stream is out of sync
     *  and the decoding of the message has to be aborted. */
    INVALID,
};

/**
 * Read a single address entry of an "addrv2" message.
 *
 * The layout of an entry is:
 *   nTime (4 bytes) || nServices (CompactSize) || netid (1 byte) ||
 *   COMPACTSIZE(length) || address bytes || port (2 bytes, big endian)
 *
 * In contrast to upstream Bitcoin Core, which throws when a known network is
 * announced with an unexpected address size, an invalid entry is only skipped
 * here: the remaining entries of the message are still processed and the peer
 * is never disconnected for a malformed address.
 *
 * An entry that cannot be skipped completely (because the announced length
 * exceeds the remaining bytes of the message) leaves the stream out of sync.
 * In that case the decoding of the message has to be aborted, otherwise the
 * following entries would be parsed from a shifted position.
 */
static AddrV2EntryResult UnserializeAddrV2Entry(CDataStream& s, CAddress& addrOut)
{
    uint32_t nTime{0};
    s >> nTime;
    const uint64_t nServices = ReadCompactSize(s);
    uint8_t netid{0};
    s >> netid;
    const uint64_t nLen = ReadCompactSize(s);
    // The entry consists of the address bytes and the two port bytes. If the
    // announced length does not fit into the remaining bytes of the message,
    // the entry cannot be skipped completely and the stream is out of sync.
    if (nLen > s.size() || s.size() - nLen < 2) {
        return AddrV2EntryResult::INVALID;
    }
    if (nLen > MAX_ADDRV2_SIZE) {
        // The address is too long to be a valid address. The complete entry
        // (the address bytes *and* the port bytes) is skipped, so that the
        // remaining entries can still be processed.
        s.ignore(static_cast<int>(nLen));
        s.ignore(2);
        return AddrV2EntryResult::SKIPPED;
    }
    std::vector<unsigned char> vBytes(nLen);
    if (nLen > 0) {
        s.read(reinterpret_cast<char*>(vBytes.data()), nLen);
    }
    uint8_t pchPort[2];
    s.read(reinterpret_cast<char*>(pchPort), sizeof(pchPort));
    const uint16_t nPort = (static_cast<uint16_t>(pchPort[0]) << 8) | pchPort[1];

    CNetAddr netAddr;
    if (!netAddr.SetFromBIP155(netid, vBytes.data(), vBytes.size())) {
        // An unknown network id or an unexpected address size: the entry is
        // ignored, but it was read completely, so the stream is still in sync.
        return AddrV2EntryResult::SKIPPED;
    }
    CAddress addr{CService(netAddr, nPort), static_cast<ServiceFlags>(nServices)};
    addr.nTime = nTime;
    addrOut = addr;
    return AddrV2EntryResult::OK;
}

uint64_t DecodeAddrV2Message(CDataStream& s, std::vector<CAddress>& vAddrOut)
{
    vAddrOut.clear();
    uint64_t nCount{0};
    try {
        nCount = ReadCompactSize(s);
        // A peer that announces more entries than allowed is misbehaving
        // anyway, so only a limited number of entries is decoded.
        const uint64_t nMax = std::min<uint64_t>(nCount, MAX_ADDRV2_ENTRIES + 1);
        for (uint64_t i = 0; i < nMax; ++i) {
            CAddress addr;
            const AddrV2EntryResult result = UnserializeAddrV2Entry(s, addr);
            if (result == AddrV2EntryResult::INVALID) {
                // The entry cannot be skipped completely, so the remaining
                // bytes of the message cannot be decoded reliably. The
                // entries that were decoded so far are kept.
                LogPrint(BCLog::NET, "Aborting the decoding of an addrv2 message: out of sync\n");
                break;
            }
            if (result == AddrV2EntryResult::SKIPPED) {
                continue;
            }
            vAddrOut.push_back(addr);
        }
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "Failed to decode an addrv2 message: %s\n", e.what());
    }
    return nCount;
}

GenTxid ToGenTxid(const CInv& inv)
{
    assert(inv.IsGenTxMsg());
    return inv.IsMsgWtx() ? GenTxid::Wtxid(inv.hash) : GenTxid::Txid(inv.hash);
}
