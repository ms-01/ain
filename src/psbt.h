// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_PSBT_H
#define DEFI_PSBT_H

#include <amount.h>
#include <attributes.h>
#include <node/transaction.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/keyorigin.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <streams.h>

#include <optional>
#include <string>
#include <vector>

// Magic bytes
static constexpr uint8_t PSBT_MAGIC_BYTES[5] = {'p', 's', 'b', 't', 0xff};

// Global types
static constexpr uint8_t PSBT_GLOBAL_UNSIGNED_TX = 0x00;
static constexpr uint8_t PSBT_GLOBAL_XPUB = 0x01;
static constexpr uint8_t PSBT_GLOBAL_TX_VERSION = 0x02;
static constexpr uint8_t PSBT_GLOBAL_FALLBACK_LOCKTIME = 0x03;
static constexpr uint8_t PSBT_GLOBAL_INPUT_COUNT = 0x04;
static constexpr uint8_t PSBT_GLOBAL_OUTPUT_COUNT = 0x05;
static constexpr uint8_t PSBT_GLOBAL_TX_MODIFIABLE = 0x06;
static constexpr uint8_t PSBT_GLOBAL_VERSION = 0xFB;
static constexpr uint8_t PSBT_GLOBAL_PROPRIETARY = 0xFC;

// Input types
static constexpr uint8_t PSBT_IN_NON_WITNESS_UTXO = 0x00;
static constexpr uint8_t PSBT_IN_WITNESS_UTXO = 0x01;
static constexpr uint8_t PSBT_IN_PARTIAL_SIG = 0x02;
static constexpr uint8_t PSBT_IN_SIGHASH = 0x03;
static constexpr uint8_t PSBT_IN_REDEEMSCRIPT = 0x04;
static constexpr uint8_t PSBT_IN_WITNESSSCRIPT = 0x05;
static constexpr uint8_t PSBT_IN_BIP32_DERIVATION = 0x06;
static constexpr uint8_t PSBT_IN_SCRIPTSIG = 0x07;
static constexpr uint8_t PSBT_IN_SCRIPTWITNESS = 0x08;
static constexpr uint8_t PSBT_IN_PREVIOUS_TXID = 0x0e;
static constexpr uint8_t PSBT_IN_OUTPUT_INDEX = 0x0f;
static constexpr uint8_t PSBT_IN_SEQUENCE = 0x10;
static constexpr uint8_t PSBT_IN_REQUIRED_TIME_LOCKTIME = 0x11;
static constexpr uint8_t PSBT_IN_REQUIRED_HEIGHT_LOCKTIME = 0x12;
static constexpr uint8_t PSBT_IN_PROPRIETARY = 0xFC;

// Output types
static constexpr uint8_t PSBT_OUT_REDEEMSCRIPT = 0x00;
static constexpr uint8_t PSBT_OUT_WITNESSSCRIPT = 0x01;
static constexpr uint8_t PSBT_OUT_BIP32_DERIVATION = 0x02;
static constexpr uint8_t PSBT_OUT_AMOUNT = 0x03;
static constexpr uint8_t PSBT_OUT_SCRIPT = 0x04;
static constexpr uint8_t PSBT_OUT_PROPRIETARY = 0xFC;

// The separator is 0x00. Reading this in means that the unserializer can interpret it
// as a 0 length key which indicates that this is the separator. The separator has no value.
static constexpr uint8_t PSBT_SEPARATOR = 0x00;

/**
 * The highest PSBT version supported by this implementation.
 * 0 = BIP174 (PSBTv0), 2 = PSBTv2 (global version, global xpubs and per output data).
 */
static constexpr uint32_t PSBT_HIGHEST_VERSION = 2;

/** Key type of a BIP174 proprietary record (0xFC || CompactSize(subtype) || identifier). */
static constexpr uint8_t PSBT_PROPRIETARY_KEY_TYPE = 0xFC;

/** Identifier of the DFI specific proprietary records. */
static constexpr const char* PSBT_PROPRIETARY_IDENTIFIER = "defi";

/** Subtype of the DFI proprietary input record holding the complete previous transaction. */
static constexpr uint64_t PSBT_PROPRIETARY_SUBTYPE_PREVIOUS_TX = 1;

/** Subtype of the DFI proprietary output record holding the token id of the output. */
static constexpr uint64_t PSBT_PROPRIETARY_SUBTYPE_OUTPUT_TOKEN_ID = 1;

/**
 * Build a BIP174 proprietary key: 0xFC || CompactSize(subtype) || identifier.
 */
inline std::vector<unsigned char> MakePSBTProprietaryKey(uint64_t subtype, const std::string& identifier)
{
    std::vector<unsigned char> key;
    key.push_back(PSBT_PROPRIETARY_KEY_TYPE);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    WriteCompactSize(ss, subtype);
    for (const char c : ss) {
        key.push_back(static_cast<unsigned char>(c));
    }

    key.insert(key.end(), identifier.begin(), identifier.end());
    return key;
}

/**
 * Write a compact size encoded number as the value of a PSBT key.
 */
template<typename Stream>
inline void SerializePSBTCompactSize(Stream& s, uint64_t n)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    WriteCompactSize(ss, n);
    const std::vector<unsigned char> bytes(ss.begin(), ss.end());
    WriteCompactSize(s, bytes.size());
    s.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/**
 * Read a compact size encoded number from the value of a PSBT key.
 */
inline uint64_t UnserializePSBTCompactSize(const std::vector<unsigned char>& bytes)
{
    if (bytes.empty()) {
        throw std::ios_base::failure("Empty value for a compact size encoded number");
    }
    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    return ReadCompactSize(ss);
}

