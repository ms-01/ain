// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Unit tests for the MuSig2 (BIP 327) core library.

#include <musig.h>

#include <crypto/sha256.h>
#include <key.h>
#include <pubkey.h>
#include <random.h>
#include <uint256.h>

#include <test/setup_common.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

namespace {
    //! Key aggregation test vectors, taken from the libsecp256k1 musig module test
    //! vectors (src/secp256k1/src/modules/musig/vectors.h), which are generated from
    //! the BIP-327 test vectors.
    const unsigned char MUSIG_VECTOR_PUBKEYS[7][33] = {
        // [0]
        0x02,
        0xF9,
        0x30,
        0x8A,
        0x01,
        0x92,
        0x58,
        0xC3,
        0x10,
        0x49,
        0x34,
        0x4F,
        0x85,
        0xF8,
        0x9D,
        0x52,
        0x29,
        0xB5,
        0x31,
        0xC8,
        0x45,
        0x83,
        0x6F,
        0x99,
        0xB0,
        0x86,
        0x01,
        0xF1,
        0x13,
        0xBC,
        0xE0,
        0x36,
        0xF9,
        // [1]
        0x03,
        0xDF,
        0xF1,
        0xD7,
        0x7F,
        0x2A,
        0x67,
        0x1C,
        0x5F,
        0x36,
        0x18,
        0x37,
        0x26,
        0xDB,
        0x23,
        0x41,
        0xBE,
        0x58,
        0xFE,
        0xAE,
        0x1D,
        0xA2,
        0xDE,
        0xCE,
        0xD8,
        0x43,
        0x24,
        0x0F,
        0x7B,
        0x50,
        0x2B,
        0xA6,
        0x59,
        // [2]
        0x02,
        0x35,
        0x90,
        0xA9,
        0x4E,
        0x76,
        0x8F,
        0x8E,
        0x18,
        0x15,
        0xC2,
        0xF2,
        0x4B,
        0x4D,
        0x80,
        0xA8,
        0xE3,
        0x14,
        0x93,
        0x16,
        0xC3,
        0x51,
        0x8C,
        0xE7,
        0xB7,
        0xAD,
        0x33,
        0x83,
        0x68,
        0xD0,
        0x38,
        0xCA,
        0x66,
        // [3]
        0x02,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x05,
        // [4]
        0x02,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFE,
        0xFF,
        0xFF,
        0xFC,
        0x30,
        // [5]
        0x04,
        0xF9,
        0x30,
        0x8A,
        0x01,
        0x92,
        0x58,
        0xC3,
        0x10,
        0x49,
        0x34,
        0x4F,
        0x85,
        0xF8,
        0x9D,
        0x52,
        0x29,
        0xB5,
        0x31,
        0xC8,
        0x45,
        0x83,
        0x6F,
        0x99,
        0xB0,
        0x86,
        0x01,
        0xF1,
        0x13,
        0xBC,
        0xE0,
        0x36,
        0xF9,
        // [6]
        0x03,
        0x93,
        0x5F,
        0x97,
        0x2D,
        0xA0,
        0x13,
        0xF8,
        0x0A,
        0xE0,
        0x11,
        0x89,
        0x0F,
        0xA8,
        0x9B,
        0x67,
        0xA2,
        0x7B,
        0x7B,
        0xE6,
        0xCC,
        0xB2,
        0x4D,
        0x32,
        0x74,
        0xD1,
        0x8B,
        0x2D,
        0x40,
        0x67,
        0xF2,
        0x61,
        0xA9,
    };

    //! Expected x-only aggregate pubkeys for the key aggregation vectors above.
    const unsigned char MUSIG_VECTOR_AGG_0_1_2[32] = {
        0x90, 0x53, 0x9E, 0xED, 0xE5, 0x65, 0xF5, 0xD0, 0x54, 0xF3, 0x2C, 0xC0, 0xC2, 0x20, 0x12, 0x68,
        0x89, 0xED, 0x1E, 0x5D, 0x19, 0x3B, 0xAF, 0x15, 0xAE, 0xF3, 0x44, 0xFE, 0x59, 0xD4, 0x61, 0x0C,
    };

    const unsigned char MUSIG_VECTOR_AGG_2_1_0[32] = {
        0x62, 0x04, 0xDE, 0x8B, 0x08, 0x34, 0x26, 0xDC, 0x6E, 0xAF, 0x95, 0x02, 0xD2, 0x70, 0x24, 0xD5,
        0x3F, 0xC8, 0x26, 0xBF, 0x7D, 0x20, 0x12, 0x14, 0x8A, 0x05, 0x75, 0x43, 0x5D, 0xF5, 0x4B, 0x2B,
    };

