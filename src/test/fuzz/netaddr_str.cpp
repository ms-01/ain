// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <netaddress.h>
#include <test/fuzz/fuzz.h>

#include <cassert>
#include <string>

/**
 * Fuzz the parsing of privacy network addresses (Tor v3 onion addresses and
 * I2P destinations).
 *
 * The input is interpreted as a string and handed to CNetAddr::SetSpecial(),
 * which parses the base32 encoding, verifies the Tor v3 checksum and the
 * version byte and derives the textual representation of the address.
 */
FUZZ_TARGET(netaddr_str)
{
    // The address is a printable string; NUL bytes terminate it.
    const std::string str(reinterpret_cast<const char*>(buffer.data()), buffer.size());

    CNetAddr addr;
    if (!addr.SetSpecial(str)) {
        return;
    }

    // A parsed address must round trip through its textual representation.
    const std::string text = addr.ToStringIP();
    CNetAddr roundtrip;
    if (roundtrip.SetSpecial(text)) {
        assert(roundtrip == addr);
        assert(roundtrip.GetAddrBytes() == addr.GetAddrBytes());
    }

    // The derived address must be consistent with the raw bytes: a Tor v3
    // onion address and an I2P destination are 32 bytes, the legacy Tor v2
    // (OnionCat) address has the 16 bytes of its v1 representation.
    if (addr.IsTorV3() || addr.IsI2P()) {
        assert(addr.GetAddrBytes().size() == 32U);
    } else {
        assert(addr.IsTor());
        assert(addr.GetAddrBytes().size() == 16U);
    }
}
