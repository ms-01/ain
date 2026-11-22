// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/walletdescriptors.h>

#include <outputtype.h>
#include <script/standard.h>

#include <cstdint>

bool ExpandDescriptorForImport(const Descriptor& descriptor,
                                const FlatSigningProvider& provider,
                                int64_t range_begin,
                                int64_t range_end,
                                std::set<CScript>& script_pub_keys,
                                std::set<CScript>& import_scripts,
                                std::map<CKeyID, CPubKey>& pubkey_map,
                                std::map<CKeyID, CKey>& privkey_map,
                                std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins,
                                std::vector<CKeyID>& ordered_pubkeys)
{
    // Expand all descriptors to get public keys and scripts, and private keys if available.
    for (int64_t i = range_begin; i <= range_end; ++i) {
        FlatSigningProvider out_keys;
        std::vector<CScript> scripts_temp;
        if (!descriptor.Expand(static_cast<int>(i), provider, scripts_temp, out_keys)) {
            return false;
        }
        std::copy(scripts_temp.begin(), scripts_temp.end(), std::inserter(script_pub_keys, script_pub_keys.end()));
        for (const auto& key_pair : out_keys.pubkeys) {
            ordered_pubkeys.push_back(key_pair.first);
        }

        for (const auto& x : out_keys.scripts) {
            import_scripts.emplace(x.second);
        }

        descriptor.ExpandPrivate(static_cast<int>(i), provider, out_keys);

        std::copy(out_keys.pubkeys.begin(), out_keys.pubkeys.end(), std::inserter(pubkey_map, pubkey_map.end()));
        for (const auto& [key, value] : out_keys.keys) {
            privkey_map.emplace(key, value);
        }
        key_origins.insert(out_keys.origins.begin(), out_keys.origins.end());
    }
    return true;
}

int8_t GetWalletOutputType(const CScript& script)
{
    CTxDestination dest;
    if (!ExtractDestination(script, dest)) {
        return -1;
    }
    if (std::holds_alternative<PKHash>(dest)) {
        return static_cast<int8_t>(OutputType::LEGACY);
    }
    if (std::holds_alternative<WitnessV0KeyHash>(dest)) {
        return static_cast<int8_t>(OutputType::BECH32);
    }
    if (std::holds_alternative<ScriptHash>(dest)) {
        return static_cast<int8_t>(OutputType::P2SH_SEGWIT);
    }
    return -1;
}
