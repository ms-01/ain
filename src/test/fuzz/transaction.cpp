// Copyright (c) 2019-2026 The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <cassert>
#include <cstdint>

FUZZ_TARGET(transaction)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const int ser_version = fdp.ConsumeBool() ? (PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) : PROTOCOL_VERSION;
    const auto raw_tx = fdp.ConsumeRemainingBytes<uint8_t>();

    CDataStream ds(SER_NETWORK, ser_version);
    if (!raw_tx.empty()) {
        ds.write(reinterpret_cast<const char*>(raw_tx.data()), raw_tx.size());
    }

    CMutableTransaction mtx;
    try {
        ds >> mtx;
    } catch (const std::ios_base::failure&) {
        return;
    }

    const CTransaction tx(mtx);
    (void)tx.GetHash();
    (void)tx.GetWitnessHash();
    (void)tx.IsCoinBase();
    (void)tx.HasWitness();
    (void)GetTransactionWeight(tx);
    (void)GetVirtualTransactionSize(tx);

    CValidationState state;
    (void)CheckTransaction(tx, state);

    // Round-trip re-serialization must yield an identical transaction.
    CDataStream reserialized(SER_NETWORK, ser_version);
    reserialized << tx;
    CMutableTransaction roundtrip;
    reserialized >> roundtrip;
    assert(CTransaction(roundtrip) == tx);
}
