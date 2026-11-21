// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// MuSig2 (BIP 327) support, ported from Bitcoin Core (src/musig.cpp, v32.1).
//
// DFI-specific deviations from upstream:
//  - C++17 (upstream v32 is C++20): no std::span, no util::hex_literals.
//    * `uint256::data()` does not exist in the fork -> `uint256::begin()` is used.
//    * `GetStrongRandBytes(std::span)` does not exist in the fork ->
//      `GetStrongRandBytes(buf, num)` is used.
//    * `UCharCast()` does not exist in the fork -> `CKey::begin()` already
//      returns a `const unsigned char*`.
//  - `CreateMuSig2SyntheticXpub()` (BIP 328) is not ported (the fork's ChainCode
//    is a big-endian uint256, upstream uses a byte-oriented base_blob<256>). It
//    belongs to the descriptor/wallet layer (Taproot follow-up issue).
//  - `CPubKey::COMPRESSED_SIZE` is called `CPubKey::COMPRESSED_PUBLIC_KEY_SIZE`
//    in the fork.
//  - The upstream MIT license header is kept.
//
// This file is not consensus relevant: it only provides the MuSig2 primitives.

#include <hash.h>
#include <key.h>
#include <musig.h>
#include <random.h>
#include <support/allocators/secure.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>

static bool GetMuSig2KeyAggCache(const std::vector<CPubKey> &pubkeys, secp256k1_musig_keyagg_cache &keyagg_cache) {
    if (pubkeys.empty()) {
        return false;
    }

    // Parse the pubkeys
    std::vector<secp256k1_pubkey> secp_pubkeys;
    std::vector<const secp256k1_pubkey *> pubkey_ptrs;
    for (const CPubKey &pubkey : pubkeys) {
        if (!secp256k1_ec_pubkey_parse(
                secp256k1_context_static, &secp_pubkeys.emplace_back(), pubkey.data(), pubkey.size())) {
            return false;
        }
    }
    pubkey_ptrs.reserve(secp_pubkeys.size());
    for (const secp256k1_pubkey &p : secp_pubkeys) {
        pubkey_ptrs.push_back(&p);
    }

    // Aggregate the pubkeys
    if (!secp256k1_musig_pubkey_agg(
            secp256k1_context_static, nullptr, &keyagg_cache, pubkey_ptrs.data(), pubkey_ptrs.size())) {
        return false;
    }
    return true;
}

static std::optional<CPubKey> GetCPubKeyFromMuSig2KeyAggCache(secp256k1_musig_keyagg_cache &keyagg_cache) {
    // Get the plain aggregated pubkey
    secp256k1_pubkey agg_pubkey;
    if (!secp256k1_musig_pubkey_get(secp256k1_context_static, &agg_pubkey, &keyagg_cache)) {
        return std::nullopt;
    }

    // Turn into CPubKey
    unsigned char ser_agg_pubkey[CPubKey::COMPRESSED_PUBLIC_KEY_SIZE];
    size_t ser_agg_pubkey_len = CPubKey::COMPRESSED_PUBLIC_KEY_SIZE;
    secp256k1_ec_pubkey_serialize(
        secp256k1_context_static, ser_agg_pubkey, &ser_agg_pubkey_len, &agg_pubkey, SECP256K1_EC_COMPRESSED);
    return CPubKey(ser_agg_pubkey, ser_agg_pubkey + ser_agg_pubkey_len);
}

std::optional<CPubKey> MuSig2AggregatePubkeys(const std::vector<CPubKey> &pubkeys,
                                              secp256k1_musig_keyagg_cache &keyagg_cache,
                                              const std::optional<CPubKey> &expected_aggregate) {
    if (!GetMuSig2KeyAggCache(pubkeys, keyagg_cache)) {
        return std::nullopt;
    }
    std::optional<CPubKey> agg_key = GetCPubKeyFromMuSig2KeyAggCache(keyagg_cache);
    if (!agg_key.has_value()) {
        return std::nullopt;
    }
    if (expected_aggregate.has_value() && expected_aggregate != agg_key) {
        return std::nullopt;
    }
    return agg_key;
}

std::optional<CPubKey> MuSig2AggregatePubkeys(const std::vector<CPubKey> &pubkeys) {
    secp256k1_musig_keyagg_cache keyagg_cache;
    return MuSig2AggregatePubkeys(pubkeys, keyagg_cache, std::nullopt);
}

