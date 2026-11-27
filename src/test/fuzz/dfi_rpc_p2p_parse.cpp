// Copyright (c) 2026 The DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dfi/anchors.h>
#include <dfi/balances.h>
#include <key_io.h>
#include <rpc/util.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

void initialize_dfi_rpc_p2p_parse()
{
    SelectParams(CBaseChainParams::REGTEST);
}

} // namespace

FUZZ_TARGET(dfi_rpc_p2p_parse, .init = initialize_dfi_rpc_p2p_parse)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const uint8_t choice = fdp.ConsumeIntegralInRange<uint8_t>(0, 8);

    if (choice <= 3) {
        // High-risk RPC JSON and argument parsing paths (no disk/network I/O).
        const bool skip_money_range = fdp.ConsumeBool();
        const unsigned int max_target = fdp.ConsumeIntegralInRange<unsigned int>(1, 1008);
        const std::string input = fdp.ConsumeRemainingBytesAsString();

        switch (choice) {
        case 0: {
            UniValue uv;
            if (uv.read(input)) {
                try {
                    (void)AmountFromValue(uv, skip_money_range);
                } catch (const UniValue&) {
                }
                CAmount amount = 0;
                (void)AmountFromValue(uv, amount);
            }
            break;
        }
        case 1: {
            UniValue uv;
            if (uv.read(input)) {
                try {
                    (void)ParseHashV(uv, "hash");
                } catch (const UniValue&) {
                }
                try {
                    (void)ParseHexV(uv, "hex");
                } catch (const UniValue&) {
                }
                try {
                    (void)ParseConfirmTarget(uv, max_target);
                } catch (const UniValue&) {
                }
                try {
                    (void)ParsePSBTVersion(uv);
                } catch (const UniValue&) {
                }
                try {
                    (void)ParseDescriptorRange(uv);
                } catch (const UniValue&) {
                }
            }
            break;
        }
        case 2: {
            const CTxDestination dest = DecodeDestination(input);
            if (IsValidDestination(dest)) {
                (void)EncodeDestination(dest);
                (void)DescribeAddress(dest);
            }
            (void)IsValidDestinationString(input);
            break;
        }
        case 3: {
            try {
                (void)HexToPubKey(input);
            } catch (const UniValue&) {
            }
            int64_t val = 0;
            (void)ParseFixedPoint(input, 8, &val);
            break;
        }
        }
        return;
    }

    // High-risk DFI P2P anchor and balance message deserialization paths.
    const int proto_version = fdp.ConsumeIntegral<int>();
    const auto payload = fdp.ConsumeRemainingBytes<uint8_t>();
    CDataStream ds(SER_NETWORK, proto_version);
    if (!payload.empty()) {
        ds.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    }

    try {
        switch (choice) {
        case 4: {
            CAnchorData anchor_data;
            ds >> anchor_data;
            break;
        }
        case 5: {
            CAnchorConfirmData confirm_data;
            ds >> confirm_data;
            break;
        }
        case 6: {
            CAnchorFinalizationMessage fin_msg;
            ds >> fin_msg;
            break;
        }
        case 7: {
            CAnchorFinalizationMessagePlus fin_msg_plus;
            ds >> fin_msg_plus;
            break;
        }
        case 8: {
            CBalances balances;
            ds >> balances;
            (void)balances.ToString();
            break;
        }
        }
    } catch (const std::ios_base::failure&) {
    }
}
