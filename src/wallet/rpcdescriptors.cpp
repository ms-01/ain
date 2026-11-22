// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/keyorigin.h>
#include <script/standard.h>
#include <sync.h>
#include <util/system.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletdescriptors.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <univalue.h>

namespace {

/**
 * Read the timestamp of an import request without throwing.
 *
 * Returns false and fills `error` when the timestamp is missing or invalid.
 */
bool TryGetImportTimestamp(const UniValue& data, int64_t now, int64_t& timestamp, std::string& error)
{
    if (!data.exists("timestamp")) {
        error = "Missing required timestamp field for key";
        return false;
    }
    const UniValue& value = data["timestamp"];
    if (value.isNum()) {
        timestamp = value.getInt<int64_t>();
        return true;
    }
    if (value.isStr() && value.get_str() == "now") {
        timestamp = now;
        return true;
    }
    error = strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(value.type()));
    return false;
}

/**
 * Import a single descriptor into the wallet.
 *
 * The function never aborts the whole request, every failure is reported as a
 * per item error object (the same way `importmulti` reports its results).
 */
UniValue ProcessDescriptorImport(CWallet* const pwallet, const UniValue& data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    UniValue warnings(UniValue::VARR);
    UniValue result(UniValue::VOBJ);

    try {
        if (!data.exists("desc")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor not found.");
        }
        if (!data.exists("timestamp")) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
        }
        const std::string& descriptor = data["desc"].get_str();
        const bool active = data.exists("active") ? data["active"].get_bool() : false;
        const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
        const std::string& label = data.exists("label") ? data["label"].get_str() : "";

        // Parse the descriptor string, the checksum is mandatory.
        FlatSigningProvider keys;
        std::string error;
        auto parsed_desc = Parse(descriptor, keys, error, /* require_checksum = */ true);
        if (!parsed_desc) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
        }
        if (!parsed_desc->IsSolvable()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Descriptor is not solvable");
        }

        // Range handling.
        const bool ranged = parsed_desc->IsRange();
        int64_t range_start = 0;
        int64_t range_end = 0;
        int64_t next_index = 0;
        if (!ranged) {
            if (data.exists("range")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for an un-ranged descriptor");
            }
        } else {
            if (data.exists("range")) {
                std::tie(range_start, range_end) = ParseDescriptorRange(data["range"]);
            } else {
                warnings.push_back("Range not given, using default keypool range");
                range_start = 0;
                range_end = static_cast<int64_t>(gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE)) - 1;
            }
            next_index = range_start;
        }

        // An active descriptor derives new addresses, a non ranged descriptor
        // would always derive the very same address.
        if (active && !ranged) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Active descriptors must be ranged");
        }

        // next_index is only meaningful for an active ranged descriptor.
        if (data.exists("next_index")) {
            if (!active || !ranged) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "next_index is only allowed for active ranged descriptors");
            }
            next_index = data["next_index"].getInt<int64_t>();
            if (next_index < range_start || next_index > range_end) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "next_index is out of range");
            }
        }

        // Labels are not allowed for internal or ranged descriptors.
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal addresses should not have a label");
        }
        if (ranged && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptors should not have a label");
        }

        // A wallet with disabled private keys must not import private keys.
        if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && !keys.keys.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
        }

        // Collect the key material of the descriptor.
        std::set<CScript> script_pub_keys;
        std::set<CScript> import_scripts;
        std::map<CKeyID, CPubKey> pubkey_map;
        std::map<CKeyID, CKey> privkey_map;
        std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>> key_origins;
        std::vector<CKeyID> ordered_pubkeys;
        if (!ExpandDescriptorForImport(*parsed_desc, keys, range_start, range_end, script_pub_keys, import_scripts, pubkey_map, privkey_map, key_origins, ordered_pubkeys)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot expand descriptor. Probably because of hardened derivations without private keys provided");
        }

        // The output type of the descriptor decides for which address type the
        // descriptor can be used.
        int8_t output_type = -1;
        {
            FlatSigningProvider expand_keys;
            std::vector<CScript> first_scripts;
            if (parsed_desc->Expand(static_cast<int>(range_start), keys, first_scripts, expand_keys) && !first_scripts.empty()) {
                output_type = GetWalletOutputType(first_scripts[0]);
            }
        }

        const std::string canonical_desc = parsed_desc->ToString();

        // Only one active descriptor per address type and externality is allowed.
        if (active) {
            if (output_type < 0) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Descriptor has no address type that is supported by the wallet and cannot be set to active");
            }
            for (const auto& entry : pwallet->GetDescriptors()) {
                const WalletDescriptor& record = entry.second;
                if (record.desc == canonical_desc) continue;
                if (!record.active) continue;
                if (record.internal != internal) continue;
                if (record.output_type != output_type) continue;
                throw JSONRPCError(RPC_INVALID_PARAMETER, "There is already an active descriptor for this address type");
            }
        }

        // Import the key material of the descriptor into the wallet.
        pwallet->MarkDirty();
        if (!pwallet->ImportScripts(import_scripts, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding script to wallet");
        }
        if (!pwallet->ImportPrivKeys(privkey_map, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
        }
        if (!pwallet->ImportPubKeys(ordered_pubkeys, pubkey_map, key_origins, /* add_keypool = */ false, internal, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
        }
        if (!pwallet->ImportScriptPubKeys(label, script_pub_keys, /* have_solving_data = */ true, /* apply_label = */ !internal, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
        }

        WalletDescriptor record;
        record.desc = canonical_desc;
        record.active = active;
        record.internal = internal;
        record.range_begin = ranged ? range_start : -1;
        record.range_end = ranged ? range_end : -1;
        record.next_index = ranged ? next_index : 0;
        record.timestamp = timestamp;
        record.label = internal ? "" : label;
        record.output_type = output_type;

        // Store the private descriptor when the descriptor carries private keys.
        std::string private_desc;
        if (!keys.keys.empty() && parsed_desc->ToPrivateString(keys, private_desc)) {
            std::string encrypted_private;
            if (!pwallet->EncryptDescriptorString(canonical_desc, private_desc, encrypted_private)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error encrypting the private descriptor");
            }
            record.desc_private = encrypted_private;
            record.encrypted = pwallet->IsCrypted();
        } else {
            // Keep a previously stored private descriptor when the descriptor is
            // imported again without private key material.
            const std::map<std::string, WalletDescriptor> descriptors = pwallet->GetDescriptors();
            const auto existing = descriptors.find(canonical_desc);
            if (existing != descriptors.end() && !existing->second.desc_private.empty()) {
                record.desc_private = existing->second.desc_private;
                record.encrypted = existing->second.encrypted;
            }
        }

        if (!pwallet->AddDescriptor(record)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding descriptor to wallet");
        }

        result.pushKV("success", UniValue(true));
    } catch (const UniValue& e) {
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
    } catch (...) {
        result.pushKV("success", UniValue(false));
        result.pushKV("error", JSONRPCError(RPC_MISC_ERROR, "Missing required fields"));
    }
    if (warnings.size()) result.pushKV("warnings", warnings);
    return result;
}

} // namespace

UniValue importdescriptors(const JSONRPCRequest& mainRequest)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(mainRequest);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, mainRequest.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"importdescriptors",
                "\nImport descriptors into the wallet. This will trigger a rescan of the blockchain based on the earliest timestamp of all descriptors being imported. Requires a new wallet backup.\n"
                "The descriptors are stored in the wallet and are listed by 'listdescriptors'.\n"
                "Note: This call can take over an hour to complete if using an early timestamp; during that time, other rpc calls\n"
                "may report that the imported keys, addresses or scripts exist but related transactions are still missing.\n",
                {
                    {"requests", RPCArg::Type::ARR, RPCArg::Optional::NO, "Data to be imported",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "Descriptor to import. The descriptor checksum is mandatory."},
                                    {"active", RPCArg::Type::BOOL, /* default */ "false", "Set this descriptor to be the active descriptor for the corresponding address type/externality"},
                                    {"range", RPCArg::Type::RANGE, RPCArg::Optional::OMITTED, "If a ranged descriptor is used, this specifies the end or the range (in the form [begin,end]) to import"},
                                    {"next_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If a ranged descriptor is set to active, this specifies the next index to generate addresses from"},
                                    {"timestamp", RPCArg::Type::NUM, RPCArg::Optional::NO, "Time from which to start rescanning the blockchain for this descriptor, in seconds since epoch (Jan 1 1970 GMT)\n"
        "                                                              Use the string \"now\" to substitute the current synced blockchain time.\n"
        "                                                              \"now\" can be specified to bypass scanning, for outputs which are known to never have been used, and\n"
        "                                                              0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest timestamp\n"
        "                                                              of all descriptors being imported will be scanned.",
                                        /* oneline_description */ "", {"timestamp | \"now\"", "integer / string"}
                                    },
                                    {"internal", RPCArg::Type::BOOL, /* default */ "false", "Whether matching outputs should be treated as not incoming payments (e.g. change)"},
                                    {"label", RPCArg::Type::STR, /* default */ "''", "Label to assign to the address, only allowed with internal=false"},
                                },
                            },
                        },
                        "\"requests\""},
                },
                RPCResult{
            "\nResponse is an array with the same size as the input that has the execution result :\n"
            "  [{\"success\": true}, {\"success\": true, \"warnings\": [\"Range not given, using default keypool range\"]}, {\"success\": false, \"error\": {\"code\": -8, \"message\": \"Range should not be specified for an un-ranged descriptor\"}}, ...]\n"
                },
                RPCExamples{
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"internal\": true }, "
                                          "{ \"desc\": \"<my descriptor 2>\", \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"active\": true, \"range\": [0,100], \"label\": \"<my bech32 wallet>\" }]'")
                },
            }.Check(mainRequest);

    RPCTypeCheck(mainRequest.params, {UniValue::VARR});

    const UniValue& requests = mainRequest.params[0];

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t now = 0;
    int64_t nLowestTimestamp = 0;
    bool fRunScan = false;
    UniValue response(UniValue::VARR);
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK2(pwallet->cs_wallet, locked_chain->mutex());
        EnsureWalletIsUnlocked(pwallet);

        const std::optional<int> tip_height = locked_chain->getHeight();
        now = tip_height ? locked_chain->getBlockMedianTimePast(*tip_height) : 0;

        // Verify that all timestamps are present before importing any descriptor.
        for (const UniValue& data : requests.getValues()) {
            int64_t timestamp{0};
            std::string error;
            TryGetImportTimestamp(data, now, timestamp, error);
        }

        const int64_t minimumTimestamp = 1;

        if (tip_height) {
            nLowestTimestamp = locked_chain->getBlockTime(*tip_height);
        }

        for (const UniValue& data : requests.getValues()) {
            int64_t request_timestamp{0};
            std::string timestamp_error;
            const bool valid_timestamp = TryGetImportTimestamp(data, now, request_timestamp, timestamp_error);
            const int64_t timestamp = valid_timestamp ? std::max(request_timestamp, minimumTimestamp) : minimumTimestamp;
            const UniValue result = ProcessDescriptorImport(pwallet, data, timestamp);
            response.push_back(result);

            // If at least one request was successful then allow a rescan.
            if (result["success"].get_bool()) {
                fRunScan = true;
            }

            // Get the lowest timestamp.
            if (timestamp < nLowestTimestamp) {
                nLowestTimestamp = timestamp;
            }
        }
    }

    if (fRunScan && requests.size()) {
        int64_t scannedTime = pwallet->RescanFromTime(nLowestTimestamp, reserver, true /* update */);
        {
            auto locked_chain = pwallet->chain().lock();
            LOCK2(pwallet->cs_wallet, locked_chain->mutex());
            pwallet->ReacceptWalletTransactions(*locked_chain);
        }

        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scannedTime > nLowestTimestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();
            size_t i = 0;
            for (const UniValue& request : requests.getValues()) {
                int64_t request_timestamp{0};
                std::string timestamp_error;
                if (!TryGetImportTimestamp(request, now, request_timestamp, timestamp_error)) {
                    request_timestamp = now;
                }
                // If the descriptor creation date is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scannedTime <= request_timestamp || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV(
                        "error",
                        JSONRPCError(
                            RPC_MISC_ERROR,
                            strprintf("Rescan failed for descriptor with timestamp %d. There was an error reading a "
                                      "block from time %d, which is after or within %d seconds of key creation, and "
                                      "could contain transactions pertaining to the desc. As a result, transactions "
                                      "and coins using this desc may not appear in the wallet. This error could be "
                                      "caused by pruning or data corruption (see defid log for details) and could "
                                      "be dealt with by downloading and rescanning the relevant blocks (see -reindex "
                                      "and -rescan options).",
                                request_timestamp, scannedTime - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)));
                    response.push_back(std::move(result));
                }
                ++i;
            }
        }
    }

    return response;
}