class MuSig2SecNonceImpl {
private:
    //! The actual secnonce itself
    secure_unique_ptr<secp256k1_musig_secnonce> m_nonce;

public:
    MuSig2SecNonceImpl()
        : m_nonce{make_secure_unique<secp256k1_musig_secnonce>()} {}

    // Delete copy constructors
    MuSig2SecNonceImpl(const MuSig2SecNonceImpl &) = delete;
    MuSig2SecNonceImpl &operator=(const MuSig2SecNonceImpl &) = delete;

    secp256k1_musig_secnonce *Get() const { return m_nonce.get(); }
    void Invalidate() { m_nonce.reset(); }
    bool IsValid() { return m_nonce != nullptr; }
};

MuSig2SecNonce::MuSig2SecNonce()
    : m_impl{std::make_unique<MuSig2SecNonceImpl>()} {}

MuSig2SecNonce::MuSig2SecNonce(MuSig2SecNonce &&) noexcept = default;
MuSig2SecNonce &MuSig2SecNonce::operator=(MuSig2SecNonce &&) noexcept = default;

MuSig2SecNonce::~MuSig2SecNonce() = default;

secp256k1_musig_secnonce *MuSig2SecNonce::Get() const {
    return m_impl->Get();
}

void MuSig2SecNonce::Invalidate() {
    return m_impl->Invalidate();
}

bool MuSig2SecNonce::IsValid() {
    return m_impl->IsValid();
}

uint256 MuSig2SessionID(const CPubKey &script_pubkey,
                        const CPubKey &part_pubkey,
                        const uint256 &sighash,
                        const std::vector<uint8_t> &pubnonce) {
    HashWriter hasher;
    hasher << script_pubkey << part_pubkey << sighash << pubnonce;
    return hasher.GetSHA256();
}

std::vector<uint8_t> CreateMuSig2Nonce(MuSig2SecNonce &secnonce,
                                       const uint256 &sighash,
                                       const CKey &our_seckey,
                                       const CPubKey &aggregate_pubkey,
                                       const std::vector<CPubKey> &pubkeys) {
    // DFI deviation: an invalidated secret nonce must never be handed to libsecp256k1
    // (a null pointer would abort the process via the illegal argument callback).
    if (!secnonce.IsValid()) {
        return {};
    }

    // Get the keyagg cache and aggregate pubkey
    secp256k1_musig_keyagg_cache keyagg_cache;
    if (!MuSig2AggregatePubkeys(pubkeys, keyagg_cache, aggregate_pubkey)) {
        return {};
    }

    // Parse participant pubkey
    CPubKey our_pubkey = our_seckey.GetPubKey();
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &pubkey, our_pubkey.data(), our_pubkey.size())) {
        return {};
    }

    // Generate randomness for nonce
    uint256 rand;
    GetStrongRandBytes(rand.begin(), 32);

    // Generate nonce
    secp256k1_musig_pubnonce pubnonce;
    if (!secp256k1_musig_nonce_gen(GetSecp256k1SignContext(),
                                   secnonce.Get(),
                                   &pubnonce,
                                   rand.begin(),
                                   our_seckey.begin(),
                                   &pubkey,
                                   sighash.begin(),
                                   &keyagg_cache,
                                   nullptr)) {
        return {};
    }

    // Serialize pubnonce
    std::vector<uint8_t> out;
    out.resize(MUSIG2_PUBNONCE_SIZE);
    if (!secp256k1_musig_pubnonce_serialize(secp256k1_context_static, out.data(), &pubnonce)) {
        return {};
    }

    return out;
}

