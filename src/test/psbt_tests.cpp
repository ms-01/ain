// Copyright (c) 2019-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <psbt.h>

#include <key.h>
#include <test/setup_common.h>

#include <streams.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(psbt_tests, BasicTestingSetup)

static CMutableTransaction CreateTestTx()
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.vin.emplace_back(COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000001"), 0));
    tx.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;
    tx.vout.emplace_back(1000, CScript() << OP_TRUE);
    tx.vout.emplace_back(2000, CScript() << OP_TRUE);
    tx.nLockTime = 0;
    return tx;
}

static std::vector<unsigned char> SerializePSBT(const PartiallySignedTransaction& psbt)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbt;
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

static PartiallySignedTransaction ParsePSBT(const std::vector<unsigned char>& data)
{
    PartiallySignedTransaction psbt;
    CDataStream ss(data, SER_NETWORK, PROTOCOL_VERSION);
    ss >> psbt;
    return psbt;
}

/**
 * A PSBTv0 must be serialized exactly as before the PSBTv2 support was added.
 */
BOOST_AUTO_TEST_CASE(psbt_v0_serialization_unchanged)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx);
    psbt.inputs[0].witness_utxo = tx.vout[0];
    psbt.inputs[0].redeem_script = CScript() << OP_TRUE;

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);

    // The magic bytes and the unsigned transaction key must be the first bytes
    BOOST_CHECK_EQUAL(serialized[0], 'p');
    BOOST_CHECK_EQUAL(serialized[1], 's');
    BOOST_CHECK_EQUAL(serialized[2], 'b');
    BOOST_CHECK_EQUAL(serialized[3], 't');
    BOOST_CHECK_EQUAL(serialized[4], 0xff);
    BOOST_CHECK_EQUAL(serialized[5], 0x01); // key length
    BOOST_CHECK_EQUAL(serialized[6], PSBT_GLOBAL_UNSIGNED_TX);

    // A PSBTv0 must not contain any of the PSBTv2 global keys
    PartiallySignedTransaction parsed = ParsePSBT(serialized);
    BOOST_CHECK_EQUAL(parsed.GetVersion(), 0U);
    BOOST_CHECK(parsed.tx.has_value());
    BOOST_CHECK_EQUAL(parsed.tx->GetHash(), tx.GetHash());
    BOOST_CHECK(parsed.m_xpubs.empty());
    BOOST_CHECK(parsed.proprietary.empty());

    // Serializing it again must result in the very same bytes
    BOOST_CHECK(SerializePSBT(parsed) == serialized);
}