UniValue listdescriptors(const JSONRPCRequest& mainRequest)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(mainRequest);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, mainRequest.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"listdescriptors",
                "\nList the descriptors that are stored in the wallet.\n"
                "The descriptors are sorted by their descriptor string.\n",
                {
                    {"private", RPCArg::Type::BOOL, /* default */ "false", "Whether to include the private descriptor. Requires an unlocked wallet."},
                },
                RPCResult{
            "\n{\n"
            "  \"wallet_name\": \"mywallet\",\n"
            "  \"descriptors\": [\n"
            "    {\n"
            "      \"desc\": \"wpkh([d34db33f/84h/0h/0h]0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)#8fhd9pwu\",\n"
            "      \"timestamp\": 1558737467,\n"
            "      \"active\": true,\n"
            "      \"internal\": false,\n"
            "      \"range\": [0, 99],\n"
            "      \"next\": 0,\n"
            "      \"next_index\": 0\n"
            "    }\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("listdescriptors", "") + HelpExampleRpc("listdescriptors", "")
                },
            }.Check(mainRequest);

    RPCTypeCheck(mainRequest.params, {UniValue::VBOOL});

    const bool priv = mainRequest.params.size() > 0 && !mainRequest.params[0].isNull() && mainRequest.params[0].get_bool();

    if (priv) {
        EnsureWalletIsUnlocked(pwallet);
        if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error: private keys are disabled for this wallet");
        }
    }

    UniValue descriptors(UniValue::VARR);
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& entry : pwallet->GetDescriptors()) {
            const WalletDescriptor& record = entry.second;
            UniValue descriptor(UniValue::VOBJ);
            std::string desc = record.desc;
            if (priv && !record.desc_private.empty()) {
                std::string private_desc;
                if (!pwallet->GetPrivateDescriptorString(record, private_desc)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Cannot decrypt the private descriptor");
                }
                desc = private_desc;
            }
            descriptor.pushKV("desc", desc);
            descriptor.pushKV("timestamp", record.timestamp);
            descriptor.pushKV("active", record.active);
            if (record.active) {
                descriptor.pushKV("internal", record.internal);
            }
            if (record.range_begin >= 0) {
                UniValue range(UniValue::VARR);
                range.push_back(record.range_begin);
                range.push_back(record.range_end);
                descriptor.pushKV("range", range);
                descriptor.pushKV("next", record.next_index);
                descriptor.pushKV("next_index", record.next_index);
            }
            descriptors.push_back(descriptor);
        }
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("wallet_name", pwallet->GetName());
    response.pushKV("descriptors", descriptors);
    return response;
}
