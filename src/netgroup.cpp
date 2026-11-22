// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Chain-local variant of the upstream Bitcoin Core netgroup manager
// (src/netgroup.{h,cpp}).

#include <netgroup.h>

#include <hash.h>
#include <uint256.h>
#include <util/asmap.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

/** The IPv4-in-IPv6 prefix (::ffff:0:0/96) used to look up IPv4 ranges. */
static const unsigned char IPV4_IN_IPV6_PREFIX[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

/**
 * Build the 16 byte lookup key of an address.
 *
 * IPv4 ranges are looked up in the IPv4-in-IPv6 space (::ffff:0:0/96), which is
 * the representation the ASMap tooling uses for IPv4 prefixes. Addresses that
 * embed an IPv4 address (SIIT, NAT64, 6to4, Teredo) are looked up via their
 * embedded IPv4 address, exactly like upstream does.
 *
 * @return false if the address has no mappable IPv4 address.
 */
bool BuildLookupKey(const CNetAddr& address, std::vector<unsigned char>& key)
{
    const std::vector<unsigned char> bytes = address.GetAddrBytes();
    if (bytes.size() != 16) return false;
    key.assign(16, 0);
    if (address.IsIPv4()) {
        // IPv4 addresses are stored in the IPv4-mapped IPv6 form, so the stored
        // bytes are already the lookup key.
        key = bytes;
        return true;
    }
    if (address.IsRFC6145() || address.IsRFC6052()) {
        // SIIT translated IPv4 and NAT64: the IPv4 address is the last 4 bytes.
        for (int i = 0; i < 4; ++i) {
            key[12 + i] = bytes[12 + i];
        }
    } else if (address.IsRFC3964()) {
        // 6to4 tunneled IPv4: the IPv4 address is in bytes 2-5.
        for (int i = 0; i < 4; ++i) {
            key[12 + i] = bytes[2 + i];
        }
    } else if (address.IsRFC4380()) {
        // Teredo tunneled IPv4: the IPv4 address is in the last 4 bytes, but
        // bitflipped.
        for (int i = 0; i < 4; ++i) {
            key[12 + i] = bytes[12 + i] ^ 0xFF;
        }
    } else {
        // Plain IPv6: use all 128 bits of the address.
        key = bytes;
        return true;
    }
    // Map the IPv4 address into the IPv4-in-IPv6 space.
    for (int i = 0; i < 12; ++i) {
        key[i] = IPV4_IN_IPV6_PREFIX[i];
    }
    return true;
}

} // anonymous namespace

NetgroupManager::NetgroupManager(std::vector<unsigned char>&& asmap)
    : m_asmap{std::move(asmap)},
      m_asmap_version{AsmapVersion(m_asmap)}
{
}

NetgroupManager NetgroupManager::NoAsmap()
{
    return NetgroupManager({});
}

NetgroupManager NetgroupManager::WithLoadedAsmap(std::vector<unsigned char>&& asmap)
{
    return NetgroupManager(std::move(asmap));
}

const uint256& NetgroupManager::GetAsmapVersion() const
{
    return m_asmap_version;
}

std::vector<unsigned char> NetgroupManager::GetGroup(const CNetAddr& address) const
{
    // If a non-empty asmap is supplied and the address is IPv4/IPv6, return
    // the ASN to be used for bucketing.
    const uint32_t asn = GetMappedAS(address);
    if (asn != 0) {
        // IPv4 and IPv6 with the same ASN are in the same bucket.
        std::vector<unsigned char> vchRet;
        vchRet.push_back(NET_IPV6);
        for (int i = 0; i < 4; i++) {
            vchRet.push_back((asn >> (8 * i)) & 0xFF);
        }
        return vchRet;
    }

    // Fall back to the classic (prefix based) grouping. CNetAddr::GetGroup()
    // is the single implementation of it and stays untouched.
    return address.GetGroup();
}

uint32_t NetgroupManager::GetMappedAS(const CNetAddr& address) const
{
    if (m_asmap.empty()) return 0;
    // Only publicly routable IPv4/IPv6 addresses are looked up; everything
    // else (Tor, I2P, internal, unroutable) is not mappable.
    if (!address.IsRoutable() || !(address.IsIPv4() || address.IsIPv6())) {
        return 0; // Indicates not found, safe because AS0 is reserved per RFC7607.
    }
    std::vector<unsigned char> key;
    if (!BuildLookupKey(address, key)) return 0;
    return Interpret(m_asmap, key);
}

bool NetgroupManager::UsingASMap() const
{
    return !m_asmap.empty();
}

const std::shared_ptr<const NetgroupManager>& DefaultNetgroupManager()
{
    static const std::shared_ptr<const NetgroupManager> manager{
        std::make_shared<const NetgroupManager>(NetgroupManager::NoAsmap())};
    return manager;
}