/**
 * A global xpub record of a PSBTv2. The version byte is kept separately because
 * CExtPubKey does not carry it, but it is part of the serialized key.
 */
struct PSBTXPub
{
    uint8_t version{0};
    CExtPubKey xpub;
    KeyOriginInfo origin;

    friend bool operator==(const PSBTXPub& a, const PSBTXPub& b)
    {
        return a.version == b.version && a.xpub == b.xpub && a.origin == b.origin;
    }

    friend bool operator<(const PSBTXPub& a, const PSBTXPub& b)
    {
        if (a.version != b.version) return a.version < b.version;
        unsigned char code_a[BIP32_EXTKEY_SIZE];
        unsigned char code_b[BIP32_EXTKEY_SIZE];
        a.xpub.Encode(code_a);
        b.xpub.Encode(code_b);
        const int cmp = memcmp(code_a, code_b, BIP32_EXTKEY_SIZE);
        if (cmp != 0) return cmp < 0;
        return std::lexicographical_compare(a.origin.path.begin(), a.origin.path.end(),
                                            b.origin.path.begin(), b.origin.path.end());
    }
};

/** A structure for PSBTs which contain per-input information */
struct PSBTInput
{
    CTransactionRef non_witness_utxo;
    CTxOut witness_utxo;
    CScript redeem_script;
    CScript witness_script;
    CScript final_script_sig;
    CScriptWitness final_script_witness;
    std::map<CPubKey, KeyOriginInfo> hd_keypaths;
    std::map<CKeyID, SigPair> partial_sigs;
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown;
    int sighash_type = 0;

    /** The PSBT version this input belongs to (0 = BIP174, 2 = PSBTv2). */
    uint32_t m_psbt_version{0};

    /** PSBTv2 only: the txid of the output being spent. */
    uint256 prev_txid;
    /** PSBTv2 only: the index of the output being spent. */
    uint32_t prev_out{0};
    /** PSBTv2 only: the sequence number of the input. */
    std::optional<uint32_t> sequence;
    /** PSBTv2 only: required time based locktime. */
    std::optional<uint32_t> time_locktime;
    /** PSBTv2 only: required height based locktime. */
    std::optional<uint32_t> height_locktime;

    /** Proprietary records (BIP174), stored as raw key bytes -> raw value bytes. */
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> proprietary;

    bool IsNull() const;
    void FillSignatureData(SignatureData& sigdata) const;
    void FromSignatureData(const SignatureData& sigdata);
    void Merge(const PSBTInput& input);
    bool IsSane() const;

