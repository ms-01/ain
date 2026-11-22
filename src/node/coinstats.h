// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NODE_COINSTATS_H
#define DEFI_NODE_COINSTATS_H

#include <amount.h>
#include <uint256.h>

#include <cstdint>
#include <functional>

class CCoinsView;

/** Which UTXO set hash should be calculated. */
enum class CoinStatsHashType {
    //! Legacy double SHA-256 over the serialized UTXO set (hash_serialized_2).
    HASH_SERIALIZED,
    //! MuHash3072 over the UTXO set.
    MUHASH,
    //! No UTXO set hash at all.
    NONE,
};

struct CCoinsStats
{
    int nHeight;
    uint256 hashBlock;
    uint64_t nTransactions;
    uint64_t nTransactionOutputs;
    uint64_t nBogoSize;
    uint256 hashSerialized;
    uint64_t nDiskSize;
    CAmount nTotalAmount;

    CCoinsStats() : nHeight(0), nTransactions(0), nTransactionOutputs(0), nBogoSize(0), nDiskSize(0), nTotalAmount(0) {}
};

//! Calculate statistics about the unspent transaction output set
bool GetUTXOStats(CCoinsView* view, CCoinsStats& stats, CoinStatsHashType hash_type = CoinStatsHashType::HASH_SERIALIZED, const std::function<void()>& interruption_point = {});

#endif // DEFI_NODE_COINSTATS_H
