// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <index/txospenderindex.h>

#include <chainparams.h>
#include <crypto/siphash.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <index/base.h>
#include <index/disktxpos.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/system.h>
#include <validation.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* The database is used to find the spending transaction of a given utxo.
 * For every input of every transaction it stores a key that is a
 * pair(siphash(input outpoint), transaction location on disk) and a zero-byte
 * value. To find the spending transaction of an outpoint, we perform a range
 * query on siphash(outpoint), and for each returned key load the transaction
 * and return it if it does spend the provided outpoint.
 */

// LevelDB key prefix. We only have one key for now but it will make it easier to add others if needed.
constexpr uint8_t DB_TXOSPENDERINDEX{'s'};

std::unique_ptr<TxoSpenderIndex> g_txospenderindex;

struct DBKey {
    uint64_t hash;
    CDiskTxPos pos;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        uint8_t prefix{DB_TXOSPENDERINDEX};
        READWRITE(prefix);
        READWRITE(hash);
        READWRITE(pos);
    }

    DBKey() : hash{0} {}
    DBKey(const uint64_t& hash_in, const CDiskTxPos& pos_in) : hash(hash_in), pos(pos_in) {}
};

static uint64_t CreateKeyPrefix(const std::pair<uint64_t, uint64_t>& siphash_key, const COutPoint& vout)
{
    return SipHashUint256Extra(siphash_key.first, siphash_key.second, vout.hash, vout.n);
}

static DBKey CreateKey(const std::pair<uint64_t, uint64_t>& siphash_key, const COutPoint& vout, const CDiskTxPos& pos)
{
    return DBKey(CreateKeyPrefix(siphash_key, vout), pos);
}

/**
 * Collect the position of every transaction of the given block that spends an
 * output. The offsets are computed in the very same way as the transaction
 * index does it, so that the transactions can be read back from disk.
 */
static std::vector<std::pair<COutPoint, CDiskTxPos>> BuildSpenderPositions(const CBlock& block, const FlatFilePos& block_pos)
{
    std::vector<std::pair<COutPoint, CDiskTxPos>> items;
    items.reserve(block.vtx.size());

    CDiskTxPos pos(block_pos, GetSizeOfCompactSize(block.vtx.size()));
    for (const auto& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const auto& input : tx->vin) {
                items.emplace_back(input.prevout, pos);
            }
        }
        pos.nTxOffset += ::GetSerializeSize(*tx, CLIENT_VERSION);
    }

    return items;
}

TxoSpenderIndex::TxoSpenderIndex(size_t n_cache_size, bool f_memory, bool f_wipe)
    : m_db(std::make_unique<BaseIndex::DB>(GetDataDir() / "indexes" / "txospenderindex", n_cache_size, f_memory, f_wipe))
{
    // The siphash key is stored with an explicit std::string key, so that the
    // serialized key does not depend on the address of a string literal.
    if (!m_db->Read(std::string("siphash_key"), m_siphash_key)) {
        FastRandomContext rng(false);
        m_siphash_key = {rng.rand64(), rng.rand64()};
        m_db->Write(std::string("siphash_key"), m_siphash_key, /*fSync=*/true);
    }
}

TxoSpenderIndex::~TxoSpenderIndex() {}

bool TxoSpenderIndex::Init()
{
    // If the chain reorganised while the index was not running, BaseIndex::Init()
    // moves the best block back to the fork point without erasing entries of the
    // orphaned blocks. Clean them up before BaseIndex::Init() updates the best block.
    CBlockLocator locator;
    std::vector<const CBlockIndex*> orphaned_blocks;
    if (GetDB().ReadBestBlock(locator) && !locator.IsNull() && !locator.vHave.empty()) {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(locator.vHave.front());
        while (pindex != nullptr && !::ChainActive().Contains(pindex)) {
            orphaned_blocks.push_back(pindex);
            pindex = pindex->pprev;
        }
    }

    for (const CBlockIndex* pindex : orphaned_blocks) {
        if (!EraseBlockEntries(pindex)) {
            LogPrintf("Warning: %s failed to erase orphaned block %s during init\n",
                      GetName(), pindex->GetBlockHash().ToString());
        }
    }

    if (!BaseIndex::Init()) {
        return false;
    }

    if (!orphaned_blocks.empty()) {
        Commit();
    }

    return true;
}

void TxoSpenderIndex::WriteSpenderInfos(const std::vector<std::pair<COutPoint, CDiskTxPos>>& items)
{
    CDBBatch batch(*m_db);
    for (const auto& item : items) {
        DBKey key(CreateKey(m_siphash_key, item.first, item.second));
        // The key encodes the spent outpoint hash and disk position. The value is only a marker.
        batch.Write(key, std::string{});
    }
    m_db->WriteBatch(batch);
}

