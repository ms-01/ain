// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NETADDRESS_H
#define DEFI_NETADDRESS_H

#if defined(HAVE_CONFIG_H)
#include <config/defi-config.h>
#endif

#include <compat.h>
#include <prevector.h>
#include <serialize.h>
#include <span.h>

#include <stdint.h>
#include <string>
#include <vector>

/**
 * A network type.
 *
 * @note An address may belong to more than one network, for example `10.0.0.1`
 *       belongs to both `NET_UNROUTABLE` and `NET_IPV4`.
 *       Keep these sequential starting from 0 and `NET_MAX` as the last entry.
 *       We have loops like `for (int i = 0; i < NET_MAX; i++)` that expect to
 *       iterate over all enum values and also `GetExtNetwork()` "extends" this
 *       enum by introducing standalone constants starting from `NET_MAX`.
 */
enum Network
{
    /// Addresses from these networks are not publicly routable on the global Internet.
    NET_UNROUTABLE = 0,

    /// IPv4
    NET_IPV4,

    /// IPv6
    NET_IPV6,

    /// Tor (v2 or v3)
    NET_ONION,

    /// Internal (hash of a name/FQDN, used by the addrman to track DNS seeds)
    NET_INTERNAL,

    /// I2P
    NET_I2P,

    /// Dummy value to indicate the number of NET_* constants.
    NET_MAX,
};

/// Size of an IPv6 address (in bytes). This is also the size of an address in
/// the legacy (pre-BIP155) wire format.
static constexpr size_t ADDR_IPV6_SIZE = 16;

/// Size of a Tor v3 onion address (in bytes): the ed25519 public key.
static constexpr size_t ADDR_TORV3_SIZE = 32;

/// Size of an I2P destination address (in bytes).
static constexpr size_t ADDR_I2P_SIZE = 32;

/**
 * Network address.
 *
 * The address is stored as a raw byte string plus the network it belongs to.
 *
 * For the networks that have a representation in the legacy (pre-BIP155, "v1")
 * wire format - IPv4, IPv6, the legacy Tor v2 OnionCat mapping and the
 * internal addresses - the raw bytes are exactly the 16 v1 bytes, so the
 * serialized form and the addrman keys of those networks stay byte-identical
 * to the ones of the previous implementation.
 *
 * Tor v3 onion addresses (32 byte ed25519 public key) and I2P destinations
 * (32 bytes) have no v1 representation. They are never serialized into the v1
 * wire format or into `addr.dat`; they are rejected at the addrman and at the
 * address relay boundary instead.
 */
class CNetAddr
{
    protected:
        /**
         * Raw representation of the network address.
         *
         * - 16 bytes for IPv4 (in the IPv4-mapped IPv6 form), IPv6, the legacy
         *   Tor v2 OnionCat form and internal addresses,
         * - 32 bytes for a Tor v3 onion address (the ed25519 public key),
         * - 32 bytes for an I2P destination address.
         */
        prevector<ADDR_IPV6_SIZE, uint8_t> m_addr{ADDR_IPV6_SIZE, 0x0};

        /**
         * Network to which this address belongs.
         */
        Network m_net{NET_IPV6};

        /// Scope id if scoped/link-local IPV6 address.
        /// See https://tools.ietf.org/html/rfc4007
        uint32_t scopeId{0};

    public:
        CNetAddr();
        explicit CNetAddr(const struct in_addr& ipv4Addr);
        void SetIP(const CNetAddr& ip);

    private:
        /**
         * Set raw IPv4 or IPv6 address (in network byte order)
         * @note Only NET_IPV4 and NET_IPV6 are allowed for network.
         */
        void SetRaw(Network network, const uint8_t *data);

        /**
         * Set this object from the 16 byte legacy (pre-BIP155) representation.
         *
         * The network is derived from the well known prefixes, exactly like the
         * previous implementation did.
         */
        void SetLegacyIPv6(const uint8_t (&ip)[ADDR_IPV6_SIZE]);