    const unsigned char MUSIG_VECTOR_AGG_0_0_0[32] = {
        0xB4, 0x36, 0xE3, 0xBA, 0xD6, 0x2B, 0x8C, 0xD4, 0x09, 0x96, 0x9A, 0x22, 0x47, 0x31, 0xC1, 0x93,
        0xD0, 0x51, 0x16, 0x2D, 0x8C, 0x5A, 0xE8, 0xB1, 0x09, 0x30, 0x61, 0x27, 0xDA, 0x3A, 0xA9, 0x35,
    };

    const unsigned char MUSIG_VECTOR_AGG_0_0_1_1[32] = {
        0x69, 0xBC, 0x22, 0xBF, 0xA5, 0xD1, 0x06, 0x30, 0x6E, 0x48, 0xA2, 0x06, 0x79, 0xDE, 0x1D, 0x73,
        0x89, 0x38, 0x61, 0x24, 0xD0, 0x75, 0x71, 0xD0, 0xD8, 0x72, 0x68, 0x60, 0x28, 0xC2, 0x6A, 0x3E,
    };

    CPubKey VectorPubKey(size_t idx) {
        return CPubKey(MUSIG_VECTOR_PUBKEYS[idx], MUSIG_VECTOR_PUBKEYS[idx] + 33);
    }

    //! The x-only (BIP-340) serialization of a compressed pubkey.
    std::vector<unsigned char> XOnly(const CPubKey &pubkey) {
        return std::vector<unsigned char>(pubkey.data() + 1, pubkey.data() + 33);
    }

    uint256 Sha256(const std::vector<unsigned char> &data) {
        CSHA256 sha;
        sha.Write(data.data(), data.size());
        uint256 result;
        sha.Finalize(result.begin());
        return result;
    }
}  // namespace

