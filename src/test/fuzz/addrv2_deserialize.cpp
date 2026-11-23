// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <protocol.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>

#include <cassert>
#include <vector>

/**
 * Fuzz the deserialization of an "addrv2" message.
 *
 * The input is interpreted as the payload of an "addrv2" message and decoded
 * entry by entry. The decoder must never throw, never read an unbounded
 * amount of memory and never run into an endless loop, no matter how the
 * payload is crafted.
 */
FUZZ_TARGET(addrv2_deserialize)
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    if (!buffer.empty()) {
        stream.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    }

    std::vector<CAddress> vAddr;
    const uint64_t nCount = DecodeAddrV2Message(stream, vAddr);

    // The decoder never returns more addresses than the peer announced, and
    // the number of decoded addresses is bounded by the number of entries a
    // message may contain.
    assert(vAddr.size() <= nCount);
    assert(vAddr.size() <= MAX_ADDRV2_ENTRIES + 1);

    for (const CAddress& addr : vAddr) {
        // Every decoded address has a representation in the BIP155 wire
        // format and can therefore be encoded again.
        assert(addr.IsAddrV2Compatible());
        assert(!addr.GetAddrV2Bytes().empty());
    }
}