    public:
        bool SetInternal(const std::string& name);

        bool SetSpecial(const std::string &strName); // for Tor and I2P addresses
        bool IsBindAny() const; // INADDR_ANY equivalent
        bool IsIPv4() const;    // IPv4 mapped address (::FFFF:0:0/96, 0.0.0.0/0)
        bool IsIPv6() const;    // IPv6 address (not mapped IPv4, not Tor, not I2P)
        bool IsRFC1918() const; // IPv4 private networks (10.0.0.0/8, 192.168.0.0/16, 172.16.0.0/12)
        bool IsRFC2544() const; // IPv4 inter-network communications (192.18.0.0/15)
        bool IsRFC6598() const; // IPv4 ISP-level NAT (100.64.0.0/10)
        bool IsRFC5737() const; // IPv4 documentation addresses (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24)
        bool IsRFC3849() const; // IPv6 documentation address (2001:0DB8::/32)
        bool IsRFC3927() const; // IPv4 autoconfig (169.254.0.0/16)
        bool IsRFC3964() const; // IPv6 6to4 tunnelling (2002::/16)
        bool IsRFC4193() const; // IPv6 unique local (FC00::/7)
        bool IsRFC4380() const; // IPv6 Teredo tunnelling (2001::/32)
        bool IsRFC4843() const; // IPv6 ORCHID (deprecated) (2001:10::/28)
        bool IsRFC7343() const; // IPv6 ORCHIDv2 (2001:20::/28)
        bool IsRFC4862() const; // IPv6 autoconfig (FE80::/64)
        bool IsRFC6052() const; // IPv6 well-known prefix for IPv4-embedded address (64:FF9B::/96)
        bool IsRFC6145() const; // IPv6 IPv4-translated address (::FFFF:0:0:0/96) (actually defined in RFC2765)
        /**
         * Whether this address is a Tor onion address, no matter whether it is a
         * legacy Tor v2 (OnionCat) address or a Tor v3 address.
         */
        bool IsTor() const;
        /**
         * Whether this address is a Tor onion address, no matter whether it is a
         * legacy Tor v2 (OnionCat) address or a Tor v3 address.
         *
         * Semantically identical to IsTor(); kept as the explicit name for the
         * "onion" network class.
         */
        bool IsOnion() const;
        /**
         * Whether this address is a Tor v3 onion address (56 character base32
         * encoded ed25519 public key).
         */
        bool IsTorV3() const;
        /**
         * Whether this address is an I2P destination address.
         */
        bool IsI2P() const;
        bool IsLocal() const;
        bool IsRoutable() const;
        bool IsInternal() const;
        bool IsValid() const;
        /**
         * Whether this address can be represented in the legacy (pre-BIP155)
         * wire format and in the on-disk addrman.
         *
         * This is false for Tor v3 onion addresses and for I2P addresses, which
         * have no v1 representation.
         */
        bool IsAddrV1Compatible() const;
        enum Network GetNetwork() const;
        /**
         * Get the network type of this address.
         *
         * In contrast to GetNetwork(), the result does not depend on the
         * routability of the address, e.g. a private IPv4 address is reported
         * as NET_IPV4.
         */
        enum Network GetNetClass() const;
        std::string ToString() const;
        std::string ToStringIP() const;
        unsigned int GetByte(int n) const;
        uint64_t GetHash() const;
        bool GetInAddr(struct in_addr* pipv4Addr) const;
        std::vector<unsigned char> GetGroup() const;
        /**
         * Return the raw bytes of this address.
         *
         * For all networks with a legacy (pre-BIP155) representation this is
         * the 16 byte v1 serialization (IPv4 addresses are stored as
         * IPv4-mapped IPv6, so the mapping is faithful). For Tor v3 onion
         * addresses and I2P addresses this is the raw 32 byte address.
         */
        std::vector<unsigned char> GetAddrBytes() const;
        int GetReachabilityFrom(const CNetAddr *paddrPartner = nullptr) const;

