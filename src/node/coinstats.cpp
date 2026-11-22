// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <node/coinstats.h>

#include <amount.h>
#include <coins.h>
#include <crypto/muhash.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <validation.h>
#include <uint256.h>
#include <util/system.h>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>


/** The legacy hash serializes the hashBlock before the UTXO set. */
static void PrepareHash(CHashWriter& ss, const CCoinsStats& stats)
{
    ss << stats.hashBlock;
}

/** MuHash does not need a prepare step. */
static void PrepareHash(MuHash3072&, const CCoinsStats&) {}

/** No hash at all. */
static void PrepareHash(std::nullptr_t, const CCoinsStats&) {}

/**
 * Serialize a single UTXO for MuHash purposes.
 *
 * This mirrors upstream's kernel::TxOutSer(): the *raw* (uncompressed) output
 * is hashed, so that the digest is comparable with other implementations.
 *
 * The token id of the output is serialized explicitly instead of relying on
 * CTxOut's own serialization: CTxOut omits the token id when the node runs
 * with -txnotokens, which would make the digest depend on the node's
 * configuration and thus not comparable between nodes.
 */
static std::vector<unsigned char> TxOutSer(const COutPoint& outpoint, const Coin& coin)
{
    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    ss << outpoint;
    ss << static_cast<uint32_t>(coin.nHeight * 2 + coin.fCoinBase);
    ss << coin.out.nValue;
    ss << coin.out.scriptPubKey;
    ss << VARINT(coin.out.nTokenId.v);
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

//! Apply the legacy (hash_serialized_2) hash to the outputs of a single transaction.
static void ApplyHash(CHashWriter& ss, const uint256& hash, const std::map<uint32_t, Coin>& outputs)
{
    assert(!outputs.empty());
    ss << hash;
    ss << VARINT(outputs.begin()->second.nHeight * 2 + outputs.begin()->second.fCoinBase ? 1u : 0u);
    for (const auto& output : outputs) {
        ss << VARINT(output.first + 1);
        ss << output.second.out.scriptPubKey;
        ss << VARINT(output.second.out.nValue, VarIntMode::NONNEGATIVE_SIGNED);
    }
    ss << VARINT(0u);
}

//! Apply the MuHash hash to the outputs of a single transaction.
static void ApplyHash(MuHash3072& muhash, const uint256& hash, const std::map<uint32_t, Coin>& outputs)
{
    for (const auto& output : outputs) {
        const COutPoint outpoint(hash, output.first);
        const std::vector<unsigned char> element = TxOutSer(outpoint, output.second);
        muhash.Insert(Span<const unsigned char>(element.data(), element.size()));
    }
}

//! No hash at all.
static void ApplyHash(std::nullptr_t, const uint256&, const std::map<uint32_t, Coin>&) {}

static void FinalizeHash(CHashWriter& ss, CCoinsStats& stats)
{
    stats.hashSerialized = ss.GetHash();
}

static void FinalizeHash(MuHash3072& muhash, CCoinsStats& stats)
{
    uint256 out;
    muhash.Finalize(out);
    stats.hashSerialized = out;
}

static void FinalizeHash(std::nullptr_t, CCoinsStats&) {}

static void ApplyStats(CCoinsStats& stats, const std::map<uint32_t, Coin>& outputs)
{
    assert(!outputs.empty());
    stats.nTransactions++;
    for (const auto& output : outputs) {
        stats.nTransactionOutputs++;
        stats.nTotalAmount += output.second.out.nValue;
        stats.nBogoSize += 32 /* txid */ + 4 /* vout index */ + 4 /* height + coinbase */ + 8 /* amount */ +
                           2 /* scriptPubKey len */ + output.second.out.scriptPubKey.size() /* scriptPubKey */;
    }
}

//! Calculate statistics about the unspent transaction output set
template <typename T>
static bool ComputeUTXOStats(CCoinsView* view, CCoinsStats& stats, T hash_obj, const std::function<void()>& interruption_point)
{
    std::unique_ptr<CCoinsViewCursor> pcursor(view->Cursor());
    assert(pcursor);

    stats.hashBlock = pcursor->GetBestBlock();
    {
        LOCK(cs_main);
        stats.nHeight = LookupBlockIndex(stats.hashBlock)->nHeight;
    }

    PrepareHash(hash_obj, stats);

    uint256 prevkey;
    std::map<uint32_t, Coin> outputs;
    while (pcursor->Valid()) {
        interruption_point();
        COutPoint key;
        Coin coin;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            if (!outputs.empty() && key.hash != prevkey) {
                ApplyStats(stats, outputs);
                ApplyHash(hash_obj, prevkey, outputs);
                outputs.clear();
            }
            prevkey = key.hash;
            outputs[key.n] = std::move(coin);
        } else {
            return error("%s: unable to read value", __func__);
        }
        pcursor->Next();
    }
    if (!outputs.empty()) {
        ApplyStats(stats, outputs);
        ApplyHash(hash_obj, prevkey, outputs);
    }

    FinalizeHash(hash_obj, stats);

    stats.nDiskSize = view->EstimateSize();
    return true;
}

//! Calculate statistics about the unspent transaction output set
bool GetUTXOStats(CCoinsView* view, CCoinsStats& stats, CoinStatsHashType hash_type, const std::function<void()>& interruption_point)
{
    switch (hash_type) {
    case CoinStatsHashType::HASH_SERIALIZED: {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        return ComputeUTXOStats(view, stats, ss, interruption_point);
    }
    case CoinStatsHashType::MUHASH: {
        MuHash3072 muhash;
        return ComputeUTXOStats(view, stats, muhash, interruption_point);
    }
    case CoinStatsHashType::NONE: {
        return ComputeUTXOStats(view, stats, nullptr, interruption_point);
    }
    } // no default case, so the compiler can warn about missing cases
    assert(false);
    return false;
}