std::optional<uint256> CreateMuSig2PartialSig(const uint256 &sighash,
                                              const CKey &our_seckey,
                                              const CPubKey &aggregate_pubkey,
                                              const std::vector<CPubKey> &pubkeys,
                                              const std::map<CPubKey, std::vector<uint8_t>> &pubnonces,
                                              MuSig2SecNonce &secnonce,
                                              const std::vector<std::pair<uint256, bool>> &tweaks) {
    // DFI deviation: an invalidated secret nonce must never be handed to libsecp256k1
    // (a null pointer would abort the process via the illegal argument callback).
    // This also makes the reuse of a secret nonce impossible.
    if (!secnonce.IsValid()) {
        return std::nullopt;
    }

    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(GetSecp256k1SignContext(), &keypair, our_seckey.begin())) {
        return std::nullopt;
    }

    // Get the keyagg cache and aggregate pubkey
    secp256k1_musig_keyagg_cache keyagg_cache;
    if (!MuSig2AggregatePubkeys(pubkeys, keyagg_cache, aggregate_pubkey)) {
        return std::nullopt;
    }

    // Check that there are enough pubnonces
    if (pubnonces.size() != pubkeys.size()) {
        return std::nullopt;
    }

    // Parse the pubnonces
    std::vector<std::pair<secp256k1_pubkey, secp256k1_musig_pubnonce>> signers_data;
    std::vector<const secp256k1_musig_pubnonce *> pubnonce_ptrs;
    std::optional<size_t> our_pubkey_idx;
    CPubKey our_pubkey = our_seckey.GetPubKey();
    for (const CPubKey &part_pk : pubkeys) {
        const auto &pn_it = pubnonces.find(part_pk);
        if (pn_it == pubnonces.end()) {
            return std::nullopt;
        }
        const std::vector<uint8_t> pubnonce = pn_it->second;
        if (pubnonce.size() != MUSIG2_PUBNONCE_SIZE) {
            return std::nullopt;
        }
        if (part_pk == our_pubkey) {
            our_pubkey_idx = signers_data.size();
        }

        auto &[secp_pk, secp_pn] = signers_data.emplace_back();

        if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &secp_pk, part_pk.data(), part_pk.size())) {
            return std::nullopt;
        }

        if (!secp256k1_musig_pubnonce_parse(secp256k1_context_static, &secp_pn, pubnonce.data())) {
            return std::nullopt;
        }
    }
    if (our_pubkey_idx == std::nullopt) {
        return std::nullopt;
    }
    pubnonce_ptrs.reserve(signers_data.size());
    for (auto &[_, pn] : signers_data) {
        pubnonce_ptrs.push_back(&pn);
    }

    // Aggregate nonces
    secp256k1_musig_aggnonce aggnonce;
    if (!secp256k1_musig_nonce_agg(secp256k1_context_static, &aggnonce, pubnonce_ptrs.data(), pubnonce_ptrs.size())) {
        return std::nullopt;
    }

    // Apply tweaks
    for (const auto &[tweak, xonly] : tweaks) {
        if (xonly) {
            if (!secp256k1_musig_pubkey_xonly_tweak_add(
                    secp256k1_context_static, nullptr, &keyagg_cache, tweak.begin())) {
                return std::nullopt;
            }
        } else if (!secp256k1_musig_pubkey_ec_tweak_add(
                       secp256k1_context_static, nullptr, &keyagg_cache, tweak.begin())) {
            return std::nullopt;
        }
    }

    // Create musig_session
    secp256k1_musig_session session;
    if (!secp256k1_musig_nonce_process(secp256k1_context_static, &session, &aggnonce, sighash.begin(), &keyagg_cache)) {
        return std::nullopt;
    }

    // Create partial signature
    secp256k1_musig_partial_sig psig;
    if (!secp256k1_musig_partial_sign(
            secp256k1_context_static, &psig, secnonce.Get(), &keypair, &keyagg_cache, &session)) {
        return std::nullopt;
    }
    // The secnonce must be deleted after signing to prevent nonce reuse.
    secnonce.Invalidate();

    // Verify partial signature
    if (!secp256k1_musig_partial_sig_verify(secp256k1_context_static,
                                            &psig,
                                            &(signers_data.at(*our_pubkey_idx).second),
                                            &(signers_data.at(*our_pubkey_idx).first),
                                            &keyagg_cache,
                                            &session)) {
        return std::nullopt;
    }

    // Serialize
    uint256 sig;
    if (!secp256k1_musig_partial_sig_serialize(secp256k1_context_static, sig.begin(), &psig)) {
        return std::nullopt;
    }

    return sig;
}