void TxoSpenderIndex::EraseSpenderInfos(const std::vector<std::pair<COutPoint, CDiskTxPos>>& items)
{
    CDBBatch batch(*m_db);
    for (const auto& item : items) {
        DBKey key(CreateKey(m_siphash_key, item.first, item.second));
        batch.Erase(key);
    }
    m_db->WriteBatch(batch);
}

bool TxoSpenderIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
{
    // Exclude genesis block transaction because outputs are not spendable.
    if (pindex->nHeight == 0) return true;

    const auto items = BuildSpenderPositions(block, pindex->GetBlockPos());
    if (items.empty()) return true;

    WriteSpenderInfos(items);
    return true;
}

bool TxoSpenderIndex::EraseBlockEntries(const CBlockIndex* pindex)
{
    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
        return error("%s: failed to read block %s from disk", __func__, pindex->GetBlockHash().ToString());
    }

    return EraseBlockEntries(block, pindex);
}

bool TxoSpenderIndex::EraseBlockEntries(const CBlock& block, const CBlockIndex* pindex)
{
    EraseSpenderInfos(BuildSpenderPositions(block, pindex->GetBlockPos()));
    return true;
}

void TxoSpenderIndex::BlockDisconnected(const std::shared_ptr<const CBlock>& block)
{
    if (!IsSynced()) {
        return;
    }

    const CBlockIndex* pindex;
    {
        // The callback is executed by the validation interface scheduler
        // thread, so cs_main has to be taken for the block index lookup.
        LOCK(cs_main);
        pindex = LookupBlockIndex(block->GetHash());
    }
    if (pindex == nullptr) {
        return;
    }

    // Erase the entries of the disconnected block. The in-memory block is used
    // on purpose: the erase must not depend on the block still being readable
    // from disk.
    EraseBlockEntries(*block, pindex);

    // Move the index best block back to the parent of the disconnected block,
    // but only if the index is in sync with exactly that block. If the index is
    // behind the chain, the sync thread stays in charge of the index state.
    if (GetBestBlockIndex() != pindex) {
        return;
    }
    const CBlockIndex* parent{pindex->pprev};
    if (parent == nullptr) {
        return;
    }
    SetBestBlockIndex(parent);
    if (!Commit()) {
        // If the commit fails, the persisted locator is stale. Restore the
        // previous best block, the sync thread will then rewind the index.
        SetBestBlockIndex(pindex);
    }
}

bool TxoSpenderIndex::Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip)
{
    assert(current_tip->GetAncestor(new_tip->nHeight) == new_tip);

    // Erase the entries of all blocks that are disconnected by the reorg. This
    // has to happen before the new best block locator is committed.
    const CBlockIndex* pindex = current_tip;
    while (pindex != nullptr && pindex->nHeight > new_tip->nHeight) {
        if (!EraseBlockEntries(pindex)) {
            return false;
        }
        pindex = pindex->pprev;
    }

    return BaseIndex::Rewind(current_tip, new_tip);
}

bool TxoSpenderIndex::ReadTransaction(const CDiskTxPos& tx_pos, TxoSpender& spender) const
{
    CAutoFile file(OpenBlockFile(tx_pos, true), SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        return error("%s: OpenBlockFile failed", __func__);
    }

    CBlockHeader header;
    try {
        file >> header;
        if (fseek(file.Get(), tx_pos.nTxOffset, SEEK_CUR)) {
            return error("%s: fseek(...) failed", __func__);
        }
        file >> spender.tx;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    spender.block_hash = header.GetHash();
    return true;
}

std::optional<TxoSpender> TxoSpenderIndex::FindSpender(const COutPoint& txo, std::string* error_out) const
{
    const uint64_t prefix{CreateKeyPrefix(m_siphash_key, txo)};
    std::unique_ptr<CDBIterator> it(m_db->NewIterator());
    DBKey key(prefix, CDiskTxPos());

    // Find all keys that start with the outpoint hash, load the transaction at
    // the location specified in the key and return it if it does spend the
    // provided outpoint.
    for (it->Seek(std::make_pair(DB_TXOSPENDERINDEX, prefix)); it->Valid() && it->GetKey(key) && key.hash == prefix; it->Next()) {
        TxoSpender spender;
        if (!ReadTransaction(key.pos, spender)) {
            if (error_out != nullptr) {
                *error_out = strprintf("IO error finding spending tx for outpoint %s:%d.", txo.hash.GetHex(), txo.n);
            }
            return std::nullopt;
        }
        for (const auto& input : spender.tx->vin) {
            if (input.prevout == txo) {
                bool in_active_chain{false};
                {
                    LOCK(cs_main);
                    const CBlockIndex* pindex = LookupBlockIndex(spender.block_hash);
                    in_active_chain = pindex != nullptr && ::ChainActive().Contains(pindex);
                }
                if (in_active_chain) {
                    return spender;
                }
                break;
            }
        }
    }

    return std::nullopt;
}

BaseIndex::DB& TxoSpenderIndex::GetDB() const { return *m_db; }