BOOST_FIXTURE_TEST_SUITE(musig_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(key_aggregation_vectors) {
    // 3-of-3 participant set, in the order given by the test vector.
    const std::vector<CPubKey> pubkeys_012{VectorPubKey(0), VectorPubKey(1), VectorPubKey(2)};
    const std::vector<CPubKey> pubkeys_210{VectorPubKey(2), VectorPubKey(1), VectorPubKey(0)};
    const std::vector<CPubKey> pubkeys_000{VectorPubKey(0), VectorPubKey(0), VectorPubKey(0)};
    const std::vector<CPubKey> pubkeys_0011{VectorPubKey(0), VectorPubKey(0), VectorPubKey(1), VectorPubKey(1)};

    const std::optional<CPubKey> agg_012 = MuSig2AggregatePubkeys(pubkeys_012);
    BOOST_REQUIRE(agg_012.has_value());
    BOOST_CHECK(XOnly(*agg_012) == std::vector<unsigned char>(MUSIG_VECTOR_AGG_0_1_2, MUSIG_VECTOR_AGG_0_1_2 + 32));

    const std::optional<CPubKey> agg_210 = MuSig2AggregatePubkeys(pubkeys_210);
    BOOST_REQUIRE(agg_210.has_value());
    BOOST_CHECK(XOnly(*agg_210) == std::vector<unsigned char>(MUSIG_VECTOR_AGG_2_1_0, MUSIG_VECTOR_AGG_2_1_0 + 32));

    const std::optional<CPubKey> agg_000 = MuSig2AggregatePubkeys(pubkeys_000);
    BOOST_REQUIRE(agg_000.has_value());
    BOOST_CHECK(XOnly(*agg_000) == std::vector<unsigned char>(MUSIG_VECTOR_AGG_0_0_0, MUSIG_VECTOR_AGG_0_0_0 + 32));

    const std::optional<CPubKey> agg_0011 = MuSig2AggregatePubkeys(pubkeys_0011);
    BOOST_REQUIRE(agg_0011.has_value());
    BOOST_CHECK(XOnly(*agg_0011) ==
                std::vector<unsigned char>(MUSIG_VECTOR_AGG_0_0_1_1, MUSIG_VECTOR_AGG_0_0_1_1 + 32));

    // The aggregation is order sensitive.
    BOOST_CHECK(*agg_012 != *agg_210);
}

BOOST_AUTO_TEST_CASE(key_aggregation_invalid_pubkeys) {
    // Pubkey 3 is not a valid curve point, pubkey 4 is not a valid curve point and
    // pubkey 5 is not a valid compressed pubkey.
    BOOST_CHECK(!MuSig2AggregatePubkeys({VectorPubKey(0), VectorPubKey(3)}).has_value());
    BOOST_CHECK(!MuSig2AggregatePubkeys({VectorPubKey(0), VectorPubKey(4)}).has_value());
    BOOST_CHECK(!MuSig2AggregatePubkeys({VectorPubKey(5), VectorPubKey(0)}).has_value());

    // An empty participant set is rejected.
    BOOST_CHECK(!MuSig2AggregatePubkeys({}).has_value());
}

BOOST_AUTO_TEST_CASE(key_aggregation_expected_aggregate) {
    const std::vector<CPubKey> pubkeys{VectorPubKey(0), VectorPubKey(1), VectorPubKey(2)};
    const std::optional<CPubKey> agg = MuSig2AggregatePubkeys(pubkeys);
    BOOST_REQUIRE(agg.has_value());

    secp256k1_musig_keyagg_cache cache;
    // The expected aggregate matches -> success.
    BOOST_CHECK(MuSig2AggregatePubkeys(pubkeys, cache, agg).has_value());

    // The expected aggregate does not match -> failure.
    secp256k1_musig_keyagg_cache cache2;
    BOOST_CHECK(!MuSig2AggregatePubkeys(pubkeys, cache2, VectorPubKey(0)).has_value());
}

BOOST_AUTO_TEST_CASE(full_signing_session) {
    // Three participants sign the same message.
    std::vector<CKey> seckeys;
    std::vector<CPubKey> pubkeys;
    for (int i = 0; i < 3; ++i) {
        CKey seckey;
        seckey.MakeNewKey(true);
        seckeys.push_back(seckey);
        pubkeys.push_back(seckey.GetPubKey());
    }

    const std::optional<CPubKey> aggregate = MuSig2AggregatePubkeys(pubkeys);
    BOOST_REQUIRE(aggregate.has_value());

    const uint256 sighash = GetRandHash();

    // Every participant creates a secret nonce and publishes the public nonce.
    std::vector<MuSig2SecNonce> secnonces(3);
    std::map<CPubKey, std::vector<uint8_t>> pubnonces;
    for (int i = 0; i < 3; ++i) {
        BOOST_REQUIRE(secnonces[i].IsValid());
        const std::vector<uint8_t> pubnonce = CreateMuSig2Nonce(secnonces[i], sighash, seckeys[i], *aggregate, pubkeys);
        BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);
        pubnonces.emplace(pubkeys[i], pubnonce);
    }
    BOOST_CHECK(pubnonces.size() == 3);

    // Every participant creates a partial signature.
    std::map<CPubKey, uint256> partial_sigs;
    for (int i = 0; i < 3; ++i) {
        const std::optional<uint256> partial_sig =
            CreateMuSig2PartialSig(sighash, seckeys[i], *aggregate, pubkeys, pubnonces, secnonces[i], {});
        BOOST_REQUIRE(partial_sig.has_value());
        // The secret nonce must be invalidated after use to prevent nonce reuse.
        BOOST_CHECK(!secnonces[i].IsValid());
        partial_sigs.emplace(pubkeys[i], *partial_sig);
    }

    // The aggregate signature is a valid BIP-340 Schnorr signature over the
    // x-only aggregate pubkey.
    const std::optional<std::vector<uint8_t>> agg_sig =
        CreateMuSig2AggregateSig(pubkeys, *aggregate, {}, sighash, pubnonces, partial_sigs);
    BOOST_REQUIRE(agg_sig.has_value());
    BOOST_REQUIRE(agg_sig->size() == 64);

    const std::vector<unsigned char> xonly = XOnly(*aggregate);
    secp256k1_xonly_pubkey xonly_pubkey;
    BOOST_REQUIRE(secp256k1_xonly_pubkey_parse(secp256k1_context_static, &xonly_pubkey, xonly.data()) == 1);
    BOOST_CHECK(secp256k1_schnorrsig_verify(
                    secp256k1_context_static, agg_sig->data(), sighash.begin(), 32, &xonly_pubkey) == 1);

    // A different message does not verify with the same signature.
    const uint256 other_sighash = GetRandHash();
    BOOST_CHECK(secp256k1_schnorrsig_verify(
                    secp256k1_context_static, agg_sig->data(), other_sighash.begin(), 32, &xonly_pubkey) == 0);
}

