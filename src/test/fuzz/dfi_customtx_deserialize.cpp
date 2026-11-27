// Copyright (c) 2026 The DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dfi/customtx.h>
#include <dfi/mn_checks.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

void initialize_dfi_customtx_deserialize()
{
    SelectParams(CBaseChainParams::REGTEST);
}

} // namespace

FUZZ_TARGET(dfi_customtx_deserialize, .init = initialize_dfi_customtx_deserialize)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const uint8_t mode = fdp.ConsumeIntegralInRange<uint8_t>(0, 2);
    const uint32_t height = fdp.ConsumeIntegral<uint32_t>();
    const bool metadata_validation = fdp.ConsumeBool();
    const auto& consensus = Params().GetConsensus();

    if (mode == 0) {
        // Direct CustomTxType + metadata deserialization via CustomMetadataParse.
        const uint8_t type_byte = fdp.ConsumeIntegral<uint8_t>();
        const CustomTxType tx_type = CustomTxCodeToType(type_byte);
        (void)ToString(tx_type);
        (void)FromString(ToString(tx_type));

        const auto raw_payload = fdp.ConsumeRemainingBytes<unsigned char>();
        CCustomTxMessage tx_msg = customTypeToMessage(tx_type);
        (void)CustomMetadataParse(height, consensus, raw_payload, tx_msg);
        return;
    }

    if (mode == 1) {
        // Construct a synthetic OP_RETURN transaction with optional DfTx marker.
        const bool prepend_marker = fdp.ConsumeBool();
        const uint8_t type_byte = fdp.ConsumeIntegral<uint8_t>();
        const auto payload = fdp.ConsumeRemainingBytes<unsigned char>();

        std::vector<unsigned char> data;
        if (prepend_marker) {
            data.insert(data.end(), DfTxMarker.begin(), DfTxMarker.end());
            data.push_back(type_byte);
        }
        data.insert(data.end(), payload.begin(), payload.end());

        CMutableTransaction mtx;
        mtx.nVersion = fdp.ConsumeIntegral<int32_t>();
        CScript script;
        script << OP_RETURN << data;
        mtx.vout.emplace_back(0, script);

        const CTransaction tx(mtx);
        std::vector<unsigned char> metadata;
        const CustomTxType tx_type = GuessCustomTxType(tx, metadata, metadata_validation);
        if (tx_type != CustomTxType::None && tx_type != CustomTxType::Reject) {
            CCustomTxMessage tx_msg = customTypeToMessage(tx_type);
            (void)CustomMetadataParse(height, consensus, metadata, tx_msg);
        }
        return;
    }

    // Mode 2: Full CTransaction deserialization followed by CustomTx detection & parsing.
    const int proto_version = fdp.ConsumeIntegral<int>();
    const auto raw_tx = fdp.ConsumeRemainingBytes<uint8_t>();
    CDataStream ds(SER_NETWORK, proto_version);
    if (!raw_tx.empty()) {
        ds.write(reinterpret_cast<const char*>(raw_tx.data()), raw_tx.size());
    }

    try {
        CMutableTransaction mtx;
        ds >> mtx;
        const CTransaction tx(mtx);
        (void)GetNonMintedValuesOut(tx);

        std::vector<unsigned char> metadata;
        const CustomTxType tx_type = GuessCustomTxType(tx, metadata, metadata_validation);
        if (tx_type != CustomTxType::None && tx_type != CustomTxType::Reject) {
            CCustomTxMessage tx_msg = customTypeToMessage(tx_type);
            (void)CustomMetadataParse(height, consensus, metadata, tx_msg);
        }
    } catch (const std::ios_base::failure&) {
    }
}
