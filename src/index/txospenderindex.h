// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_INDEX_TXOSPENDERINDEX_H
#define DEFI_INDEX_TXOSPENDERINDEX_H

#include <index/base.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

struct CDiskTxPos;

inline constexpr bool DEFAULT_TXOSPENDERINDEX{false};

/**
 * The transaction spending an output and the block it was confirmed in.
 */
struct TxoSpender {
    CTransactionRef tx;
    uint256 block_hash;
};

/**
 * TxoSpenderIndex is used to look up which transaction spent a given output.
 *
 * The index is written to a LevelDB database and, for each input of each
 * transaction in a block, records the spent outpoint together with the
 * location of the spending transaction on disk.
 */
class TxoSpenderIndex final : public BaseIndex
{
private:
    std::unique_ptr<BaseIndex::DB> m_db;
    std::pair<uint64_t, uint64_t> m_siphash_key;

    void WriteSpenderInfos(const std::vector<std::pair<COutPoint, CDiskTxPos>>& items);
    void EraseSpenderInfos(const std::vector<std::pair<COutPoint, CDiskTxPos>>& items);
    bool ReadTransaction(const CDiskTxPos& pos, TxoSpender& spender) const;
    bool EraseBlockEntries(const CBlockIndex* pindex);

protected:
    bool WriteBlock(const CBlock& block, const CBlockIndex* pindex) override;

    bool Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip) override;

    BaseIndex::DB& GetDB() const override;

    const char* GetName() const override { return "txospenderindex"; }

public:
    /** Constructs the index, which becomes available to be queried. */
    explicit TxoSpenderIndex(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    // Destructor is declared because this class contains a unique_ptr to an incomplete type.
    ~TxoSpenderIndex() override;

    /**
     * Search the index for a transaction that spends the given outpoint.
     *
     * @param[in]  txo       The outpoint to search for.
     * @param[out] error_out  Set to a descriptive message if the lookup failed
     *                        (e.g. an I/O or deserialization error).
     *
     * @return std::nullopt if the outpoint is not spent on-chain (or the index
     *         does not know about it), otherwise the spending transaction and
     *         the block it was confirmed in.
     */
    std::optional<TxoSpender> FindSpender(const COutPoint& txo, std::string* error_out = nullptr) const;
};

/// The global txo spender index. May be null.
extern std::unique_ptr<TxoSpenderIndex> g_txospenderindex;

#endif // DEFI_INDEX_TXOSPENDERINDEX_H
