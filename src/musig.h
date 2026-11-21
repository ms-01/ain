// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// MuSig2 (BIP 327) support, ported from Bitcoin Core (src/musig.{h,cpp}).
//
// DFI-specific deviations from upstream (see src/musig.cpp for the per-file notes):
//  - The fork targets C++17 (upstream v32 targets C++20), so std::span based
//    helpers are replaced by the fork's C++17 equivalents.
//  - `CreateMuSig2SyntheticXpub()` (BIP 328) is not part of this port: the fork's
//    ChainCode is a big-endian uint256 (upstream uses a byte-oriented
//    base_blob<256>), and the function belongs to the descriptor/wallet layer
//    (Taproot follow-up issue).
//  - The public key operations use the library provided `secp256k1_context_static`
//    (same as upstream), only nonce generation and key pair creation use the
//    fork's signing context (exposed via GetSecp256k1SignContext()).
//
// This file is not consensus relevant: it only provides the MuSig2 primitives.

#ifndef DEFI_MUSIG_H
#define DEFI_MUSIG_H

#include <pubkey.h>

#include <map>
#include <optional>
#include <vector>

class CKey;
struct secp256k1_musig_keyagg_cache;
class MuSig2SecNonceImpl;
struct secp256k1_musig_secnonce;

inline constexpr size_t MUSIG2_PUBNONCE_SIZE{66};

//! Compute the full aggregate pubkey from the given participant pubkeys in their current order.
//! Outputs the secp256k1_musig_keyagg_cache and validates that the computed aggregate pubkey matches an expected
//! aggregate pubkey. This is necessary for most MuSig2 operations.
std::optional<CPubKey> MuSig2AggregatePubkeys(const std::vector<CPubKey> &pubkeys,
                                              secp256k1_musig_keyagg_cache &keyagg_cache,
                                              const std::optional<CPubKey> &expected_aggregate);
std::optional<CPubKey> MuSig2AggregatePubkeys(const std::vector<CPubKey> &pubkeys);

/**
 * MuSig2SecNonce encapsulates a secret nonce in use in a MuSig2 signing session.
 * Since this nonce persists outside of libsecp256k1 signing code, we must handle
 * its construction and destruction ourselves.
 * The secret nonce must be kept a secret, otherwise the private key may be leaked.
 * As such, it needs to be treated in the same way that CKeys are treated.
 * So this class handles the secure allocation of the secp256k1_musig_secnonce object
 * that libsecp256k1 uses, and only gives out references to this object to avoid
 * any possibility of copies being made. Furthermore, objects of this class are not
 * copyable to avoid nonce reuse.
 */
class MuSig2SecNonce {
private:
    std::unique_ptr<MuSig2SecNonceImpl> m_impl;

public:
    MuSig2SecNonce();
    MuSig2SecNonce(MuSig2SecNonce &&) noexcept;
    MuSig2SecNonce &operator=(MuSig2SecNonce &&) noexcept;
    ~MuSig2SecNonce();

    // Delete copy constructors
    MuSig2SecNonce(const MuSig2SecNonce &) = delete;
    MuSig2SecNonce &operator=(const MuSig2SecNonce &) = delete;

    secp256k1_musig_secnonce *Get() const;
    void Invalidate();
    bool IsValid();
};

/**
 * Computes an arbitrary unique session ID to identify ongoing signing sessions.
 * It is the SHA256 of the signing (aggregate) pubkey, the participant pubkey, the sighash, and the pubnonce
 */
uint256 MuSig2SessionID(const CPubKey &script_pubkey,
                        const CPubKey &part_pubkey,
                        const uint256 &sighash,
                        const std::vector<uint8_t> &pubnonce);

std::vector<uint8_t> CreateMuSig2Nonce(MuSig2SecNonce &secnonce,
                                       const uint256 &sighash,
                                       const CKey &our_seckey,
                                       const CPubKey &aggregate_pubkey,
                                       const std::vector<CPubKey> &pubkeys);
std::optional<uint256> CreateMuSig2PartialSig(const uint256 &hash,
                                              const CKey &our_seckey,
                                              const CPubKey &aggregate_pubkey,
                                              const std::vector<CPubKey> &pubkeys,
                                              const std::map<CPubKey, std::vector<uint8_t>> &pubnonces,
                                              MuSig2SecNonce &secnonce,
                                              const std::vector<std::pair<uint256, bool>> &tweaks);
std::optional<std::vector<uint8_t>> CreateMuSig2AggregateSig(const std::vector<CPubKey> &participants,
                                                             const CPubKey &aggregate_pubkey,
                                                             const std::vector<std::pair<uint256, bool>> &tweaks,
                                                             const uint256 &sighash,
                                                             const std::map<CPubKey, std::vector<uint8_t>> &pubnonces,
                                                             const std::map<CPubKey, uint256> &partial_sigs);

#endif  // DEFI_MUSIG_H
