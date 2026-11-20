#ifndef DEFI_CRYPTO_SHA3_H
#define DEFI_CRYPTO_SHA3_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

// SH3 256, but safer version, that always resizes the output to 256 bits.
// Otherwise, the internal sha3_256 call can fail. 
bool sha3_256_safe(const std::vector<unsigned char> &input, std::vector<unsigned char> &output);

/**
 * NIST SHA3-256 (FIPS 202).
 *
 * This is *not* the same as the legacy Keccak-256 (padding byte 0x01) that is
 * used for the Ethereum hashing in this codebase. The Tor v3 onion service
 * address checksum is defined in terms of the FIPS 202 SHA3-256.
 *
 * @returns 0 on success, -1 on invalid arguments.
 */
int sha3_256_fips202(uint8_t* out, size_t outlen, const uint8_t* in, size_t inlen);

#endif // DEFI_CRYPTO_SHA3_H