    /** The complete previous transaction stored in the DFI specific proprietary record (if any). */
    std::optional<CMutableTransaction> GetPreviousTx() const;
    /** Store the complete previous transaction in the DFI specific proprietary record. */
    void SetPreviousTx(const CTransaction& prev_tx);
    PSBTInput() {}

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        // Write the utxo
        // If there is a non-witness utxo, then don't add the witness one.
        if (non_witness_utxo) {
            SerializeToVector(s, PSBT_IN_NON_WITNESS_UTXO);
            /// @todo tokens: check if tx version was properly assumed!
            OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() | SERIALIZE_TRANSACTION_NO_WITNESS | (non_witness_utxo->nVersion < CTransaction::TOKENS_MIN_VERSION ? SERIALIZE_TRANSACTION_NO_TOKENS : 0));
            SerializeToVector(os, non_witness_utxo);
        } else if (!witness_utxo.IsNull()) {
            SerializeToVector(s, PSBT_IN_WITNESS_UTXO);
            /// @todo tokens: override with correct version when witness_utxo (and wallet's Coin's tokens) versioning implemented!
            OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() | SERIALIZE_TRANSACTION_NO_TOKENS);
            SerializeToVector(os, witness_utxo);
        }

        if (final_script_sig.empty() && final_script_witness.IsNull()) {
            // Write any partial signatures
            for (auto sig_pair : partial_sigs) {
                SerializeToVector(s, PSBT_IN_PARTIAL_SIG, MakeSpan(sig_pair.second.first));
                s << sig_pair.second.second;
            }

            // Write the sighash type
            if (sighash_type > 0) {
                SerializeToVector(s, PSBT_IN_SIGHASH);
                SerializeToVector(s, sighash_type);
            }

            // Write the redeem script
            if (!redeem_script.empty()) {
                SerializeToVector(s, PSBT_IN_REDEEMSCRIPT);
                s << redeem_script;
            }

            // Write the witness script
            if (!witness_script.empty()) {
                SerializeToVector(s, PSBT_IN_WITNESSSCRIPT);
                s << witness_script;
            }

            // Write any hd keypaths
            SerializeHDKeypaths(s, hd_keypaths, PSBT_IN_BIP32_DERIVATION);
        }

        // Write script sig
        if (!final_script_sig.empty()) {
            SerializeToVector(s, PSBT_IN_SCRIPTSIG);
            s << final_script_sig;
        }
        // write script witness
        if (!final_script_witness.IsNull()) {
            SerializeToVector(s, PSBT_IN_SCRIPTWITNESS);
            SerializeToVector(s, final_script_witness.stack);
        }

        // Write PSBTv2 specific fields
        if (m_psbt_version >= 2) {
            SerializeToVector(s, PSBT_IN_PREVIOUS_TXID);
            SerializeToVector(s, prev_txid);

            SerializeToVector(s, PSBT_IN_OUTPUT_INDEX);
            SerializeToVector(s, prev_out);

            if (sequence) {
                SerializeToVector(s, PSBT_IN_SEQUENCE);
                SerializeToVector(s, *sequence);
            }
            if (time_locktime) {
                SerializeToVector(s, PSBT_IN_REQUIRED_TIME_LOCKTIME);
                SerializeToVector(s, *time_locktime);
            }
            if (height_locktime) {
                SerializeToVector(s, PSBT_IN_REQUIRED_HEIGHT_LOCKTIME);
                SerializeToVector(s, *height_locktime);
            }
        }

        // Write proprietary records (PSBTv2 only, in a PSBTv0 they are unknown keys)
        if (m_psbt_version >= 2) {
            for (const auto& entry : proprietary) {
                s << entry.first;
                s << entry.second;
            }
        }

        // Write unknown things
        for (auto& entry : unknown) {
            s << entry.first;
            s << entry.second;
        }

        s << PSBT_SEPARATOR;
    }


    template <typename Stream>
    inline void Unserialize(Stream& s) {
        // Read a key that is not known in this version of the PSBT as an unknown key
        auto read_unknown_value = [this, &s](std::vector<unsigned char> key) {
            if (unknown.count(key) > 0) {
                throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
            }
            std::vector<unsigned char> val_bytes;
            s >> val_bytes;
            unknown.emplace(std::move(key), std::move(val_bytes));
        };

        // Read loop
        bool found_sep = false;
        bool found_prev_txid = false;
        bool found_prev_out = false;
        while(!s.empty()) {
            // Read
            std::vector<unsigned char> key;
            s >> key;

            // the key is empty if that was actually a separator byte
            // This is a special case for key lengths 0 as those are not allowed (except for separator)
            if (key.empty()) {
                found_sep = true;
                break;
            }

            // First byte of key is the type
            unsigned char type = key[0];

            // Do stuff based on type
            switch(type) {
                case PSBT_IN_NON_WITNESS_UTXO:
                {
                    if (non_witness_utxo) {
                        throw std::ios_base::failure("Duplicate Key, input non-witness utxo already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Non-witness utxo key is more than one byte type");
                    }
                    // Set the stream to unserialize with witness since this is always a valid network transaction
                    OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() & ~SERIALIZE_TRANSACTION_NO_WITNESS);
                    UnserializeFromVector(os, non_witness_utxo); /// @todo tokens: check if tx version was properly assumed!
                    break;
                }
                case PSBT_IN_WITNESS_UTXO:
                    if (!witness_utxo.IsNull()) {
                        throw std::ios_base::failure("Duplicate Key, input witness utxo already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Witness utxo key is more than one byte type");
                    }
                    /// @todo tokens: override with correct version when witness_utxo (and wallet's Coin's tokens) versioning implemented!
                    {
                        OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() | SERIALIZE_TRANSACTION_NO_TOKENS);
                        UnserializeFromVector(os, witness_utxo);
                    }
                    break;
                case PSBT_IN_PARTIAL_SIG:
                {
                    // Make sure that the key is the size of pubkey + 1
                    if (key.size() != CPubKey::PUBLIC_KEY_SIZE + 1 && key.size() != CPubKey::COMPRESSED_PUBLIC_KEY_SIZE + 1) {
                        throw std::ios_base::failure("Size of key was not the expected size for the type partial signature pubkey");
                    }
                    // Read in the pubkey from key
                    CPubKey pubkey(key.begin() + 1, key.end());
                    if (!pubkey.IsFullyValid()) {
                       throw std::ios_base::failure("Invalid pubkey");
                    }
                    if (partial_sigs.count(pubkey.GetID()) > 0) {
                        throw std::ios_base::failure("Duplicate Key, input partial signature for pubkey already provided");
                    }

                    // Read in the signature from value
                    std::vector<unsigned char> sig;
                    s >> sig;

                    // Add to list
                    partial_sigs.emplace(pubkey.GetID(), SigPair(pubkey, std::move(sig)));
                    break;
                }
                case PSBT_IN_SIGHASH:
                    if (sighash_type > 0) {
                        throw std::ios_base::failure("Duplicate Key, input sighash type already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Sighash type key is more than one byte type");
                    }
                    UnserializeFromVector(s, sighash_type);
                    break;
                case PSBT_IN_REDEEMSCRIPT:
                {
                    if (!redeem_script.empty()) {
                        throw std::ios_base::failure("Duplicate Key, input redeemScript already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input redeemScript key is more than one byte type");
                    }
                    s >> redeem_script;
                    break;
                }
                case PSBT_IN_WITNESSSCRIPT:
                {
                    if (!witness_script.empty()) {
                        throw std::ios_base::failure("Duplicate Key, input witnessScript already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input witnessScript key is more than one byte type");
                    }
                    s >> witness_script;
                    break;
                }
                case PSBT_IN_BIP32_DERIVATION:
                {
                    DeserializeHDKeypaths(s, key, hd_keypaths);
                    break;
                }
                case PSBT_IN_SCRIPTSIG:
                {
                    if (!final_script_sig.empty()) {
                        throw std::ios_base::failure("Duplicate Key, input final scriptSig already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Final scriptSig key is more than one byte type");
                    }
                    s >> final_script_sig;
                    break;
                }
                case PSBT_IN_SCRIPTWITNESS:
                {
                    if (!final_script_witness.IsNull()) {
                        throw std::ios_base::failure("Duplicate Key, input final scriptWitness already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Final scriptWitness key is more than one byte type");
                    }
                    UnserializeFromVector(s, final_script_witness.stack);
                    break;
                }
                case PSBT_IN_PREVIOUS_TXID:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input previous txid key is more than one byte type");
                    }
                    UnserializeFromVector(s, prev_txid);
                    found_prev_txid = true;
                    break;
                case PSBT_IN_OUTPUT_INDEX:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input output index key is more than one byte type");
                    }
                    UnserializeFromVector(s, prev_out);
                    found_prev_out = true;
                    break;
                case PSBT_IN_SEQUENCE:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input sequence key is more than one byte type");
                    }
                    sequence.emplace();
                    UnserializeFromVector(s, *sequence);
                    break;
                case PSBT_IN_REQUIRED_TIME_LOCKTIME:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input required time based locktime key is more than one byte type");
                    }
                    time_locktime.emplace();
                    UnserializeFromVector(s, *time_locktime);
                    break;
                case PSBT_IN_REQUIRED_HEIGHT_LOCKTIME:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Input required height based locktime key is more than one byte type");
                    }
                    height_locktime.emplace();
                    UnserializeFromVector(s, *height_locktime);
                    break;
                case PSBT_IN_PROPRIETARY:
                    if (m_psbt_version >= 2) {
                        // Read in the value of the proprietary record
                        std::vector<unsigned char> val_bytes;
                        s >> val_bytes;
                        if (!proprietary.emplace(key, std::move(val_bytes)).second) {
                            throw std::ios_base::failure("Duplicate Key, proprietary key already provided");
                        }
                        break;
                    }
                    // In PSBTv0 proprietary records are kept as unknown keys (BIP174 behaviour)
                    read_unknown_value(std::move(key));
                    break;
                // Unknown stuff
                default:
                    if (unknown.count(key) > 0) {
                        throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
                    }
                    // Read in the value
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    unknown.emplace(std::move(key), std::move(val_bytes));
                    break;
            }
        }

        if (!found_sep) {
            throw std::ios_base::failure("Separator is missing at the end of an input map");
        }

        if (m_psbt_version >= 2) {
            if (!found_prev_txid) {
                throw std::ios_base::failure("Previous TXID is required in PSBTv2");
            }
            if (!found_prev_out) {
                throw std::ios_base::failure("Previous output's index is required in PSBTv2");
            }
        }
    }

    template <typename Stream>
    PSBTInput(deserialize_type, Stream& s) {
        Unserialize(s);
    }
};

