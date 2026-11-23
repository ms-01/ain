// Copyright (c) 2009-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>
#include <addrman.h>
#include <blockencodings.h>
#include <bloom.h>
#include <chain.h>
#include <coins.h>
#include <compressor.h>
#include <consensus/merkle.h>
#include <net.h>
#include <primitives/block.h>
#include <protocol.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>
#include <undo.h>
#include <version.h>

#include <cstdint>
#include <vector>

namespace {

bool MakeVersionedStream(FuzzBufferType buffer, CDataStream& ds)
{
    ds.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    try {
        int nVersion;
        ds >> nVersion;
        ds.SetVersion(nVersion);
    } catch (const std::ios_base::failure&) {
        return false;
    }
    return true;
}

template <typename T>
void FuzzDeserialize(FuzzBufferType buffer)
{
    CDataStream ds(SER_NETWORK, INIT_PROTO_VERSION);
    if (!MakeVersionedStream(buffer, ds)) return;
    try {
        T obj;
        ds >> obj;
    } catch (const std::ios_base::failure&) {
    }
}

} // namespace

FUZZ_TARGET(block_deserialize)
{
    FuzzDeserialize<CBlock>(buffer);
}

FUZZ_TARGET(transaction_deserialize)
{
    CDataStream ds(SER_NETWORK, INIT_PROTO_VERSION);
    if (!MakeVersionedStream(buffer, ds)) return;
    try {
        CTransaction tx(deserialize, ds);
        (void)tx;
    } catch (const std::ios_base::failure&) {
    }
}

FUZZ_TARGET(blocklocator_deserialize)
{
    FuzzDeserialize<CBlockLocator>(buffer);
}

FUZZ_TARGET(blockmerkleroot)
{
    CDataStream ds(SER_NETWORK, INIT_PROTO_VERSION);
    if (!MakeVersionedStream(buffer, ds)) return;
    try {
        CBlock block;
        ds >> block;
        bool mutated;
        BlockMerkleRoot(block, &mutated);
    } catch (const std::ios_base::failure&) {
    }
}

FUZZ_TARGET(addrman_deserialize)
{
    FuzzDeserialize<CAddrMan>(buffer);
}

FUZZ_TARGET(blockheader_deserialize)
{
    FuzzDeserialize<CBlockHeader>(buffer);
}

FUZZ_TARGET(banentry_deserialize)
{
    FuzzDeserialize<CBanEntry>(buffer);
}

FUZZ_TARGET(txundo_deserialize)
{
    FuzzDeserialize<CTxUndo>(buffer);
}

FUZZ_TARGET(blockundo_deserialize)
{
    FuzzDeserialize<CBlockUndo>(buffer);
}

FUZZ_TARGET(coins_deserialize)
{
    FuzzDeserialize<Coin>(buffer);
}

FUZZ_TARGET(netaddr_deserialize)
{
    FuzzDeserialize<CNetAddr>(buffer);
}

FUZZ_TARGET(service_deserialize)
{
    FuzzDeserialize<CService>(buffer);
}

FUZZ_TARGET(messageheader_deserialize)
{
    CDataStream ds(SER_NETWORK, INIT_PROTO_VERSION);
    if (!MakeVersionedStream(buffer, ds)) return;
    CMessageHeader::MessageStartChars pchMessageStart = {0x00, 0x00, 0x00, 0x00};
    try {
        CMessageHeader mh(pchMessageStart);
        ds >> mh;
        (void)mh.IsValid(pchMessageStart);
    } catch (const std::ios_base::failure&) {
    }
}

FUZZ_TARGET(address_deserialize)
{
    FuzzDeserialize<CAddress>(buffer);
}

FUZZ_TARGET(inv_deserialize)
{
    FuzzDeserialize<CInv>(buffer);
}

FUZZ_TARGET(bloomfilter_deserialize)
{
    FuzzDeserialize<CBloomFilter>(buffer);
}

FUZZ_TARGET(diskblockindex_deserialize)
{
    FuzzDeserialize<CDiskBlockIndex>(buffer);
}

FUZZ_TARGET(txoutcompressor_deserialize)
{
    CDataStream ds(SER_NETWORK, INIT_PROTO_VERSION);
    if (!MakeVersionedStream(buffer, ds)) return;
    CTxOut to;
    CTxOutCompressor toc(to);
    try {
        ds >> toc;
    } catch (const std::ios_base::failure&) {
    }
}

FUZZ_TARGET(blocktransactions_deserialize)
{
    FuzzDeserialize<BlockTransactions>(buffer);
}

FUZZ_TARGET(blocktransactionsrequest_deserialize)
{
    FuzzDeserialize<BlockTransactionsRequest>(buffer);
}