/**
 * A PSBTv2 must survive a round trip, including the output data and the DFI token id.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_roundtrip)
{
    CMutableTransaction tx = CreateTestTx();
    // Make it a token transaction
    tx.vout[0].nTokenId = DCT_ID{42};
    tx.nVersion = CTransaction::TOKENS_MIN_VERSION;

    PartiallySignedTransaction psbt(tx, 2);
    BOOST_CHECK_EQUAL(psbt.GetVersion(), 2U);
    BOOST_CHECK_EQUAL(psbt.tx_version, static_cast<uint32_t>(CTransaction::TOKENS_MIN_VERSION));

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    BOOST_CHECK_EQUAL(parsed.GetVersion(), 2U);
    BOOST_CHECK(parsed.tx.has_value());
    BOOST_CHECK_EQUAL(parsed.tx->GetHash(), tx.GetHash());
    BOOST_CHECK_EQUAL(parsed.tx->nVersion, tx.nVersion);
    BOOST_CHECK_EQUAL(parsed.tx->nLockTime, tx.nLockTime);
    BOOST_CHECK_EQUAL(parsed.inputs.size(), 1U);
    BOOST_CHECK_EQUAL(parsed.outputs.size(), 2U);

    // The output data must be preserved
    BOOST_CHECK_EQUAL(parsed.outputs[0].amount, tx.vout[0].nValue);
    BOOST_CHECK(parsed.outputs[0].script == tx.vout[0].scriptPubKey);
    BOOST_CHECK_EQUAL(parsed.outputs[0].GetTokenId().v, tx.vout[0].nTokenId.v);
    BOOST_CHECK_EQUAL(parsed.outputs[1].GetTokenId().v, 0U);

    // The input data must be preserved
    BOOST_CHECK_EQUAL(parsed.inputs[0].prev_txid, tx.vin[0].prevout.hash);
    BOOST_CHECK_EQUAL(parsed.inputs[0].prev_out, tx.vin[0].prevout.n);
    BOOST_CHECK(parsed.inputs[0].sequence.has_value());
    BOOST_CHECK_EQUAL(*parsed.inputs[0].sequence, tx.vin[0].nSequence);

    // The PSBTv2 does not contain the unsigned transaction
    const std::vector<unsigned char> reserialized = SerializePSBT(parsed);
    BOOST_CHECK(reserialized == serialized);
}

/**
 * The transaction of a PSBTv2 is rebuilt from the global and the per input/output data.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_rebuild_transaction)
{
    CMutableTransaction tx = CreateTestTx();
    tx.vout[0].nTokenId = DCT_ID{7};
    tx.nVersion = CTransaction::TOKENS_MIN_VERSION;
    tx.nLockTime = 500;

    PartiallySignedTransaction psbt(tx, 2);
    // Remove the transaction to force the rebuild
    psbt.tx.reset();

    const auto unsigned_tx = psbt.GetUnsignedTx();
    BOOST_REQUIRE(unsigned_tx.has_value());
    BOOST_CHECK_EQUAL(unsigned_tx->GetHash(), tx.GetHash());
    BOOST_CHECK_EQUAL(unsigned_tx->nVersion, tx.nVersion);
    BOOST_CHECK_EQUAL(unsigned_tx->nLockTime, tx.nLockTime);
    BOOST_CHECK_EQUAL(unsigned_tx->vout[0].nTokenId.v, 7U);
}

/**
 * A PSBTv2 without the DFI specific records must be parsable, analyzable and signable.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_without_defi_records)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx, 2);

    // Remove all DFI specific records
    for (PSBTInput& input : psbt.inputs) {
        input.proprietary.clear();
    }
    for (PSBTOutput& output : psbt.outputs) {
        output.proprietary.clear();
    }

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    BOOST_CHECK_EQUAL(parsed.GetVersion(), 2U);
    BOOST_CHECK(parsed.tx.has_value());
    BOOST_CHECK_EQUAL(parsed.tx->GetHash(), tx.GetHash());
    // Without the DFI records there is no token information
    BOOST_CHECK_EQUAL(parsed.outputs[0].GetTokenId().v, 0U);
}

/**
 * Proprietary records of other vendors must survive a round trip.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_unknown_proprietary_record)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx, 2);

    // A record of another vendor: 0xFC || CompactSize(subtype) || identifier
    std::vector<unsigned char> foreign_key;
    foreign_key.push_back(PSBT_PROPRIETARY_KEY_TYPE);
    foreign_key.push_back(0x07);
    const std::string identifier = "other";
    foreign_key.insert(foreign_key.end(), identifier.begin(), identifier.end());
    const std::vector<unsigned char> foreign_value{0xde, 0xad, 0xbe, 0xef};

    psbt.proprietary[foreign_key] = foreign_value;
    psbt.inputs[0].proprietary[foreign_key] = foreign_value;
    psbt.outputs[0].proprietary[foreign_key] = foreign_value;

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    BOOST_CHECK_EQUAL(parsed.proprietary.count(foreign_key), 1U);
    BOOST_CHECK(parsed.proprietary.at(foreign_key) == foreign_value);
    BOOST_CHECK_EQUAL(parsed.inputs[0].proprietary.count(foreign_key), 1U);
    BOOST_CHECK(parsed.inputs[0].proprietary.at(foreign_key) == foreign_value);
    BOOST_CHECK_EQUAL(parsed.outputs[0].proprietary.count(foreign_key), 1U);
    BOOST_CHECK(parsed.outputs[0].proprietary.at(foreign_key) == foreign_value);

    // The record must not change the transaction
    BOOST_CHECK_EQUAL(parsed.tx->GetHash(), tx.GetHash());
}

/**
 * PSBTs of different versions must not be merged.
 */