/** A structure for PSBTs which contains per output information */
struct PSBTOutput
{
    CScript redeem_script;
    CScript witness_script;
    std::map<CPubKey, KeyOriginInfo> hd_keypaths;
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown;

    /** The PSBT version this output belongs to (0 = BIP174, 2 = PSBTv2). */
    uint32_t m_psbt_version{0};

    /** PSBTv2 only: the amount of the output. */
    CAmount amount{0};
    /** PSBTv2 only: the scriptPubKey of the output. */
    CScript script;
    /** PSBTv2 only: the token id of the output (DFI specific, kept in a proprietary record). */
    DCT_ID token_id;

    /** Proprietary records (BIP174), stored as raw key bytes -> raw value bytes. */
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> proprietary;

    bool IsNull() const;
    void FillSignatureData(SignatureData& sigdata) const;
    void FromSignatureData(const SignatureData& sigdata);
    void Merge(const PSBTOutput& output);
    bool IsSane() const;

    /** The token id stored in the DFI specific proprietary record (0 if there is none). */
    DCT_ID GetTokenId() const;
    /** Store the token id in the DFI specific proprietary record. */
    void SetTokenId(const DCT_ID& id);
    PSBTOutput() {}

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        // Write the redeem script
        if (!redeem_script.empty()) {
            SerializeToVector(s, PSBT_OUT_REDEEMSCRIPT);
            s << redeem_script;
        }

        // Write the witness script
        if (!witness_script.empty()) {
            SerializeToVector(s, PSBT_OUT_WITNESSSCRIPT);
            s << witness_script;
        }

        // Write any hd keypaths
        SerializeHDKeypaths(s, hd_keypaths, PSBT_OUT_BIP32_DERIVATION);

        // Write PSBTv2 specific fields
        if (m_psbt_version >= 2) {
            SerializeToVector(s, PSBT_OUT_AMOUNT);
            SerializeToVector(s, amount);

            SerializeToVector(s, PSBT_OUT_SCRIPT);
            s << script;
        }

        // Write proprietary records (PSBTv2 only, in a PSBTv0 they are unknown keys)
        if (m_psbt_version >= 2) {
            for (const auto& entry : proprietary) {
                s << entry.first;
                s << entry.second;
            }
        }

        // Write unknown things
        for (auto& entry : unknown) {
            s << entry.first;
            s << entry.second;
        }