std::optional<std::vector<uint8_t>> CreateMuSig2AggregateSig(const std::vector<CPubKey> &part_pubkeys,
                                                             const CPubKey &aggregate_pubkey,
                                                             const std::vector<std::pair<uint256, bool>> &tweaks,
                                                             const uint256 &sighash,
                                                             const std::map<CPubKey, std::vector<uint8_t>> &pubnonces,
                                                             const std::map<CPubKey, uint256> &partial_sigs) {
    if (!part_pubkeys.size()) {
        return std::nullopt;
    }

    // Get the keyagg cache and aggregate pubkey
    secp256k1_musig_keyagg_cache keyagg_cache;
    if (!MuSig2AggregatePubkeys(part_pubkeys, keyagg_cache, aggregate_pubkey)) {
        return std::nullopt;
    }

    // Check if enough pubnonces and partial sigs
    if (pubnonces.size() != part_pubkeys.size()) {
        return std::nullopt;
    }
    if (partial_sigs.size() != part_pubkeys.size()) {
        return std::nullopt;
    }

    // Parse the pubnonces and partial sigs
    std::vector<std::tuple<secp256k1_pubkey, secp256k1_musig_pubnonce, secp256k1_musig_partial_sig>> signers_data;
    std::vector<const secp256k1_musig_pubnonce *> pubnonce_ptrs;
    std::vector<const secp256k1_musig_partial_sig *> partial_sig_ptrs;
    for (const CPubKey &part_pk : part_pubkeys) {
        const auto &pn_it = pubnonces.find(part_pk);
        if (pn_it == pubnonces.end()) {
            return std::nullopt;
        }
        const std::vector<uint8_t> pubnonce = pn_it->second;
        if (pubnonce.size() != MUSIG2_PUBNONCE_SIZE) {
            return std::nullopt;
        }
        const auto &it = partial_sigs.find(part_pk);
        if (it == partial_sigs.end()) {
            return std::nullopt;
        }
        const uint256 &partial_sig = it->second;

        auto &[secp_pk, secp_pn, secp_ps] = signers_data.emplace_back();

        if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &secp_pk, part_pk.data(), part_pk.size())) {
            return std::nullopt;
        }

        if (!secp256k1_musig_pubnonce_parse(secp256k1_context_static, &secp_pn, pubnonce.data())) {
            return std::nullopt;
        }

        if (!secp256k1_musig_partial_sig_parse(secp256k1_context_static, &secp_ps, partial_sig.begin())) {
            return std::nullopt;
        }
    }
    pubnonce_ptrs.reserve(signers_data.size());
    partial_sig_ptrs.reserve(signers_data.size());
    for (auto &[_, pn, ps] : signers_data) {
        pubnonce_ptrs.push_back(&pn);
        partial_sig_ptrs.push_back(&ps);
    }

    // Aggregate nonces
    secp256k1_musig_aggnonce aggnonce;
    if (!secp256k1_musig_nonce_agg(secp256k1_context_static, &aggnonce, pubnonce_ptrs.data(), pubnonce_ptrs.size())) {
        return std::nullopt;
    }

    // Apply tweaks
    for (const auto &[tweak, xonly] : tweaks) {
        if (xonly) {
            if (!secp256k1_musig_pubkey_xonly_tweak_add(
                    secp256k1_context_static, nullptr, &keyagg_cache, tweak.begin())) {
                return std::nullopt;
            }
        } else if (!secp256k1_musig_pubkey_ec_tweak_add(
                       secp256k1_context_static, nullptr, &keyagg_cache, tweak.begin())) {
            return std::nullopt;
        }
    }

    // Create musig_session
    secp256k1_musig_session session;
    if (!secp256k1_musig_nonce_process(secp256k1_context_static, &session, &aggnonce, sighash.begin(), &keyagg_cache)) {
        return std::nullopt;
    }

    // Verify partial sigs
    for (const auto &[pk, pb, ps] : signers_data) {
        if (!secp256k1_musig_partial_sig_verify(secp256k1_context_static, &ps, &pb, &pk, &keyagg_cache, &session)) {
            return std::nullopt;
        }
    }

    // Aggregate partial sigs
    std::vector<uint8_t> sig;
    sig.resize(64);
    if (!secp256k1_musig_partial_sig_agg(
            secp256k1_context_static, sig.data(), &session, partial_sig_ptrs.data(), partial_sig_ptrs.size())) {
        return std::nullopt;
    }

    return sig;
}
