// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// This is a C++17 port of the upstream Bitcoin Core ASMap parser
// (src/util/asmap.{h,cpp}). The upstream implementation is written against
// C++20 (std::span, std::byte, <bit>); this port uses plain
// std::vector<unsigned char> instead.

#ifndef DEFI_UTIL_ASMAP_H
#define DEFI_UTIL_ASMAP_H

#include <fs.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

/**
 * Interpret the ASMap bytecode for the given 16 byte IP address.
 *
 * @param asmap The binary ASMap data.
 * @param ip The 16 byte address (in network byte order) to look up.
 * @return The ASN of the address, or 0 if the address is not mapped.
 */
uint32_t Interpret(const std::vector<unsigned char>& asmap, const std::vector<unsigned char>& ip);

/**
 * Validate the structure of the ASMap data.
 *
 * @param asmap The binary ASMap data.
 * @param bits The number of bits of the addresses the map is used for (128 for IPv6).
 * @return true if the data is a well formed ASMap.
 */
bool SanityCheckAsmap(const std::vector<unsigned char>& asmap, int bits);

/** Check standard asmap data (128 bits for IPv6) */
bool CheckStandardAsmap(const std::vector<unsigned char>& data);

/** Read and check asmap from provided binary file */
std::vector<unsigned char> DecodeAsmap(const fs::path& path);

/** Calculate the asmap version, a checksum identifying the asmap being used. */
uint256 AsmapVersion(const std::vector<unsigned char>& data);

#endif // DEFI_UTIL_ASMAP_H