BOOST_AUTO_TEST_CASE(psbt_merge_only_same_version)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction v0(tx);
    PartiallySignedTransaction v2(tx, 2);

    BOOST_CHECK(!v0.Merge(v2));
    BOOST_CHECK(!v2.Merge(v0));

    PartiallySignedTransaction v2b(tx, 2);
    BOOST_CHECK(v2.Merge(v2b));
}

/**
 * The PSBTv2 only keys are unknown keys in a PSBTv0 and must not change its byte stream.
 */
BOOST_AUTO_TEST_CASE(psbt_v0_keeps_v2_keys_as_unknown)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx);

    // A PSBTv2 only key (the previous txid) in the input map
    std::vector<unsigned char> key;
    key.push_back(PSBT_IN_PREVIOUS_TXID);
    std::vector<unsigned char> value(32, 0x11);
    psbt.inputs[0].unknown[key] = value;

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    // The key is an unknown key in a PSBTv0 and must be preserved
    BOOST_CHECK_EQUAL(parsed.GetVersion(), 0U);
    BOOST_CHECK_EQUAL(parsed.inputs[0].unknown.count(key), 1U);
    BOOST_CHECK(parsed.inputs[0].unknown.at(key) == value);
    BOOST_CHECK(SerializePSBT(parsed) == serialized);
}

/**
 * Only the PSBT versions 0 and 2 are supported.
 */
BOOST_AUTO_TEST_CASE(psbt_invalid_version)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx, 2);
    const std::vector<unsigned char> serialized = SerializePSBT(psbt);

    // Replace the version (2) with an unsupported version (1)
    std::vector<unsigned char> modified = serialized;
    bool found = false;
    for (size_t i = 0; i + 5 < modified.size(); ++i) {
        // key length 1, key type PSBT_GLOBAL_VERSION (0xFB), value length 4, value
        if (modified[i] == 0x01 && modified[i + 1] == PSBT_GLOBAL_VERSION && modified[i + 2] == 0x04) {
            modified[i + 3] = 0x01; // version 1
            found = true;
            break;
        }
    }
    BOOST_REQUIRE(found);

    bool threw = false;
    try {
        ParsePSBT(modified);
    } catch (const std::exception&) {
        threw = true;
    }
    BOOST_CHECK(threw);
}

/**
 * A PSBTv0 can be converted into a PSBTv2 and back.
 */
BOOST_AUTO_TEST_CASE(psbt_v0_to_v2_conversion)
{
    CMutableTransaction tx = CreateTestTx();
    tx.vout[1].nTokenId = DCT_ID{11};
    tx.nVersion = CTransaction::TOKENS_MIN_VERSION;

    PartiallySignedTransaction v0(tx);
    const std::vector<unsigned char> serialized_v0 = SerializePSBT(v0);

    // Convert it into a PSBTv2
    PartiallySignedTransaction v2(tx, 2);
    const std::vector<unsigned char> serialized_v2 = SerializePSBT(v2);
    BOOST_CHECK(serialized_v0 != serialized_v2);

    // The PSBTv2 contains the same transaction
    PartiallySignedTransaction parsed_v2 = ParsePSBT(serialized_v2);
    BOOST_CHECK_EQUAL(parsed_v2.GetVersion(), 2U);
    BOOST_CHECK_EQUAL(parsed_v2.tx->GetHash(), tx.GetHash());
    BOOST_CHECK_EQUAL(parsed_v2.outputs[1].GetTokenId().v, 11U);
}