        s << PSBT_SEPARATOR;
    }


    template <typename Stream>
    inline void Unserialize(Stream& s) {
        // Read a key that is not known in this version of the PSBT as an unknown key
        auto read_unknown_value = [this, &s](std::vector<unsigned char> key) {
            if (unknown.count(key) > 0) {
                throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
            }
            std::vector<unsigned char> val_bytes;
            s >> val_bytes;
            unknown.emplace(std::move(key), std::move(val_bytes));
        };

        // Read loop
        bool found_sep = false;
        while(!s.empty()) {
            // Read
            std::vector<unsigned char> key;
            s >> key;

            // the key is empty if that was actually a separator byte
            // This is a special case for key lengths 0 as those are not allowed (except for separator)
            if (key.empty()) {
                found_sep = true;
                break;
            }

            // First byte of key is the type
            unsigned char type = key[0];

            // Do stuff based on type
            switch(type) {
                case PSBT_OUT_REDEEMSCRIPT:
                {
                    if (!redeem_script.empty()) {
                        throw std::ios_base::failure("Duplicate Key, output redeemScript already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Output redeemScript key is more than one byte type");
                    }
                    s >> redeem_script;
                    break;
                }
                case PSBT_OUT_WITNESSSCRIPT:
                {
                    if (!witness_script.empty()) {
                        throw std::ios_base::failure("Duplicate Key, output witnessScript already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Output witnessScript key is more than one byte type");
                    }
                    s >> witness_script;
                    break;
                }
                case PSBT_OUT_BIP32_DERIVATION:
                {
                    DeserializeHDKeypaths(s, key, hd_keypaths);
                    break;
                }
                case PSBT_OUT_AMOUNT:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Output amount key is more than one byte type");
                    }
                    UnserializeFromVector(s, amount);
                    break;
                case PSBT_OUT_SCRIPT:
                    if (m_psbt_version < 2) {
                        // In a PSBTv0 these keys are unknown keys (BIP174 behaviour), they are
                        // kept as they are to not change the byte stream of a PSBTv0.
                        read_unknown_value(std::move(key));
                        break;
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Output script key is more than one byte type");
                    }
                    s >> script;
                    break;
                case PSBT_OUT_PROPRIETARY:
                    if (m_psbt_version >= 2) {
                        // Read in the value of the proprietary record
                        std::vector<unsigned char> val_bytes;
                        s >> val_bytes;
                        if (!proprietary.emplace(key, std::move(val_bytes)).second) {
                            throw std::ios_base::failure("Duplicate Key, proprietary key already provided");
                        }
                        break;
                    }
                    // In PSBTv0 proprietary records are kept as unknown keys (BIP174 behaviour)
                    read_unknown_value(std::move(key));
                    break;
                // Unknown stuff
                default: {
                    if (unknown.count(key) > 0) {
                        throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
                    }
                    // Read in the value
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    unknown.emplace(std::move(key), std::move(val_bytes));
                    break;
                }
            }
        }

        if (!found_sep) {
            throw std::ios_base::failure("Separator is missing at the end of an output map");
        }
    }

    template <typename Stream>
    PSBTOutput(deserialize_type, Stream& s) {
        Unserialize(s);
    }
};

/** A version of CTransaction with the PSBT format*/
struct PartiallySignedTransaction
{
    std::optional<CMutableTransaction> tx;
    std::vector<PSBTInput> inputs;
    std::vector<PSBTOutput> outputs;
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown;

    /** The PSBT version (0 = BIP174, 2 = PSBTv2). */
    uint32_t m_version{0};
    /** PSBTv2 only: the version of the unsigned transaction. */
    uint32_t tx_version{CTransaction::CURRENT_VERSION};
    /** PSBTv2 only: the locktime used if no input requires a specific one. */
    std::optional<uint32_t> fallback_locktime;
    /** PSBTv2 only: the transaction modifiable flags (BIP370). */
    std::optional<uint8_t> m_tx_modifiable;
    /** PSBTv2 only: the global xpub records. */
    std::vector<PSBTXPub> m_xpubs;
    /** Proprietary records (BIP174), stored as raw key bytes -> raw value bytes. */
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> proprietary;

    bool IsNull() const;

    /** Merge psbt into this. The two psbts must have the same underlying CTransaction (i.e. the
      * same actual DeFi Blockchain transaction.) Returns true if the merge succeeded, false otherwise. */
    NODISCARD bool Merge(const PartiallySignedTransaction& psbt);
    bool IsSane() const;
    bool AddInput(const CTxIn& txin, PSBTInput& psbtin);
    bool AddOutput(const CTxOut& txout, const PSBTOutput& psbtout);

    /** The PSBT version of this PSBT (0 = BIP174, 2 = PSBTv2). */
    uint32_t GetVersion() const { return m_version; }
    /** Set the PSBT version (only 0 and 2 are supported). */
    void SetVersion(uint32_t version);

    /**
     * Rebuild the unsigned transaction of a PSBTv2 from its global and per input/output data.
     * Returns std::nullopt if the PSBT does not contain the required information.
     */
    std::optional<CMutableTransaction> GetUnsignedTx() const;
    /**
     * Compute the locktime of a PSBTv2 from the fallback locktime and the locktimes required
     * by the individual inputs. Returns std::nullopt if the requirements are contradictory.
     */
    std::optional<uint32_t> ComputeTimeLock() const;

    PartiallySignedTransaction() {}
    PartiallySignedTransaction(const PartiallySignedTransaction& psbt_in) : tx(psbt_in.tx), inputs(psbt_in.inputs), outputs(psbt_in.outputs), unknown(psbt_in.unknown), m_version(psbt_in.m_version), tx_version(psbt_in.tx_version), fallback_locktime(psbt_in.fallback_locktime), m_tx_modifiable(psbt_in.m_tx_modifiable), m_xpubs(psbt_in.m_xpubs), proprietary(psbt_in.proprietary) {}
    explicit PartiallySignedTransaction(const CMutableTransaction& tx);
    /**
     * Create a PSBT of the given version (0 = BIP174, 2 = PSBTv2) for the given transaction.
     */
    PartiallySignedTransaction(const CMutableTransaction& tx, uint32_t version);
    /**
     * Finds the UTXO for a given input index
     *
     * @param[out] utxo The UTXO of the input if found
     * @param[in] input_index Index of the input to retrieve the UTXO of
     * @return Whether the UTXO for the specified input was found
     */
    bool GetInputUTXO(CTxOut& utxo, int input_index) const;

