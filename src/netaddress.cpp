// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <netaddress.h>
#include <crypto/sha3.h>
#include <hash.h>
#include <util/strencodings.h>
#include <tinyformat.h>

#include <cstring>

static const unsigned char pchIPv4[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
static const unsigned char pchOnionCat[] = {0xFD,0x87,0xD8,0x7E,0xEB,0x43};

// 0xFD + sha256("defi")[0:5]
static const unsigned char g_internal_prefix[] = { 0xFD, 0x6B, 0x88, 0xC0, 0x87, 0x24 };

namespace torv3 {

/**
 * Helpers for the Tor v3 onion service address format.
 *
 * See https://spec.torproject.org/rend-spec-v3.html for the specification.
 */
static constexpr size_t CHECKSUM_LEN = 2;
static const unsigned char VERSION[] = {3};
static constexpr size_t TOTAL_LEN = ADDR_TORV3_SIZE + CHECKSUM_LEN + sizeof(VERSION);

/**
 * Compute the Tor v3 address checksum.
 *
 * CHECKSUM = H(".onion checksum" | PUBKEY | VERSION)[:2]
 */
static void Checksum(const uint8_t* pubkey, uint8_t (&checksum)[CHECKSUM_LEN])
{
    static const unsigned char prefix[] = ".onion checksum";

    uint8_t buf[sizeof(prefix) - 1 + ADDR_TORV3_SIZE + sizeof(VERSION)];
    memcpy(buf, prefix, sizeof(prefix) - 1);
    memcpy(buf + sizeof(prefix) - 1, pubkey, ADDR_TORV3_SIZE);
    memcpy(buf + sizeof(prefix) - 1 + ADDR_TORV3_SIZE, VERSION, sizeof(VERSION));

    uint8_t full[32];
    sha3_256_fips202(full, sizeof(full), buf, sizeof(buf));
    memcpy(checksum, full, CHECKSUM_LEN);
}

} // namespace torv3

/**
 * Construct an unspecified IPv6 network address (::/128).
 *
 * @note This address is considered invalid by CNetAddr::IsValid()
 */
CNetAddr::CNetAddr()
{
}

void CNetAddr::SetIP(const CNetAddr& ipIn)
{
    m_net = ipIn.m_net;
    m_addr = ipIn.m_addr;
    scopeId = ipIn.scopeId;
}

void CNetAddr::SetRaw(Network network, const uint8_t *ip_in)
{
    switch(network)
    {
        case NET_IPV4:
            m_net = NET_IPV4;
            m_addr.assign(ADDR_IPV6_SIZE, 0x0);
            memcpy(m_addr.data(), pchIPv4, 12);
            memcpy(m_addr.data() + 12, ip_in, 4);
            break;
        case NET_IPV6: {
            uint8_t buf[ADDR_IPV6_SIZE];
            memcpy(buf, ip_in, ADDR_IPV6_SIZE);
            SetLegacyIPv6(buf);
            break;
        }
        default:
            assert(!"invalid network");
    }
}

/**
 * Set this object from the 16 byte legacy (pre-BIP155) representation.
 *
 * The network is derived from the well known prefixes:
 * - `::ffff:0:0/96`  -> IPv4 (kept in the IPv4-mapped IPv6 form)
 * - `fd87:d87e:eb43::/48` -> legacy Tor v2 (OnionCat)
 * - `fd6b:88c0:8724::/48` -> internal
 * - everything else -> IPv6
 */
void CNetAddr::SetLegacyIPv6(const uint8_t (&ip)[ADDR_IPV6_SIZE])
{
    m_addr.assign(ip, ip + ADDR_IPV6_SIZE);
    if (memcmp(ip, pchIPv4, sizeof(pchIPv4)) == 0) {
        m_net = NET_IPV4;
    } else if (memcmp(ip, pchOnionCat, sizeof(pchOnionCat)) == 0) {
        m_net = NET_ONION;
    } else if (memcmp(ip, g_internal_prefix, sizeof(g_internal_prefix)) == 0) {
        m_net = NET_INTERNAL;
    } else {
        m_net = NET_IPV6;
    }
}

/**
 * Try to make this a dummy address that maps the specified name into IPv6 like
 * so: (0xFD + %sha256("defi")[0:5]) + %sha256(name)[0:10]. Such dummy
 * addresses have a prefix of fd6b:88c0:8724::/48 and are guaranteed to not be
 * publicly routable as it falls under RFC4193's fc00::/7 subnet allocated to
 * unique-local addresses.
 *
 * CAddrMan uses these fake addresses to keep track of which DNS seeds were
 * used.
 *
 * @returns Whether or not the operation was successful.
 *
 * @see CNetAddr::IsInternal(), CNetAddr::IsRFC4193()
 */
bool CNetAddr::SetInternal(const std::string &name)
{
    if (name.empty()) {
        return false;
    }
    unsigned char hash[32] = {};
    CSHA256().Write((const unsigned char*)name.data(), name.size()).Finalize(hash);
    m_net = NET_INTERNAL;
    m_addr.assign(ADDR_IPV6_SIZE, 0x0);
    memcpy(m_addr.data(), g_internal_prefix, sizeof(g_internal_prefix));
    memcpy(m_addr.data() + sizeof(g_internal_prefix), hash, m_addr.size() - sizeof(g_internal_prefix));
    return true;
}

/**
 * Try to parse a privacy network address.
 *
 * Supported formats:
 * - Tor v3 onion addresses: 56 character base32 encoded
 *   `PUBKEY | CHECKSUM | VERSION` followed by ".onion",
 * - legacy Tor v2 onion addresses: 16 character base32 encoded onion id
 *   followed by ".onion" (mapped into the OnionCat IPv6 range, deprecated),
 * - I2P destinations: 52 character base32 encoded destination followed by
 *   ".b32.i2p".
 *
 * @returns Whether or not the operation was successful.
 *
 * @see CNetAddr::IsTor(), CNetAddr::IsTorV3(), CNetAddr::IsI2P()
 */
bool CNetAddr::SetSpecial(const std::string &strName)
{
    if (strName.size() > 6 && strName.substr(strName.size() - 6, 6) == ".onion") {
        bool invalid = false;
        std::vector<unsigned char> vchAddr = DecodeBase32(strName.substr(0, strName.size() - 6).c_str(), &invalid);
        if (invalid) {
            return false;
        }
        if (vchAddr.size() == 16 - sizeof(pchOnionCat)) {
            // Legacy Tor v2 (OnionCat) address. Kept for backwards compatibility,
            // Tor itself dropped v2 services in 0.4.6.
            m_net = NET_ONION;
            m_addr.assign(ADDR_IPV6_SIZE, 0x0);
            memcpy(m_addr.data(), pchOnionCat, sizeof(pchOnionCat));
            memcpy(m_addr.data() + sizeof(pchOnionCat), vchAddr.data(), vchAddr.size());
            return true;
        }
        if (vchAddr.size() == torv3::TOTAL_LEN) {
            const unsigned char* pubkey = vchAddr.data();
            const unsigned char* checksum = vchAddr.data() + ADDR_TORV3_SIZE;
            const unsigned char* version = vchAddr.data() + ADDR_TORV3_SIZE + torv3::CHECKSUM_LEN;

            if (version[0] != torv3::VERSION[0]) {
                return false;
            }
            uint8_t calculated[torv3::CHECKSUM_LEN];
            torv3::Checksum(pubkey, calculated);
            if (memcmp(checksum, calculated, sizeof(calculated)) != 0) {
                return false;
            }
            m_net = NET_ONION;
            m_addr.assign(pubkey, pubkey + ADDR_TORV3_SIZE);
            return true;
        }
        return false;
    }
    if (strName.size() > 8 && strName.substr(strName.size() - 8, 8) == ".b32.i2p") {
        // I2P destinations consist of 52 base32 characters plus the ".b32.i2p"
        // suffix.
        const std::string b32 = strName.substr(0, strName.size() - 8);
        if (b32.size() != 52) {
            return false;
        }
        // Pad the base32 string to a multiple of 8 characters so that it can be
        // decoded.
        bool invalid = false;
        std::vector<unsigned char> vchAddr = DecodeBase32((b32 + "====").c_str(), &invalid);
        if (invalid || vchAddr.size() != ADDR_I2P_SIZE) {
            return false;
        }
        m_net = NET_I2P;
        m_addr.assign(vchAddr.begin(), vchAddr.end());
        return true;
    }
    return false;
}

CNetAddr::CNetAddr(const struct in_addr& ipv4Addr)
{
    SetRaw(NET_IPV4, (const uint8_t*)&ipv4Addr);
}

CNetAddr::CNetAddr(const struct in6_addr& ipv6Addr, const uint32_t scope)
{
    SetRaw(NET_IPV6, (const uint8_t*)&ipv6Addr);
    scopeId = scope;
}

unsigned int CNetAddr::GetByte(int n) const
{
    if (n < 0 || static_cast<size_t>(n) >= m_addr.size()) {
        return 0;
    }
    return m_addr[m_addr.size() - 1 - n];
}

bool CNetAddr::IsBindAny() const
{
    if (IsIPv4()) {
        // IPv4 addresses are stored in the IPv4-mapped IPv6 form, only the last
        // four bytes belong to the address itself.
        return m_addr[12] == 0 && m_addr[13] == 0 && m_addr[14] == 0 && m_addr[15] == 0;
    }
    if (!IsIPv6()) {
        return false;
    }
    for (size_t i = 0; i < m_addr.size(); ++i) {
        if (m_addr[i]) return false;
    }
    return true;
}

bool CNetAddr::IsIPv4() const
{
    return m_net == NET_IPV4;
}

bool CNetAddr::IsIPv6() const
{
    return m_net == NET_IPV6;
}

bool CNetAddr::IsRFC1918() const
{
    return IsIPv4() && (
        GetByte(3) == 10 ||
        (GetByte(3) == 192 && GetByte(2) == 168) ||
        (GetByte(3) == 172 && (GetByte(2) >= 16 && GetByte(2) <= 31)));
}

bool CNetAddr::IsRFC2544() const
{
    return IsIPv4() && GetByte(3) == 198 && (GetByte(2) == 18 || GetByte(2) == 19);
}

bool CNetAddr::IsRFC3927() const
{
    return IsIPv4() && GetByte(3) == 169 && GetByte(2) == 254;
}

bool CNetAddr::IsRFC6598() const
{
    return IsIPv4() && GetByte(3) == 100 && (GetByte(2) >= 64 && GetByte(2) <= 127);
}

bool CNetAddr::IsRFC5737() const
{
    return IsIPv4() && ((GetByte(3) == 192 && GetByte(2) == 0 && GetByte(1) == 2) ||
        (GetByte(3) == 198 && GetByte(2) == 51 && GetByte(1) == 100) ||
        (GetByte(3) == 203 && GetByte(2) == 0 && GetByte(1) == 113));
}

bool CNetAddr::IsRFC3849() const
{
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x0D && GetByte(12) == 0xB8;
}

bool CNetAddr::IsRFC3964() const
{
    return IsIPv6() && (GetByte(15) == 0x20 && GetByte(14) == 0x02);
}

bool CNetAddr::IsRFC6052() const
{
    static const unsigned char pchRFC6052[] = {0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0};
    return IsIPv6() && (memcmp(m_addr.data(), pchRFC6052, sizeof(pchRFC6052)) == 0);
}

bool CNetAddr::IsRFC4380() const
{
    return IsIPv6() && (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0 && GetByte(12) == 0);
}

bool CNetAddr::IsRFC4862() const
{
    static const unsigned char pchRFC4862[] = {0xFE,0x80,0,0,0,0,0,0};
    return IsIPv6() && (memcmp(m_addr.data(), pchRFC4862, sizeof(pchRFC4862)) == 0);
}

bool CNetAddr::IsRFC4193() const
{
    return IsIPv6() && ((GetByte(15) & 0xFE) == 0xFC);
}

bool CNetAddr::IsRFC6145() const
{
    static const unsigned char pchRFC6145[] = {0,0,0,0,0,0,0,0,0xFF,0xFF,0,0};
    return (IsIPv4() || IsIPv6()) && (memcmp(m_addr.data(), pchRFC6145, sizeof(pchRFC6145)) == 0);
}

bool CNetAddr::IsRFC4843() const
{
    return IsIPv6() && (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 && (GetByte(12) & 0xF0) == 0x10);
}

bool CNetAddr::IsRFC7343() const
{
    return IsIPv6() && (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 && (GetByte(12) & 0xF0) == 0x20);
}

/**
 * @returns Whether or not this is a Tor onion address, either a legacy Tor v2
 *          (OnionCat) address or a Tor v3 address.
 *
 * @see CNetAddr::SetSpecial(const std::string &)
 */
bool CNetAddr::IsTor() const
{
    return m_net == NET_ONION;
}

/**
 * @returns Whether or not this is a Tor onion address, either a legacy Tor v2
 *          (OnionCat) address or a Tor v3 address.
 *
 * @see CNetAddr::IsTor()
 */
bool CNetAddr::IsOnion() const
{
    return m_net == NET_ONION;
}

/**
 * @returns Whether or not this is a Tor v3 onion address.
 *
 * @see CNetAddr::SetSpecial(const std::string &)
 */
bool CNetAddr::IsTorV3() const
{
    return m_net == NET_ONION && m_addr.size() == ADDR_TORV3_SIZE;
}

/**
 * @returns Whether or not this is an I2P destination address.
 *
 * @see CNetAddr::SetSpecial(const std::string &)
 */
bool CNetAddr::IsI2P() const
{
    return m_net == NET_I2P;
}

bool CNetAddr::IsLocal() const
{
    // IPv4 loopback (127.0.0.0/8 or 0.0.0.0/8)
    if (IsIPv4() && (GetByte(3) == 127 || GetByte(3) == 0))
        return true;

    // IPv6 loopback (::1/128)
    static const unsigned char pchLocal[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (IsIPv6() && memcmp(m_addr.data(), pchLocal, 16) == 0)
        return true;

    return false;
}

/**
 * @returns Whether or not this network address is a valid address that @a could
 *          be used to refer to an actual host.
 *
 * @note A valid address may or may not be publicly routable on the global
 *       internet. As in, the set of valid addresses is a superset of the set of
 *       publicly routable addresses.
 *
 * @see CNetAddr::IsRoutable()
 */
bool CNetAddr::IsValid() const
{
    // Tor v3 onion addresses and I2P destinations are valid addresses, they
    // just have no representation in the legacy (pre-BIP155) wire format.
    if (IsTorV3() || IsI2P()) {
        return true;
    }

    // Cleanup 3-byte shifted addresses caused by garbage in size field
    // of addr messages from versions before 0.2.9 checksum.
    // Two consecutive addr messages look like this:
    // header20 vectorlen3 addr26 addr26 addr26 header20 vectorlen3 addr26 addr26 addr26...
    // so if the first length field is garbled, it reads the second batch
    // of addr misaligned by 3 bytes.
    if (memcmp(m_addr.data(), pchIPv4 + 3, sizeof(pchIPv4) - 3) == 0)
        return false;

    // unspecified IPv6 address (::/128)
    unsigned char ipNone6[16] = {};
    if (memcmp(m_addr.data(), ipNone6, 16) == 0)
        return false;

    // documentation IPv6 address
    if (IsRFC3849())
        return false;

    if (IsInternal())
        return false;

    if (IsIPv4())
    {
        // INADDR_NONE
        uint32_t ipNone = INADDR_NONE;
        if (memcmp(m_addr.data() + 12, &ipNone, 4) == 0)
            return false;

        // 0
        ipNone = 0;
        if (memcmp(m_addr.data() + 12, &ipNone, 4) == 0)
            return false;
    }

    return true;
}

/**
 * @returns Whether or not this network address is publicly routable on the
 *          global internet.
 *
 * @note A routable address is always valid. As in, the set of routable addresses
 *       is a subset of the set of valid addresses.
 *
 * @see CNetAddr::IsValid()
 */
bool CNetAddr::IsRoutable() const
{
    return IsValid() && !(IsRFC1918() || IsRFC2544() || IsRFC3927() || IsRFC4862() || IsRFC6598() || IsRFC5737() || (IsRFC4193() && !IsTor()) || IsRFC4843() || IsRFC7343() || IsLocal() || IsInternal());
}

/**
 * @returns Whether or not this is a dummy address that maps a name into IPv6.
 *
 * @see CNetAddr::SetInternal(const std::string &)
 */
bool CNetAddr::IsInternal() const
{
   return m_net == NET_INTERNAL;
}

/**
 * @returns Whether or not this address can be represented in the legacy
 *          (pre-BIP155) wire format and in the on-disk addrman.
 *
 * Tor v3 onion addresses and I2P destinations cannot; they must be rejected at
 * the addrman and at the address relay boundary.
 */
bool CNetAddr::IsAddrV1Compatible() const
{
    switch (m_net) {
    case NET_IPV4:
    case NET_IPV6:
    case NET_INTERNAL:
        return true;
    case NET_ONION:
        // Only the legacy Tor v2 (OnionCat) form has a v1 representation.
        return m_addr.size() != ADDR_TORV3_SIZE;
    case NET_I2P:
        return false;
    case NET_UNROUTABLE:
    case NET_MAX:
        return false;
    }
    return false;
}

enum Network CNetAddr::GetNetwork() const
{
    if (IsInternal())
        return NET_INTERNAL;

    if (!IsRoutable())
        return NET_UNROUTABLE;

    if (IsIPv4())
        return NET_IPV4;

    if (IsTor())
        return NET_ONION;

    if (IsI2P())
        return NET_I2P;

    return NET_IPV6;
}

enum Network CNetAddr::GetNetClass() const
{
    if (IsTor())
        return NET_ONION;

    if (IsInternal())
        return NET_INTERNAL;

    if (IsI2P())
        return NET_I2P;

    if (IsIPv4())
        return NET_IPV4;

    if (IsValid() && IsIPv6())
        return NET_IPV6;

    return NET_UNROUTABLE;
}

std::string CNetAddr::ToStringIP() const
{
    if (IsTorV3()) {
        // TORv3 onion_address = base32(PUBKEY | CHECKSUM | VERSION) + ".onion"
        uint8_t checksum[torv3::CHECKSUM_LEN];
        torv3::Checksum(m_addr.data(), checksum);

        std::vector<unsigned char> address(m_addr.begin(), m_addr.end());
        address.insert(address.end(), checksum, checksum + torv3::CHECKSUM_LEN);
        address.insert(address.end(), torv3::VERSION, torv3::VERSION + sizeof(torv3::VERSION));

        return EncodeBase32(address.data(), address.size()) + ".onion";
    }
    if (IsTor())
        return EncodeBase32(m_addr.data() + 6, 10) + ".onion";
    if (IsI2P())
        return EncodeBase32(m_addr.data(), m_addr.size(), false /* don't pad with = */) + ".b32.i2p";
    if (IsInternal())
        return EncodeBase32(m_addr.data() + sizeof(g_internal_prefix), m_addr.size() - sizeof(g_internal_prefix)) + ".internal";
    CService serv(*this, 0);
    struct sockaddr_storage sockaddr;
    socklen_t socklen = sizeof(sockaddr);
    if (serv.GetSockAddr((struct sockaddr*)&sockaddr, &socklen)) {
        char name[1025] = "";
        if (!getnameinfo((const struct sockaddr*)&sockaddr, socklen, name, sizeof(name), nullptr, 0, NI_NUMERICHOST))
            return std::string(name);
    }
    if (IsIPv4())
        return strprintf("%u.%u.%u.%u", GetByte(3), GetByte(2), GetByte(1), GetByte(0));
    else
        return strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
                         GetByte(15) << 8 | GetByte(14), GetByte(13) << 8 | GetByte(12),
                         GetByte(11) << 8 | GetByte(10), GetByte(9) << 8 | GetByte(8),
                         GetByte(7) << 8 | GetByte(6), GetByte(5) << 8 | GetByte(4),
                         GetByte(3) << 8 | GetByte(2), GetByte(1) << 8 | GetByte(0));
}

std::string CNetAddr::ToString() const
{
    return ToStringIP();
}

bool operator==(const CNetAddr& a, const CNetAddr& b)
{
    return a.m_net == b.m_net && a.m_addr == b.m_addr;
}

bool operator<(const CNetAddr& a, const CNetAddr& b)
{
    if (a.m_net != b.m_net) {
        return a.m_net < b.m_net;
    }
    return a.m_addr < b.m_addr;
}

/**
 * Try to get our IPv4 address.
 *
 * @param[out] pipv4Addr The in_addr struct to which to copy.
 *
 * @returns Whether or not the operation was successful, in particular, whether
 *          or not our address was an IPv4 address.
 *
 * @see CNetAddr::IsIPv4()
 */
bool CNetAddr::GetInAddr(struct in_addr* pipv4Addr) const
{
    if (!IsIPv4())
        return false;
    memcpy(pipv4Addr, m_addr.data() + 12, 4);
    return true;
}

/**
 * Try to get our IPv6 address.
 *
 * @param[out] pipv6Addr The in6_addr struct to which to copy.
 *
 * @returns Whether or not the operation was successful, in particular, whether
 *          or not our address was an IPv6 address.
 *
 * @see CNetAddr::IsIPv6()
 */
bool CNetAddr::GetIn6Addr(struct in6_addr* pipv6Addr) const
{
    if (!IsIPv6()) {
        return false;
    }
    memcpy(pipv6Addr, m_addr.data(), 16);
    return true;
}

/**
 * Get the canonical identifier of our network group
 *
 * The groups are assigned in a way where it should be costly for an attacker to
 * obtain addresses with many different group identifiers, even if it is cheap to
 * obtain addresses with the same identifier.
 *
 * @note No two connections will be attempted to addresses with the same network
 *       group.
 */
std::vector<unsigned char> CNetAddr::GetGroup() const
{
    std::vector<unsigned char> vchRet;
    int nClass = NET_IPV6;
    int nStartByte = 0;
    int nBits = 16;

    // all local addresses belong to the same group
    if (IsLocal())
    {
        nClass = 255;
        nBits = 0;
    }
    // all internal-usage addresses get their own group
    if (IsInternal())
    {
        nClass = NET_INTERNAL;
        nStartByte = sizeof(g_internal_prefix);
        nBits = (m_addr.size() - sizeof(g_internal_prefix)) * 8;
    }
    // all other unroutable addresses belong to the same group
    else if (!IsRoutable())
    {
        nClass = NET_UNROUTABLE;
        nBits = 0;
    }
    // for IPv4 addresses, '1' + the 16 higher-order bits of the IP
    // includes mapped IPv4, SIIT translated IPv4, and the well-known prefix
    else if (IsIPv4() || IsRFC6145() || IsRFC6052())
    {
        nClass = NET_IPV4;
        nStartByte = 12;
    }
    // for 6to4 tunnelled addresses, use the encapsulated IPv4 address
    else if (IsRFC3964())
    {
        nClass = NET_IPV4;
        nStartByte = 2;
    }
    // for Teredo-tunnelled IPv6 addresses, use the encapsulated IPv4 address
    else if (IsRFC4380())
    {
        vchRet.push_back(NET_IPV4);
        vchRet.push_back(GetByte(3) ^ 0xFF);
        vchRet.push_back(GetByte(2) ^ 0xFF);
        return vchRet;
    }
    else if (IsTorV3() || IsI2P())
    {
        // Tor v3 and I2P addresses are grouped by the first four bits of the
        // address.
        vchRet.push_back(IsTorV3() ? NET_ONION : NET_I2P);
        vchRet.push_back(m_addr[0] | ((1 << (8 - 4)) - 1));
        return vchRet;
    }
    else if (IsTor())
    {
        nClass = NET_ONION;
        nStartByte = 6;
        nBits = 4;
    }
    // for he.net, use /36 groups
    else if (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x04 && GetByte(12) == 0x70)
        nBits = 36;
    // for the rest of the IPv6 network, use /32 groups
    else
        nBits = 32;

    vchRet.push_back(nClass);

    // push our ip onto vchRet byte by byte...
    while (nBits >= 8)
    {
        vchRet.push_back(GetByte(15 - nStartByte));
        nStartByte++;
        nBits -= 8;
    }
    // ...for the last byte, push nBits and for the rest of the byte push 1's
    if (nBits > 0)
        vchRet.push_back(GetByte(15 - nStartByte) | ((1 << (8 - nBits)) - 1));

    return vchRet;
}

uint64_t CNetAddr::GetHash() const
{
    uint256 hash = Hash(m_addr.data(), m_addr.data() + m_addr.size());
    uint64_t nRet;
    memcpy(&nRet, &hash, sizeof(nRet));
    return nRet;
}

// private extensions to enum Network, only returned by GetExtNetwork,
// and only used in GetReachabilityFrom
static const int NET_UNKNOWN = NET_MAX + 0;
static const int NET_TEREDO  = NET_MAX + 1;
int static GetExtNetwork(const CNetAddr *addr)
{
    if (addr == nullptr)
        return NET_UNKNOWN;
    if (addr->IsRFC4380())
        return NET_TEREDO;
    return addr->GetNetwork();
}

std::vector<unsigned char> CNetAddr::GetAddrBytes() const
{
    return std::vector<unsigned char>(m_addr.begin(), m_addr.end());
}

uint8_t CNetAddr::GetBIP155Network() const
{
    switch (m_net) {
    case NET_IPV4:
        return BIP155_IPV4;
    case NET_IPV6:
    case NET_INTERNAL:
        // Internal addresses are serialized as IPv6, like upstream does.
        return BIP155_IPV6;
    case NET_ONION:
        return IsTorV3() ? BIP155_TORV3 : BIP155_TORV2;
    case NET_I2P:
        return BIP155_I2P;
    case NET_UNROUTABLE:
    case NET_MAX:
        break;
    }
    return 0;
}

bool CNetAddr::IsAddrV2Compatible() const
{
    switch (m_net) {
    case NET_IPV4:
    case NET_IPV6:
    case NET_INTERNAL:
        return true;
    case NET_ONION:
        // Only the Tor v3 form has a BIP155 representation; the legacy Tor v2
        // (OnionCat) form is only kept for the legacy (pre-BIP155) wire format.
        return m_addr.size() == ADDR_TORV3_SIZE;
    case NET_I2P:
        return true;
    case NET_UNROUTABLE:
    case NET_MAX:
        break;
    }
    return false;
}

std::vector<unsigned char> CNetAddr::GetAddrV2Bytes() const
{
    if (!IsAddrV2Compatible()) {
        return {};
    }
    if (m_net == NET_IPV4) {
        // IPv4 addresses are stored in the IPv4-mapped IPv6 form, but the
        // BIP155 wire format uses the plain 4 byte form.
        if (m_addr.size() != ADDR_IPV6_SIZE) {
            return {};
        }
        return std::vector<unsigned char>(m_addr.end() - ADDR_IPV4_SIZE, m_addr.end());
    }
    return std::vector<unsigned char>(m_addr.begin(), m_addr.end());
}

bool CNetAddr::SetFromBIP155(uint8_t net, const unsigned char* data, size_t size)
{
    switch (net) {
    case BIP155_IPV4: {
        if (size != ADDR_IPV4_SIZE) {
            return false;
        }
        // The address layer keeps IPv4 addresses in the IPv4-mapped IPv6 form,
        // so that the addrman keys and the legacy wire format stay identical.
        uint8_t buf[ADDR_IPV6_SIZE] = {0};
        memcpy(buf, pchIPv4, sizeof(pchIPv4));
        memcpy(buf + ADDR_IPV6_SIZE - ADDR_IPV4_SIZE, data, ADDR_IPV4_SIZE);
        SetLegacyIPv6(buf);
        return true;
    }
    case BIP155_IPV6: {
        if (size != ADDR_IPV6_SIZE) {
            return false;
        }
        uint8_t buf[ADDR_IPV6_SIZE];
        memcpy(buf, data, ADDR_IPV6_SIZE);
        // Like upstream, the IPv4 and the legacy Tor v2 forms are not supposed
        // to be embedded in the IPv6 form of the BIP155 wire format. Such an
        // entry is ignored.
        if (memcmp(buf, pchIPv4, sizeof(pchIPv4)) == 0 || memcmp(buf, pchOnionCat, sizeof(pchOnionCat)) == 0) {
            return false;
        }
        // The network is derived from the well known prefixes, exactly like it
        // is done for the legacy (pre-BIP155) representation.
        SetLegacyIPv6(buf);
        return true;
    }
    case BIP155_TORV2:
        // The legacy Tor v2 (OnionCat) form is not supported by the BIP155
        // address layer of this node: the entry is parsed and skipped.
        return false;
    case BIP155_TORV3: {
        if (size != ADDR_TORV3_SIZE) {
            return false;
        }
        m_net = NET_ONION;
        m_addr.assign(data, data + size);
        return true;
    }
    case BIP155_I2P: {
        if (size != ADDR_I2P_SIZE) {
            return false;
        }
        m_net = NET_I2P;
        m_addr.assign(data, data + size);
        return true;
    }
    default:
        // An unknown network id may come from a future protocol version: the
        // entry is ignored instead of being treated as a protocol violation.
        return false;
    }
}

void CNetAddr::GetV1Array(uint8_t (&arr)[ADDR_IPV6_SIZE]) const
{
    if (IsAddrV1Compatible() && m_addr.size() == ADDR_IPV6_SIZE) {
        memcpy(arr, m_addr.data(), ADDR_IPV6_SIZE);
    } else {
        // Addresses without a legacy (pre-BIP155) representation must never
        // reach the v1 wire format or `addr.dat`; they are rejected at the
        // addrman and at the address relay boundary.
        memset(arr, 0, ADDR_IPV6_SIZE);
    }
}

/** Calculates a metric for how reachable (*this) is from a given partner */
int CNetAddr::GetReachabilityFrom(const CNetAddr *paddrPartner) const
{
    enum Reachability {
        REACH_UNREACHABLE,
        REACH_DEFAULT,
        REACH_TEREDO,
        REACH_IPV6_WEAK,
        REACH_IPV4,
        REACH_IPV6_STRONG,
        REACH_PRIVATE
    };

    if (!IsRoutable() || IsInternal())
        return REACH_UNREACHABLE;

    int ourNet = GetExtNetwork(this);
    int theirNet = GetExtNetwork(paddrPartner);
    bool fTunnel = IsRFC3964() || IsRFC6052() || IsRFC6145();

    switch(theirNet) {
    case NET_IPV4:
        switch(ourNet) {
        default:       return REACH_DEFAULT;
        case NET_IPV4: return REACH_IPV4;
        }
    case NET_IPV6:
        switch(ourNet) {
        default:         return REACH_DEFAULT;
        case NET_TEREDO: return REACH_TEREDO;
        case NET_IPV4:   return REACH_IPV4;
        case NET_IPV6:   return fTunnel ? REACH_IPV6_WEAK : REACH_IPV6_STRONG; // only prefer giving our IPv6 address if it's not tunnelled
        }
    case NET_ONION:
        switch(ourNet) {
        default:         return REACH_DEFAULT;
        case NET_IPV4:   return REACH_IPV4; // Tor users can connect to IPv4 as well
        case NET_ONION:    return REACH_PRIVATE;
        }
    case NET_I2P:
        switch(ourNet) {
        default:         return REACH_DEFAULT;
        case NET_IPV4:   return REACH_IPV4;
        case NET_I2P:    return REACH_PRIVATE;
        }
    case NET_TEREDO:
        switch(ourNet) {
        default:          return REACH_DEFAULT;
        case NET_TEREDO:  return REACH_TEREDO;
        case NET_IPV6:    return REACH_IPV6_WEAK;
        case NET_IPV4:    return REACH_IPV4;
        }
    case NET_UNKNOWN:
    case NET_UNROUTABLE:
    default:
        switch(ourNet) {
        default:          return REACH_DEFAULT;
        case NET_TEREDO:  return REACH_TEREDO;
        case NET_IPV6:    return REACH_IPV6_WEAK;
        case NET_IPV4:    return REACH_IPV4;
        case NET_ONION:     return REACH_PRIVATE; // either from Tor, or don't care about our address
        }
    }
}

CService::CService() : port(0)
{
}

CService::CService(const CNetAddr& cip, unsigned short portIn) : CNetAddr(cip), port(portIn)
{
}

CService::CService(const struct in_addr& ipv4Addr, unsigned short portIn) : CNetAddr(ipv4Addr), port(portIn)
{
}

CService::CService(const struct in6_addr& ipv6Addr, unsigned short portIn) : CNetAddr(ipv6Addr), port(portIn)
{
}

CService::CService(const struct sockaddr_in& addr) : CNetAddr(addr.sin_addr), port(ntohs(addr.sin_port))
{
    assert(addr.sin_family == AF_INET);
}

CService::CService(const struct sockaddr_in6 &addr) : CNetAddr(addr.sin6_addr, addr.sin6_scope_id), port(ntohs(addr.sin6_port))
{
   assert(addr.sin6_family == AF_INET6);
}

bool CService::SetSockAddr(const struct sockaddr *paddr)
{
    switch (paddr->sa_family) {
    case AF_INET:
        *this = CService(*(const struct sockaddr_in*)paddr);
        return true;
    case AF_INET6:
        *this = CService(*(const struct sockaddr_in6*)paddr);
        return true;
    default:
        return false;
    }
}

unsigned short CService::GetPort() const
{
    return port;
}

bool operator==(const CService& a, const CService& b)
{
    return static_cast<CNetAddr>(a) == static_cast<CNetAddr>(b) && a.port == b.port;
}

bool operator<(const CService& a, const CService& b)
{
    return static_cast<CNetAddr>(a) < static_cast<CNetAddr>(b) || (static_cast<CNetAddr>(a) == static_cast<CNetAddr>(b) && a.port < b.port);
}

/**
 * Obtain the IPv4/6 socket address this represents.
 *
 * @param[out] paddr The obtained socket address.
 * @param[in,out] addrlen The size, in bytes, of the address structure pointed
 *                        to by paddr. The value that's pointed to by this
 *                        parameter might change after calling this function if
 *                        the size of the corresponding address structure
 *                        changed.
 *
 * @returns Whether or not the operation was successful.
 */
bool CService::GetSockAddr(struct sockaddr* paddr, socklen_t *addrlen) const
{
    if (IsIPv4()) {
        if (*addrlen < (socklen_t)sizeof(struct sockaddr_in))
            return false;
        *addrlen = sizeof(struct sockaddr_in);
        struct sockaddr_in *paddrin = (struct sockaddr_in*)paddr;
        memset(paddrin, 0, *addrlen);
        if (!GetInAddr(&paddrin->sin_addr))
            return false;
        paddrin->sin_family = AF_INET;
        paddrin->sin_port = htons(port);
        return true;
    }
    if (IsIPv6()) {
        if (*addrlen < (socklen_t)sizeof(struct sockaddr_in6))
            return false;
        *addrlen = sizeof(struct sockaddr_in6);
        struct sockaddr_in6 *paddrin6 = (struct sockaddr_in6*)paddr;
        memset(paddrin6, 0, *addrlen);
        if (!GetIn6Addr(&paddrin6->sin6_addr))
            return false;
        paddrin6->sin6_scope_id = scopeId;
        paddrin6->sin6_family = AF_INET6;
        paddrin6->sin6_port = htons(port);
        return true;
    }
    return false;
}

/**
 * @returns An identifier unique to this service's address and port number.
 */
std::vector<unsigned char> CService::GetKey() const
{
    std::vector<unsigned char> vKey = GetAddrBytes();
    vKey.push_back(port / 0x100); // most significant byte of our port
    vKey.push_back(port & 0x0FF); // least significant byte of our port
    return vKey;
}

std::string CService::ToStringPort() const
{
    return strprintf("%u", port);
}

std::string CService::ToStringIPPort() const
{
    if (IsIPv4() || IsTor() || IsI2P() || IsInternal()) {
        return ToStringIP() + ":" + ToStringPort();
    } else {
        return "[" + ToStringIP() + "]:" + ToStringPort();
    }
}

std::string CService::ToString() const
{
    return ToStringIPPort();
}

CSubNet::CSubNet():
    valid(false)
{
    memset(netmask, 0, sizeof(netmask));
}

CSubNet::CSubNet(const CNetAddr &addr, int32_t mask)
{
    valid = true;
    network = addr;
    // Default to /32 (IPv4) or /128 (IPv6), i.e. match single address
    memset(netmask, 255, sizeof(netmask));

    // Only addresses with a legacy (pre-BIP155) representation can be used in
    // a subnet; Tor v3 and I2P addresses have no v1 representation and are
    // rejected explicitly.
    if (!network.IsAddrV1Compatible() || network.m_addr.size() != sizeof(netmask)) {
        valid = false;
        return;
    }

    // IPv4 addresses start at offset 12, and first 12 bytes must match, so just offset n
    const int astartofs = network.IsIPv4() ? 12 : 0;

    int32_t n = mask;
    if(n >= 0 && n <= (128 - astartofs*8)) // Only valid if in range of bits of address
    {
        n += astartofs*8;
        // Clear bits [n..127]
        for (; n < 128; ++n)
            netmask[n>>3] &= ~(1<<(7-(n&7)));
    } else
        valid = false;

    // Normalize network according to netmask
    for(int x=0; x<16; ++x)
        network.m_addr[x] &= netmask[x];
}

CSubNet::CSubNet(const CNetAddr &addr, const CNetAddr &mask)
{
    valid = true;
    network = addr;
    // Default to /32 (IPv4) or /128 (IPv6), i.e. match single address
    memset(netmask, 255, sizeof(netmask));

    // Only addresses with a legacy (pre-BIP155) representation can be used in
    // a subnet; Tor v3 and I2P addresses have no v1 representation and are
    // rejected explicitly.
    if (!network.IsAddrV1Compatible() || network.m_addr.size() != sizeof(netmask) ||
        !mask.IsAddrV1Compatible() || mask.m_addr.size() != sizeof(netmask)) {
        valid = false;
        return;
    }

    // IPv4 addresses start at offset 12, and first 12 bytes must match, so just offset n
    const int astartofs = network.IsIPv4() ? 12 : 0;

    for(int x=astartofs; x<16; ++x)
        netmask[x] = mask.m_addr[x];

    // Normalize network according to netmask
    for(int x=0; x<16; ++x)
        network.m_addr[x] &= netmask[x];
}

CSubNet::CSubNet(const CNetAddr &addr):
    valid(addr.IsValid() && addr.IsAddrV1Compatible())
{
    memset(netmask, 255, sizeof(netmask));
    network = addr;
}

/**
 * @returns True if this subnet is valid, the specified address is valid, and
 *          the specified address belongs in this subnet.
 */
bool CSubNet::Match(const CNetAddr &addr) const
{
    if (!valid || !addr.IsValid() || !addr.IsAddrV1Compatible())
        return false;
    if (addr.m_addr.size() != sizeof(netmask) || network.m_addr.size() != sizeof(netmask))
        return false;
    for(int x=0; x<16; ++x)
        if ((addr.m_addr[x] & netmask[x]) != network.m_addr[x])
            return false;
    return true;
}

/**
 * @returns The number of 1-bits in the prefix of the specified subnet mask. If
 *          the specified subnet mask is not a valid one, -1.
 */
static inline int NetmaskBits(uint8_t x)
{
    switch(x) {
    case 0x00: return 0;
    case 0x80: return 1;
    case 0xc0: return 2;
    case 0xe0: return 3;
    case 0xf0: return 4;
    case 0xf8: return 5;
    case 0xfc: return 6;
    case 0xfe: return 7;
    case 0xff: return 8;
    default: return -1;
    }
}

std::string CSubNet::ToString() const
{
    /* Parse binary 1{n}0{N-n} to see if mask can be represented as /n */
    int cidr = 0;
    bool valid_cidr = true;
    int n = network.IsIPv4() ? 12 : 0;
    for (; n < 16 && netmask[n] == 0xff; ++n)
        cidr += 8;
    if (n < 16) {
        int bits = NetmaskBits(netmask[n]);
        if (bits < 0)
            valid_cidr = false;
        else
            cidr += bits;
        ++n;
    }
    for (; n < 16 && valid_cidr; ++n)
        if (netmask[n] != 0x00)
            valid_cidr = false;

    /* Format output */
    std::string strNetmask;
    if (valid_cidr) {
        strNetmask = strprintf("%u", cidr);
    } else {
        if (network.IsIPv4())
            strNetmask = strprintf("%u.%u.%u.%u", netmask[12], netmask[13], netmask[14], netmask[15]);
        else
            strNetmask = strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
                             netmask[0] << 8 | netmask[1], netmask[2] << 8 | netmask[3],
                             netmask[4] << 8 | netmask[5], netmask[6] << 8 | netmask[7],
                             netmask[8] << 8 | netmask[9], netmask[10] << 8 | netmask[11],
                             netmask[12] << 8 | netmask[13], netmask[14] << 8 | netmask[15]);
    }

    return network.ToString() + "/" + strNetmask;
}

bool CSubNet::IsValid() const
{
    return valid;
}

bool operator==(const CSubNet& a, const CSubNet& b)
{
    return a.valid == b.valid && a.network == b.network && !memcmp(a.netmask, b.netmask, 16);
}

bool operator<(const CSubNet& a, const CSubNet& b)
{
    return (a.network < b.network || (a.network == b.network && memcmp(a.netmask, b.netmask, 16) < 0));
}