BOOST_AUTO_TEST_CASE(negative_cases) {
    std::vector<CKey> seckeys;
    std::vector<CPubKey> pubkeys;
    for (int i = 0; i < 3; ++i) {
        CKey seckey;
        seckey.MakeNewKey(true);
        seckeys.push_back(seckey);
        pubkeys.push_back(seckey.GetPubKey());
    }

    const std::optional<CPubKey> aggregate = MuSig2AggregatePubkeys(pubkeys);
    BOOST_REQUIRE(aggregate.has_value());
    const uint256 sighash = GetRandHash();

    // A wrong aggregate pubkey is rejected.
    {
        MuSig2SecNonce secnonce;
        BOOST_CHECK(CreateMuSig2Nonce(secnonce, sighash, seckeys[0], pubkeys[0], pubkeys).empty());
    }

    // A missing pubnonce is rejected.
    {
        MuSig2SecNonce secnonce;
        const std::vector<uint8_t> pubnonce = CreateMuSig2Nonce(secnonce, sighash, seckeys[0], *aggregate, pubkeys);
        BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);

        std::map<CPubKey, std::vector<uint8_t>> missing;
        missing.emplace(pubkeys[0], pubnonce);
        BOOST_CHECK(
            !CreateMuSig2PartialSig(sighash, seckeys[0], *aggregate, pubkeys, missing, secnonce, {}).has_value());
    }

    // A tampered pubnonce is rejected.
    {
        MuSig2SecNonce secnonce;
        const std::vector<uint8_t> pubnonce = CreateMuSig2Nonce(secnonce, sighash, seckeys[0], *aggregate, pubkeys);
        BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);

        std::map<CPubKey, std::vector<uint8_t>> tampered;
        std::vector<uint8_t> bad_pubnonce = pubnonce;
        bad_pubnonce[0] ^= 0x01;
        tampered.emplace(pubkeys[0], bad_pubnonce);
        BOOST_CHECK(
            !CreateMuSig2PartialSig(sighash, seckeys[0], *aggregate, pubkeys, tampered, secnonce, {}).has_value());
    }

    // A partial signature of a non-participant is rejected.
    {
        std::vector<MuSig2SecNonce> secnonces(3);
        std::map<CPubKey, std::vector<uint8_t>> pubnonces;
        for (int i = 0; i < 3; ++i) {
            const std::vector<uint8_t> pubnonce =
                CreateMuSig2Nonce(secnonces[i], sighash, seckeys[i], *aggregate, pubkeys);
            BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);
            pubnonces.emplace(pubkeys[i], pubnonce);
        }

        CKey outsider;
        outsider.MakeNewKey(true);
        const CPubKey outsider_pubkey = outsider.GetPubKey();

        std::map<CPubKey, uint256> partial_sigs;
        for (int i = 0; i < 3; ++i) {
            const std::optional<uint256> partial_sig =
                CreateMuSig2PartialSig(sighash, seckeys[i], *aggregate, pubkeys, pubnonces, secnonces[i], {});
            BOOST_REQUIRE(partial_sig.has_value());
            partial_sigs.emplace(pubkeys[i], *partial_sig);
        }
        // Replace the partial signature of the last participant with one of a
        // non-participant -> the aggregate signature cannot be created.
        std::map<CPubKey, uint256> foreign_sigs = partial_sigs;
        foreign_sigs.erase(pubkeys[2]);
        foreign_sigs.emplace(outsider_pubkey, partial_sigs.at(pubkeys[2]));
        BOOST_CHECK(!CreateMuSig2AggregateSig(pubkeys, *aggregate, {}, sighash, pubnonces, foreign_sigs).has_value());
    }

    // A tampered partial signature is rejected.
    {
        std::vector<MuSig2SecNonce> secnonces(3);
        std::map<CPubKey, std::vector<uint8_t>> pubnonces;
        for (int i = 0; i < 3; ++i) {
            const std::vector<uint8_t> pubnonce =
                CreateMuSig2Nonce(secnonces[i], sighash, seckeys[i], *aggregate, pubkeys);
            BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);
            pubnonces.emplace(pubkeys[i], pubnonce);
        }

        std::map<CPubKey, uint256> partial_sigs;
        for (int i = 0; i < 3; ++i) {
            const std::optional<uint256> partial_sig =
                CreateMuSig2PartialSig(sighash, seckeys[i], *aggregate, pubkeys, pubnonces, secnonces[i], {});
            BOOST_REQUIRE(partial_sig.has_value());
            partial_sigs.emplace(pubkeys[i], *partial_sig);
        }
        uint256 tampered_sig = partial_sigs.at(pubkeys[1]);
        tampered_sig.begin()[0] ^= 0x01;
        partial_sigs[pubkeys[1]] = tampered_sig;
        BOOST_CHECK(!CreateMuSig2AggregateSig(pubkeys, *aggregate, {}, sighash, pubnonces, partial_sigs).has_value());
    }
}

