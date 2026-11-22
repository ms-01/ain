// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_WALLET_WALLETDESCRIPTORS_H
#define DEFI_WALLET_WALLETDESCRIPTORS_H

#include <script/descriptor.h>
#include <script/keyorigin.h>
#include <script/signingprovider.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class CKey;
class CPubKey;

/**
 * Expand a descriptor over the given range of indices and collect the key
 * material (scripts, public keys, private keys and key origins) that is
 * required to import the descriptor into a wallet.
 *
 * This is the shared code path of `importmulti` (descriptor branch) and
 * `importdescriptors`, so that both RPCs import the very same key material
 * for a descriptor.
 */
bool ExpandDescriptorForImport(const Descriptor& descriptor,
                                const FlatSigningProvider& provider,
                                int64_t range_begin,
                                int64_t range_end,
                                std::set<CScript>& script_pub_keys,
                                std::set<CScript>& import_scripts,
                                std::map<CKeyID, CPubKey>& pubkey_map,
                                std::map<CKeyID, CKey>& privkey_map,
                                std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins,
                                std::vector<CKeyID>& ordered_pubkeys);

/**
 * Determine the DeFi Blockchain output type of a descriptor script.
 *
 * Returns -1 when the script does not correspond to an address type that is
 * supported by the wallet (for example bare multisig or P2WSH scripts).
 */
int8_t GetWalletOutputType(const CScript& script);

#endif // DEFI_WALLET_WALLETDESCRIPTORS_H