/**
 * The DFI specific records hold the complete previous transaction and the token id.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_defi_records)
{
    // The token data is not serialized in the tests by default
    CTxOut::SERIALIZE_FORCED_TO_OLD_IN_TESTS = false;

    CMutableTransaction prev_tx = CreateTestTx();
    prev_tx.vout[0].nTokenId = DCT_ID{5};
    prev_tx.nVersion = CTransaction::TOKENS_MIN_VERSION;

    PartiallySignedTransaction psbt(CreateTestTx(), 2);
    psbt.inputs[0].SetPreviousTx(CTransaction(prev_tx));

    const auto stored = psbt.inputs[0].GetPreviousTx();
    BOOST_REQUIRE(stored.has_value());
    BOOST_CHECK_EQUAL(stored->GetHash(), prev_tx.GetHash());
    BOOST_CHECK_EQUAL(stored->vout[0].nTokenId.v, 5U);

    // The record survives a round trip
    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);
    const auto stored_parsed = parsed.inputs[0].GetPreviousTx();
    BOOST_REQUIRE(stored_parsed.has_value());
    BOOST_CHECK_EQUAL(stored_parsed->GetHash(), prev_tx.GetHash());

    CTxOut::SERIALIZE_FORCED_TO_OLD_IN_TESTS = true;
}

/**
 * The global xpubs of a PSBTv2 must survive a round trip.
 */
BOOST_AUTO_TEST_CASE(psbt_v2_global_xpubs)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx, 2);

    CKey key;
    key.MakeNewKey(true);
    CExtKey ext_key;
    const unsigned char seed[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    ext_key.SetSeed(seed, sizeof(seed));
    CExtPubKey ext_pubkey = ext_key.Neuter();
    ext_pubkey.nDepth = 3;
    ext_pubkey.nChild = 7;

    PSBTXPub xpub_entry;
    xpub_entry.version = 0;
    xpub_entry.xpub = ext_pubkey;
    xpub_entry.origin.fingerprint[0] = 0x01;
    xpub_entry.origin.fingerprint[1] = 0x02;
    xpub_entry.origin.fingerprint[2] = 0x03;
    xpub_entry.origin.fingerprint[3] = 0x04;
    xpub_entry.origin.path = {5, 6, 7};
    psbt.m_xpubs.push_back(xpub_entry);

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    BOOST_REQUIRE_EQUAL(parsed.m_xpubs.size(), 1U);
    BOOST_CHECK_EQUAL(parsed.m_xpubs[0].version, xpub_entry.version);
    BOOST_CHECK(parsed.m_xpubs[0].xpub == ext_pubkey);
    BOOST_CHECK(parsed.m_xpubs[0].origin == xpub_entry.origin);
}

/**
 * A PSBTv0 with a global xpub record must keep its byte stream (the xpub stays an unknown key).
 */
BOOST_AUTO_TEST_CASE(psbt_v0_xpub_byte_stream_unchanged)
{
    CMutableTransaction tx = CreateTestTx();
    PartiallySignedTransaction psbt(tx);

    // A BIP174 global xpub record: 0x01 || version || xpub
    CExtKey ext_key;
    const unsigned char seed[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    ext_key.SetSeed(seed, sizeof(seed));
    const CExtPubKey ext_pubkey = ext_key.Neuter();

    std::vector<unsigned char> key;
    key.push_back(PSBT_GLOBAL_XPUB);
    key.push_back(0x00); // version
    unsigned char code[BIP32_EXTKEY_SIZE];
    ext_pubkey.Encode(code);
    key.insert(key.end(), code, code + BIP32_EXTKEY_SIZE);

    // The value is the derivation path of the xpub (the length is added by the serialization)
    std::vector<unsigned char> value;
    const unsigned char fingerprint[4] = {0x01, 0x02, 0x03, 0x04};
    value.insert(value.end(), fingerprint, fingerprint + 4);
    for (const uint32_t path : {5, 6, 7}) {
        value.push_back(path & 0xff);
        value.push_back((path >> 8) & 0xff);
        value.push_back((path >> 16) & 0xff);
        value.push_back((path >> 24) & 0xff);
    }

    psbt.unknown[key] = value;

    const std::vector<unsigned char> serialized = SerializePSBT(psbt);
    PartiallySignedTransaction parsed = ParsePSBT(serialized);

    // The xpub is an unknown key in a PSBTv0 and the byte stream must not change
    BOOST_CHECK_EQUAL(parsed.GetVersion(), 0U);
    BOOST_CHECK(parsed.m_xpubs.empty());
    BOOST_CHECK_EQUAL(parsed.unknown.count(key), 1U);
    BOOST_CHECK(parsed.unknown.at(key) == value);
    BOOST_CHECK(SerializePSBT(parsed) == serialized);
}

BOOST_AUTO_TEST_SUITE_END()
