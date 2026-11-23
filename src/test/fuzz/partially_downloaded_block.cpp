// Copyright (c) 2020-2026 The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <blockencodings.h>
#include <chainparams.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <txmempool.h>
#include <version.h>

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

void initialize_partially_downloaded_block()
{
    SelectParams(CBaseChainParams::REGTEST);
}

bool ConsumeTransaction(FuzzedDataProvider& fdp, CTransactionRef& out)
{
    const auto bytes = fdp.ConsumeBytes<uint8_t>(fdp.ConsumeIntegralInRange<size_t>(0, 4096));
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    if (!bytes.empty()) {
        ds.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    try {
        CMutableTransaction mtx;
        ds >> mtx;
        out = MakeTransactionRef(std::move(mtx));
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

} // namespace

FUZZ_TARGET(partially_downloaded_block, .init = initialize_partially_downloaded_block)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const auto cmpct_bytes = fdp.ConsumeBytes<uint8_t>(fdp.ConsumeIntegralInRange<size_t>(0, 16384));
    CDataStream cmpct_stream(SER_NETWORK, PROTOCOL_VERSION);
    if (!cmpct_bytes.empty()) {
        cmpct_stream.write(reinterpret_cast<const char*>(cmpct_bytes.data()), cmpct_bytes.size());
    }

    CBlockHeaderAndShortTxIDs cmpctblock;
    try {
        cmpct_stream >> cmpctblock;
    } catch (const std::ios_base::failure&) {
        return;
    }

    std::vector<std::pair<uint256, CTransactionRef>> extra_txn;
    LIMITED_WHILE(fdp.ConsumeBool(), 16) {
        CTransactionRef tx;
        if (ConsumeTransaction(fdp, tx)) {
            extra_txn.emplace_back(tx->GetWitnessHash(), std::move(tx));
        }
    }

    CTxMemPool pool;
    PartiallyDownloadedBlock pdb(&pool);
    const ReadStatus init_status = pdb.InitData(cmpctblock, extra_txn);
    if (init_status != READ_STATUS_OK) {
        return;
    }

    std::vector<CTransactionRef> missing;
    const size_t tx_count = cmpctblock.BlockTxCount();
    for (size_t i = 0; i < tx_count; ++i) {
        if (!pdb.IsTxAvailable(i)) {
            CTransactionRef tx;
            if (ConsumeTransaction(fdp, tx)) {
                missing.push_back(std::move(tx));
            }
        }
    }

    CBlock block;
    const int height = fdp.ConsumeIntegralInRange<int>(0, 1000000);
    (void)pdb.FillBlock(block, missing, height);
}
