// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Chain-local variant of the upstream Bitcoin Core netgroup manager
// (src/netgroup.{h,cpp}). It is reduced to the parts that are needed for the
// ASN based address bucketing: there is no embedded ASMap, the classic
// grouping is delegated to the untouched CNetAddr::GetGroup().

#ifndef DEFI_NETGROUP_H
#define DEFI_NETGROUP_H

#include <netaddress.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <vector>

/**
 * Netgroup manager
 *
 * The manager owns the (optional) ASMap and computes the netgroup of an
 * address. Without an ASMap the classic (prefix based) grouping of
 * CNetAddr::GetGroup() is used, so the behaviour of the node is unchanged.
 */
class NetgroupManager
{
public:
    NetgroupManager(const NetgroupManager&) = delete;
    NetgroupManager(NetgroupManager&&) = default;
    NetgroupManager& operator=(const NetgroupManager&) = delete;
    NetgroupManager& operator=(NetgroupManager&&) = delete;

    /** Create a manager that does not use an ASMap (classic grouping). */
    static NetgroupManager NoAsmap();

    /** Create a manager that uses the given (already validated) ASMap data. */
    static NetgroupManager WithLoadedAsmap(std::vector<unsigned char>&& asmap);

    /** Get the asmap version, a checksum identifying the asmap being used. */
    const uint256& GetAsmapVersion() const;

    /**
     * Get the canonical identifier of the network group for address.
     *
     * The groups are assigned in a way where it should be costly for an attacker to
     * obtain addresses with many different group identifiers, even if it is cheap to
     * obtain addresses with the same identifier.
     *
     * @note No two connections will be attempted to addresses with the same network
     *       group.
     */
    std::vector<unsigned char> GetGroup(const CNetAddr& address) const;

    /**
     * Get the autonomous system on the BGP path to address.
     *
     * The ip->AS mapping depends on how asmap is constructed.
     *
     * @return the ASN, or 0 if the address is not mapped (or not mappable at all).
     */
    uint32_t GetMappedAS(const CNetAddr& address) const;

    /**
     * Indicates whether ASMap is being used for clearnet bucketing.
     */
    bool UsingASMap() const;

private:
    /**
     * Compressed IP->ASN mapping.
     *
     * The mapping is loaded from a file at runtime; it is empty if no file was
     * provided. If a new asmap is provided, the existing addrman records are
     * re-bucketed.
     */
    std::vector<unsigned char> m_asmap;
    uint256 m_asmap_version;

    explicit NetgroupManager(std::vector<unsigned char>&& asmap);
};

/**
 * The shared instance of a netgroup manager that does not use an ASMap.
 *
 * It is used by every CAddrMan that is created without an explicit manager
 * (e.g. in the unit tests).
 */
const std::shared_ptr<const NetgroupManager>& DefaultNetgroupManager();

#endif // DEFI_NETGROUP_H
