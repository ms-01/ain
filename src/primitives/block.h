// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_PRIMITIVES_BLOCK_H
#define DEFI_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <pubkey.h>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 *
 * A block header is immutable once constructed: it does not carry any mutable or cached state.
 * This makes a CBlock/CBlockHeader safe to share between threads via a
 * std::shared_ptr<const CBlock>, as the block objects are handed out to the net thread, the
 * validation thread and the RPC layer concurrently.
 */
class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;

    uint64_t deprecatedHeight;
    uint64_t mintedBlocks;
    uint256 stakeModifier;
    std::vector<unsigned char> sig;

    CBlockHeader()
    {
        SetNull();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(this->nVersion);
        READWRITE(hashPrevBlock);
        READWRITE(hashMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(stakeModifier);
        READWRITE(deprecatedHeight);
        READWRITE(mintedBlocks);
        READWRITE(sig);
    }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        stakeModifier.SetNull();
        deprecatedHeight = 0;
        mintedBlocks = 0;
        sig = {};
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHashToSign() const;

    uint256 GetHash() const;

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }

    /**
     * Recover the public key that signed this header and return its key id.
     *
     * The key is recovered on demand into a local variable: the header itself is never
     * modified, so this method is pure, re-entrant and safe to call concurrently on a shared
     * block instance.
     */
    bool ExtractMinterKey(CKeyID &key) const
    {
        CPubKey recoveredPubKey;
        if (!recoveredPubKey.RecoverCompact(GetHashToSign(), sig)) {
            return false;
        }

        key = recoveredPubKey.GetID();
        return true;
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITEAS(CBlockHeader, *this);
        READWRITE(vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.stakeModifier  = stakeModifier;
        block.deprecatedHeight = deprecatedHeight;
        block.mintedBlocks   = mintedBlocks;
        block.sig            = sig;

        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    std::vector<uint256> vHave;

    CBlockLocator() {}

    explicit CBlockLocator(const std::vector<uint256>& vHaveIn) : vHave(vHaveIn) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // DEFI_PRIMITIVES_BLOCK_H