    template <typename Stream>
    inline void Serialize(Stream& s) const {

        // magic bytes
        s << PSBT_MAGIC_BYTES;

        if (GetVersion() < 2) {
            // unsigned tx flag
            SerializeToVector(s, PSBT_GLOBAL_UNSIGNED_TX);

            // Write serialized tx to a stream
            OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() | SERIALIZE_TRANSACTION_NO_WITNESS);
            SerializeToVector(os, *tx);
        } else {
            // Write the global xpubs
            for (const PSBTXPub& xpub_entry : m_xpubs) {
                // The serialized key contains the version byte in front of the xpub
                unsigned char code[BIP32_EXTKEY_SIZE];
                xpub_entry.xpub.Encode(code);
                std::vector<unsigned char> key;
                key.push_back(PSBT_GLOBAL_XPUB);
                key.push_back(xpub_entry.version);
                key.insert(key.end(), code, code + BIP32_EXTKEY_SIZE);
                s << key;

                // The value is the derivation path of the xpub
                WriteCompactSize(s, (xpub_entry.origin.path.size() + 1) * sizeof(uint32_t));
                s << xpub_entry.origin.fingerprint;
                for (const uint32_t path : xpub_entry.origin.path) {
                    s << path;
                }
            }

            // Write the version of the unsigned transaction
            SerializeToVector(s, PSBT_GLOBAL_TX_VERSION);
            SerializeToVector(s, tx_version);

            // Write the fallback locktime
            if (fallback_locktime) {
                SerializeToVector(s, PSBT_GLOBAL_FALLBACK_LOCKTIME);
                SerializeToVector(s, *fallback_locktime);
            }

            // Write the input and output counts
            SerializeToVector(s, PSBT_GLOBAL_INPUT_COUNT);
            SerializePSBTCompactSize(s, inputs.size());
            SerializeToVector(s, PSBT_GLOBAL_OUTPUT_COUNT);
            SerializePSBTCompactSize(s, outputs.size());

            // Write the transaction modifiable flags
            if (m_tx_modifiable) {
                SerializeToVector(s, PSBT_GLOBAL_TX_MODIFIABLE);
                SerializeToVector(s, *m_tx_modifiable);
            }

            // Write the PSBT version
            SerializeToVector(s, PSBT_GLOBAL_VERSION);
            SerializeToVector(s, m_version);
        }

        // Write proprietary records (PSBTv2 only, in a PSBTv0 they are unknown keys)
        if (GetVersion() >= 2) {
            for (const auto& entry : proprietary) {
                s << entry.first;
                s << entry.second;
            }
        }

        // Write the unknown things
        for (auto& entry : unknown) {
            s << entry.first;
            s << entry.second;
        }

        // Separator
        s << PSBT_SEPARATOR;