BOOST_AUTO_TEST_CASE(secret_nonce_reuse_rejected) {
    CKey seckey;
    seckey.MakeNewKey(true);
    const CPubKey pubkey = seckey.GetPubKey();
    const std::vector<CPubKey> pubkeys{pubkey};
    const std::optional<CPubKey> aggregate = MuSig2AggregatePubkeys(pubkeys);
    BOOST_REQUIRE(aggregate.has_value());
    const uint256 sighash = GetRandHash();

    MuSig2SecNonce secnonce;
    BOOST_CHECK(secnonce.IsValid());
    const std::vector<uint8_t> pubnonce = CreateMuSig2Nonce(secnonce, sighash, seckey, *aggregate, pubkeys);
    BOOST_REQUIRE(pubnonce.size() == MUSIG2_PUBNONCE_SIZE);

    std::map<CPubKey, std::vector<uint8_t>> pubnonces;
    pubnonces.emplace(pubkey, pubnonce);

    BOOST_CHECK(CreateMuSig2PartialSig(sighash, seckey, *aggregate, pubkeys, pubnonces, secnonce, {}).has_value());

    // The secret nonce is invalidated after the first partial signature, so it
    // cannot be used a second time.
    BOOST_CHECK(!secnonce.IsValid());
    BOOST_CHECK(secnonce.Get() == nullptr);
    BOOST_CHECK(!CreateMuSig2PartialSig(sighash, seckey, *aggregate, pubkeys, pubnonces, secnonce, {}).has_value());
    BOOST_CHECK(CreateMuSig2Nonce(secnonce, sighash, seckey, *aggregate, pubkeys).empty());
}

BOOST_AUTO_TEST_CASE(session_id) {
    CKey seckey;
    seckey.MakeNewKey(true);
    const CPubKey pubkey = seckey.GetPubKey();
    CKey other_seckey;
    other_seckey.MakeNewKey(true);
    const CPubKey other_pubkey = other_seckey.GetPubKey();
    const uint256 sighash = GetRandHash();
    const uint256 other_sighash = GetRandHash();
    const std::vector<uint8_t> pubnonce(MUSIG2_PUBNONCE_SIZE, 0x42);
    const std::vector<uint8_t> other_pubnonce(MUSIG2_PUBNONCE_SIZE, 0x43);

    // The session id is deterministic.
    BOOST_CHECK(MuSig2SessionID(pubkey, other_pubkey, sighash, pubnonce) ==
                MuSig2SessionID(pubkey, other_pubkey, sighash, pubnonce));

    // ... and differs for differing inputs.
    BOOST_CHECK(MuSig2SessionID(pubkey, other_pubkey, sighash, pubnonce) !=
                MuSig2SessionID(pubkey, other_pubkey, other_sighash, pubnonce));
    BOOST_CHECK(MuSig2SessionID(pubkey, other_pubkey, sighash, pubnonce) !=
                MuSig2SessionID(pubkey, other_pubkey, sighash, other_pubnonce));
    BOOST_CHECK(MuSig2SessionID(pubkey, other_pubkey, sighash, pubnonce) !=
                MuSig2SessionID(other_pubkey, pubkey, sighash, pubnonce));

    // The session id is the single SHA-256 of the serialized
    // (aggregate pubkey, participant pubkey, sighash, pubnonce) tuple.
    // The pubkeys are serialized with their compact size prefix, the sighash is
    // serialized as 32 raw bytes and the pubnonce is serialized with its compact
    // size prefix.
    std::vector<unsigned char> preimage;
    preimage.push_back(33);
    preimage.insert(preimage.end(), MUSIG_VECTOR_PUBKEYS[0], MUSIG_VECTOR_PUBKEYS[0] + 33);
    preimage.push_back(33);
    preimage.insert(preimage.end(), MUSIG_VECTOR_PUBKEYS[1], MUSIG_VECTOR_PUBKEYS[1] + 33);
    preimage.insert(preimage.end(), sighash.begin(), sighash.end());
    preimage.push_back(MUSIG2_PUBNONCE_SIZE);
    preimage.insert(preimage.end(), pubnonce.begin(), pubnonce.end());
    BOOST_CHECK(MuSig2SessionID(VectorPubKey(0), VectorPubKey(1), sighash, pubnonce) == Sha256(preimage));
}

BOOST_AUTO_TEST_SUITE_END()