        explicit CNetAddr(const struct in6_addr& pipv6Addr, const uint32_t scope = 0);
        bool GetIn6Addr(struct in6_addr* pipv6Addr) const;

        friend bool operator==(const CNetAddr& a, const CNetAddr& b);
        friend bool operator!=(const CNetAddr& a, const CNetAddr& b) { return !(a == b); }
        friend bool operator<(const CNetAddr& a, const CNetAddr& b);

        ADD_SERIALIZE_METHODS;

        template <typename Stream, typename Operation>
        inline void SerializationOp(Stream& s, Operation ser_action) {
            uint8_t serialized[ADDR_IPV6_SIZE];
            if (ser_action.ForRead()) {
                READWRITE(serialized);
                SetLegacyIPv6(serialized);
            } else {
                GetV1Array(serialized);
                READWRITE(serialized);
            }
        }

        friend class CSubNet;

    private:
        /**
         * Fill `arr` with the legacy (pre-BIP155) 16 byte representation of this
         * address.
         *
         * Addresses without a v1 representation (Tor v3, I2P) are serialized as
         * all-zeroes; they must never reach the v1 wire format or `addr.dat`
         * (see IsAddrV1Compatible()).
         */
        void GetV1Array(uint8_t (&arr)[ADDR_IPV6_SIZE]) const;
};

class CSubNet
{
    protected:
        /// Network (base) address
        CNetAddr network;
        /// Netmask, in network byte order
        uint8_t netmask[16];
        /// Is this value valid? (only used to signal parse errors)
        bool valid;

    public:
        CSubNet();
        CSubNet(const CNetAddr &addr, int32_t mask);
        CSubNet(const CNetAddr &addr, const CNetAddr &mask);

        //constructor for single ip subnet (<ipv4>/32 or <ipv6>/128)
        explicit CSubNet(const CNetAddr &addr);

        bool Match(const CNetAddr &addr) const;

        std::string ToString() const;
        bool IsValid() const;

        friend bool operator==(const CSubNet& a, const CSubNet& b);
        friend bool operator!=(const CSubNet& a, const CSubNet& b) { return !(a == b); }
        friend bool operator<(const CSubNet& a, const CSubNet& b);

        ADD_SERIALIZE_METHODS;

        template <typename Stream, typename Operation>
        inline void SerializationOp(Stream& s, Operation ser_action) {
            READWRITE(network);
            READWRITE(netmask);
            READWRITE(valid);
        }
};

/** A combination of a network address (CNetAddr) and a (TCP) port */
class CService : public CNetAddr
{
    protected:
        uint16_t port; // host order

    public:
        CService();
        CService(const CNetAddr& ip, unsigned short port);
        CService(const struct in_addr& ipv4Addr, unsigned short port);
        explicit CService(const struct sockaddr_in& addr);
        unsigned short GetPort() const;
        bool GetSockAddr(struct sockaddr* paddr, socklen_t *addrlen) const;
        bool SetSockAddr(const struct sockaddr* paddr);
        friend bool operator==(const CService& a, const CService& b);
        friend bool operator!=(const CService& a, const CService& b) { return !(a == b); }
        friend bool operator<(const CService& a, const CService& b);
        std::vector<unsigned char> GetKey() const;
        std::string ToString() const;
        std::string ToStringPort() const;
        std::string ToStringIPPort() const;

        CService(const struct in6_addr& ipv6Addr, unsigned short port);
        explicit CService(const struct sockaddr_in6& addr);

        ADD_SERIALIZE_METHODS;

        template <typename Stream, typename Operation>
        inline void SerializationOp(Stream& s, Operation ser_action) {
            READWRITEAS(CNetAddr, *this);
            READWRITE(WrapBigEndian(port));
        }
};

#endif // DEFI_NETADDRESS_H