        // Write inputs
        for (const PSBTInput& input : inputs) {
            s << input;
        }
        // Write outputs
        for (const PSBTOutput& output : outputs) {
            s << output;
        }
    }


    template <typename Stream>
    inline void Unserialize(Stream& s) {
        // Read the magic bytes
        uint8_t magic[5];
        s >> magic;
        if (!std::equal(magic, magic + 5, PSBT_MAGIC_BYTES)) {
            throw std::ios_base::failure("Invalid PSBT magic bytes");
        }

        // Read global data
        bool found_sep = false;
        bool found_tx_version = false;
        bool found_fallback_locktime = false;
        bool found_input_count = false;
        bool found_output_count = false;
        uint64_t input_count = 0;
        uint64_t output_count = 0;
        // The xpub and the proprietary records are collected first, because the version of the
        // PSBT is only known once the whole global map has been read.
        std::vector<PSBTXPub> parsed_xpubs;
        std::vector<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>> raw_xpub_records;
        std::vector<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>> parsed_proprietary;
        while(!s.empty()) {
            // Read
            std::vector<unsigned char> key;
            s >> key;

            // the key is empty if that was actually a separator byte
            // This is a special case for key lengths 0 as those are not allowed (except for separator)
            if (key.empty()) {
                found_sep = true;
                break;
            }

            // First byte of key is the type
            unsigned char type = key[0];

            // Do stuff based on type
            switch(type) {
                case PSBT_GLOBAL_UNSIGNED_TX:
                {
                    if (tx) {
                        throw std::ios_base::failure("Duplicate Key, unsigned tx already provided");
                    } else if (key.size() != 1) {
                        throw std::ios_base::failure("Global unsigned tx key is more than one byte type");
                    }
                    CMutableTransaction mtx;
                    // Set the stream to serialize with non-witness since this should always be non-witness
                    OverrideStream<Stream> os(&s, s.GetType(), s.GetVersion() | SERIALIZE_TRANSACTION_NO_WITNESS);
                    UnserializeFromVector(os, mtx);
                    tx = std::move(mtx);
                    // Make sure that all scriptSigs and scriptWitnesses are empty
                    for (const CTxIn& txin : tx->vin) {
                        if (!txin.scriptSig.empty() || !txin.scriptWitness.IsNull()) {
                            throw std::ios_base::failure("Unsigned tx does not have empty scriptSigs and scriptWitnesses.");
                        }
                    }
                    tx_version = tx->nVersion;
                    fallback_locktime = tx->nLockTime;
                    input_count = tx->vin.size();
                    output_count = tx->vout.size();
                    break;
                }
                case PSBT_GLOBAL_XPUB:
                {
                    if (key.size() != 2 + BIP32_EXTKEY_SIZE) {
                        throw std::ios_base::failure("Global xpub key has an unexpected length");
                    }
                    // Read the value (the derivation path of the xpub)
                    const uint64_t value_len = ReadCompactSize(s);
                    if (value_len % 4 || value_len == 0) {
                        throw std::ios_base::failure("Invalid length for HD key path");
                    }
                    std::vector<unsigned char> value(value_len);
                    s.read(reinterpret_cast<char*>(value.data()), value_len);

                    CDataStream ss(value, SER_NETWORK, PROTOCOL_VERSION);
                    PSBTXPub xpub_entry;
                    xpub_entry.version = key[1];
                    xpub_entry.xpub.Decode(&key[2]);
                    ss >> xpub_entry.origin.fingerprint;
                    for (uint64_t i = 4; i < value_len; i += sizeof(uint32_t)) {
                        uint32_t index;
                        ss >> index;
                        xpub_entry.origin.path.push_back(index);
                    }
                    for (const auto& known : raw_xpub_records) {
                        if (known.first == key) {
                            throw std::ios_base::failure("Duplicate Key, global xpub already provided");
                        }
                    }
                    parsed_xpubs.push_back(xpub_entry);
                    raw_xpub_records.emplace_back(key, value);
                    break;
                }
                case PSBT_GLOBAL_TX_VERSION:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global tx version key is more than one byte type");
                    }
                    UnserializeFromVector(s, tx_version);
                    found_tx_version = true;
                    break;
                }
                case PSBT_GLOBAL_FALLBACK_LOCKTIME:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global fallback locktime key is more than one byte type");
                    }
                    fallback_locktime.emplace();
                    UnserializeFromVector(s, *fallback_locktime);
                    found_fallback_locktime = true;
                    break;
                }
                case PSBT_GLOBAL_INPUT_COUNT:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global input count key is more than one byte type");
                    }
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    input_count = UnserializePSBTCompactSize(val_bytes);
                    found_input_count = true;
                    break;
                }
                case PSBT_GLOBAL_OUTPUT_COUNT:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global output count key is more than one byte type");
                    }
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    output_count = UnserializePSBTCompactSize(val_bytes);
                    found_output_count = true;
                    break;
                }
                case PSBT_GLOBAL_TX_MODIFIABLE:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global tx modifiable key is more than one byte type");
                    }
                    uint8_t tx_mod;
                    UnserializeFromVector(s, tx_mod);
                    m_tx_modifiable.emplace(tx_mod);
                    break;
                }
                case PSBT_GLOBAL_VERSION:
                {
                    if (key.size() != 1) {
                        throw std::ios_base::failure("Global PSBT version key is more than one byte type");
                    }
                    UnserializeFromVector(s, m_version);
                    if (m_version > PSBT_HIGHEST_VERSION) {
                        throw std::ios_base::failure("Unsupported PSBT version number");
                    }
                    if (m_version == 1) {
                        throw std::ios_base::failure("There is no PSBT version 1");
                    }
                    break;
                }
                case PSBT_GLOBAL_PROPRIETARY:
                {
                    // Read in the value of the proprietary record
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    for (const auto& entry : parsed_proprietary) {
                        if (entry.first == key) {
                            throw std::ios_base::failure("Duplicate Key, proprietary key already provided");
                        }
                    }
                    parsed_proprietary.emplace_back(key, std::move(val_bytes));
                    break;
                }
                // Unknown stuff
                default: {
                    if (unknown.count(key) > 0) {
                        throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
                    }
                    // Read in the value
                    std::vector<unsigned char> val_bytes;
                    s >> val_bytes;
                    unknown.emplace(std::move(key), std::move(val_bytes));
                }
            }
        }

        if (!found_sep) {
            throw std::ios_base::failure("Separator is missing at the end of the global map");
        }

        const uint32_t psbt_version = GetVersion();

        if (psbt_version == 0) {
            // In a PSBTv0 the xpub and the proprietary records are unknown keys, they are kept
            // as they are to not change the byte stream of a PSBTv0.
            for (const auto& entry : raw_xpub_records) {
                if (unknown.count(entry.first) > 0) {
                    throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
                }
                unknown.emplace(entry.first, entry.second);
            }
            for (const auto& entry : parsed_proprietary) {
                if (unknown.count(entry.first) > 0) {
                    throw std::ios_base::failure("Duplicate Key, key for unknown value already provided");
                }
                unknown.emplace(entry.first, entry.second);
            }
        } else {
            m_xpubs = parsed_xpubs;
            for (const auto& entry : parsed_proprietary) {
                if (!proprietary.emplace(entry.first, entry.second).second) {
                    throw std::ios_base::failure("Duplicate Key, proprietary key already provided");
                }
            }
        }

        if (psbt_version == 0) {
            // Make sure that we got an unsigned tx for PSBTv0
            if (!tx) {
                throw std::ios_base::failure("No unsigned transcation was provided");
            }
            // Make sure that no PSBTv2 fields are present
            if (found_tx_version) {
                throw std::ios_base::failure("PSBT_GLOBAL_TX_VERSION is not allowed in PSBTv0");
            }
            if (found_fallback_locktime) {
                throw std::ios_base::failure("PSBT_GLOBAL_FALLBACK_LOCKTIME is not allowed in PSBTv0");
            }
            if (found_input_count) {
                throw std::ios_base::failure("PSBT_GLOBAL_INPUT_COUNT is not allowed in PSBTv0");
            }
            if (found_output_count) {
                throw std::ios_base::failure("PSBT_GLOBAL_OUTPUT_COUNT is not allowed in PSBTv0");
            }
            if (m_tx_modifiable) {
                throw std::ios_base::failure("PSBT_GLOBAL_TX_MODIFIABLE is not allowed in PSBTv0");
            }
        } else if (psbt_version == 2) {
            // The transaction version, the input count and the output count are required
            if (!found_tx_version) {
                throw std::ios_base::failure("PSBT_GLOBAL_TX_VERSION is required in PSBTv2");
            }
            if (!found_input_count) {
                throw std::ios_base::failure("PSBT_GLOBAL_INPUT_COUNT is required in PSBTv2");
            }
            if (!found_output_count) {
                throw std::ios_base::failure("PSBT_GLOBAL_OUTPUT_COUNT is required in PSBTv2");
            }
            // The unsigned transaction is not allowed in PSBTv2
            if (tx) {
                throw std::ios_base::failure("PSBT_GLOBAL_UNSIGNED_TX is not allowed in PSBTv2");
            }
        }

        // Read input data
        unsigned int i = 0;
        while (!s.empty() && i < input_count) {
            PSBTInput input;
            input.m_psbt_version = psbt_version;
            s >> input;
            inputs.push_back(input);

            // Make sure the non-witness utxo matches the outpoint
            if (input.non_witness_utxo) {
                const uint256& prevout_hash = psbt_version < 2 ? tx->vin[i].prevout.hash : input.prev_txid;
                if (input.non_witness_utxo->GetHash() != prevout_hash) {
                    throw std::ios_base::failure("Non-witness UTXO does not match outpoint hash");
                }
                const uint32_t prevout_index = psbt_version < 2 ? tx->vin[i].prevout.n : input.prev_out;
                if (prevout_index >= input.non_witness_utxo->vout.size()) {
                    throw std::ios_base::failure("Input specifies output index that does not exist");
                }
            }
            ++i;
        }
        // Make sure that the number of inputs matches the number of inputs in the transaction
        if (inputs.size() != input_count) {
            throw std::ios_base::failure("Inputs provided does not match the number of inputs in transaction.");
        }

        // Read output data
        i = 0;
        while (!s.empty() && i < output_count) {
            PSBTOutput output;
            output.m_psbt_version = psbt_version;
            s >> output;
            outputs.push_back(output);
            ++i;
        }
        // Make sure that the number of outputs matches the number of outputs in the transaction
        if (outputs.size() != output_count) {
            throw std::ios_base::failure("Outputs provided does not match the number of outputs in transaction.");
        }

        if (psbt_version == 2) {
            // Rebuild the unsigned transaction from the PSBTv2 data
            auto unsigned_tx = GetUnsignedTx();
            if (!unsigned_tx) {
                throw std::ios_base::failure("PSBTv2 does not contain the information to rebuild the transaction");
            }
            tx = std::move(*unsigned_tx);
            // Fill the output data of the outputs from the rebuilt transaction
            for (size_t out_idx = 0; out_idx < outputs.size(); ++out_idx) {
                outputs[out_idx].amount = tx->vout[out_idx].nValue;
                outputs[out_idx].script = tx->vout[out_idx].scriptPubKey;
                outputs[out_idx].token_id = outputs[out_idx].GetTokenId();
            }
        }

        // Sanity check
        if (!IsSane()) {
            throw std::ios_base::failure("PSBT is not sane.");
        }
    }

    template <typename Stream>
    PartiallySignedTransaction(deserialize_type, Stream& s) {
        Unserialize(s);
    }
};

enum class PSBTRole {
    UPDATER,
    SIGNER,
    FINALIZER,
    EXTRACTOR
};

std::string PSBTRoleName(PSBTRole role);

/** Checks whether a PSBTInput is already signed. */
bool PSBTInputSigned(const PSBTInput& input);

/** Signs a PSBTInput, verifying that all provided data matches what is being signed. */
bool SignPSBTInput(const SigningProvider& provider, PartiallySignedTransaction& psbt, int index, int sighash = SIGHASH_ALL, SignatureData* out_sigdata = nullptr, bool use_dummy = false);

/** Updates a PSBTOutput with information from provider.
 *
 * This fills in the redeem_script, witness_script, and hd_keypaths where possible.
 */
void UpdatePSBTOutput(const SigningProvider& provider, PartiallySignedTransaction& psbt, int index);

/**
 * Finalizes a PSBT if possible, combining partial signatures.
 *
 * @param[in,out] &psbtx reference to PartiallySignedTransaction to finalize
 * return True if the PSBT is now complete, false otherwise
 */
bool FinalizePSBT(PartiallySignedTransaction& psbtx);

/**
 * Finalizes a PSBT if possible, and extracts it to a CMutableTransaction if it could be finalized.
 *
 * @param[in]  &psbtx reference to PartiallySignedTransaction
 * @param[out] result CMutableTransaction representing the complete transaction, if successful
 * @return True if we successfully extracted the transaction, false otherwise
 */
bool FinalizeAndExtractPSBT(PartiallySignedTransaction& psbtx, CMutableTransaction& result);

/**
 * Combines PSBTs with the same underlying transaction, resulting in a single PSBT with all partial signatures from each input.
 *
 * @param[out] &out   the combined PSBT, if successful
 * @param[in]  psbtxs the PSBTs to combine
 * @return error (OK if we successfully combined the transactions, other error if they were not compatible)
 */
NODISCARD TransactionError CombinePSBTs(PartiallySignedTransaction& out, const std::vector<PartiallySignedTransaction>& psbtxs);

//! Decode a base64ed PSBT into a PartiallySignedTransaction
NODISCARD bool DecodeBase64PSBT(PartiallySignedTransaction& decoded_psbt, const std::string& base64_psbt, std::string& error);
//! Decode a raw (binary blob) PSBT into a PartiallySignedTransaction
NODISCARD bool DecodeRawPSBT(PartiallySignedTransaction& decoded_psbt, const std::string& raw_psbt, std::string& error);

#endif // DEFI_PSBT_H
