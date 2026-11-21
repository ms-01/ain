// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <core_io.h>
#include <index/txospenderindex.h>
#include <mempool_accept.h>
#include <node/transaction.h>
#include <policy/packages.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <threadsafety.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <validation.h>

#include <univalue.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

static UniValue gettxspendingprevout(const JSONRPCRequest& request)
{
    RPCHelpMan{"gettxspendingprevout",
        "\nScans the mempool (and the txospenderindex, if available) to find transactions spending any of the given outputs.\n"
        "\nNote: unlike in Bitcoin Core, the options are passed as a single object argument\n"
        "(e.g. gettxspendingprevout '[{\"txid\":\"...\",\"vout\":0}]' '{\"return_spending_tx\":true}')\n"
        "instead of as named arguments.\n",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The transaction outputs that we want to check, and within each, the txid (string) vout (numeric).",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                        },
                    },
                },
            },
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Options for the lookup.",
                {
                    {"mempool_only", RPCArg::Type::BOOL, /* default */ "true if the txospenderindex is unavailable, otherwise false", "If false and mempool lacks a relevant spend, use the txospenderindex (an error is returned if it is not available)."},
                    {"return_spending_tx", RPCArg::Type::BOOL, /* default */ "false", "If true, return the full spending transaction."},
                },
            },
        },
        RPCResult{
            "[\n"
            "  {\n"
            "    \"txid\": \"...\",        (string) The transaction id of the checked output\n"
            "    \"vout\": n,              (numeric) The output number of the checked output\n"
            "    \"spendingtxid\": \"...\", (string, optional) The transaction id of the transaction spending this output (omitted if unspent)\n"
            "    \"spendingtx\": \"...\",   (string, optional) The transaction spending this output (only if return_spending_tx is set, omitted if unspent)\n"
            "    \"blockhash\": \"...\"     (string, optional) The hash of the block the spending transaction was confirmed in (omitted if unspent or the spending transaction is not confirmed)\n"
            "  }\n"
            "]\n"
        },
        RPCExamples{
            HelpExampleCli("gettxspendingprevout", "\"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":3}]\"")
            + HelpExampleRpc("gettxspendingprevout", "\"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":3}]\"")
        },
    }.Check(request);

    const UniValue& output_params = request.params[0].get_array();
    if (output_params.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, outputs are missing");
    }

    UniValue options{UniValue::VOBJ};
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        options = request.params[1];
    }
    RPCTypeCheckObj(options,
                    {
                        {"mempool_only", UniValueType(UniValue::VBOOL)},
                        {"return_spending_tx", UniValueType(UniValue::VBOOL)},
                    },
                    /*fAllowNull=*/true, /*fStrict=*/true);

    const bool mempool_only{options.exists("mempool_only") ? options["mempool_only"].get_bool() : !g_txospenderindex};
    const bool return_spending_tx{options.exists("return_spending_tx") ? options["return_spending_tx"].get_bool() : false};

    // Worklist of outpoints to resolve
    struct Entry {
        COutPoint outpoint;
        size_t request_index;
    };
    std::vector<Entry> prevouts_to_process;
    prevouts_to_process.reserve(output_params.size());
    for (size_t idx = 0; idx < output_params.size(); ++idx) {
        const UniValue& o = output_params[idx].get_obj();

        RPCTypeCheckObj(o,
                        {
                            {"txid", UniValueType(UniValue::VSTR)},
                            {"vout", UniValueType(UniValue::VNUM)},
                        },
                        /*fAllowNull=*/false, /*fStrict=*/true);

        const uint256 txid = ParseHashO(o, "txid");
        const int nOutput{o.find_value("vout").getInt<int>()};
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");
        }
        prevouts_to_process.push_back(Entry{COutPoint(txid, static_cast<uint32_t>(nOutput)), idx});
    }

    auto make_output = [&output_params, return_spending_tx](const Entry& prevout, const CTransaction* spending_tx = nullptr) {
        UniValue o{output_params[prevout.request_index]};
        if (spending_tx) {
            o.pushKV("spendingtxid", spending_tx->GetHash().GetHex());
            if (return_spending_tx) {
                o.pushKV("spendingtx", EncodeHexTx(*spending_tx));
            }
        }
        return o;
    };

    std::vector<UniValue> results(output_params.size());

    // Search the mempool first
    std::vector<Entry> unresolved;
    unresolved.reserve(prevouts_to_process.size());
    {
        LOCK(::mempool.cs);

        // Make the result if the spending tx appears in the mempool or this is a mempool_only request
        for (const auto& prevout : prevouts_to_process) {
            const CTransaction* spending_tx{::mempool.GetConflictTx(prevout.outpoint)};

            // If the outpoint is not spent in the mempool and this is not a mempool-only
            // request, we cannot answer it yet.
            if (!spending_tx && !mempool_only) {
                unresolved.push_back(prevout);
            } else {
                results[prevout.request_index] = make_output(prevout, spending_tx);
            }
        }
    }

    // mempool_only requests resolve every outpoint above, so only other requests reach the index.
    if (!unresolved.empty() && (!g_txospenderindex || !g_txospenderindex->BlockUntilSyncedToCurrentChain())) {
        throw JSONRPCError(RPC_MISC_ERROR, "Mempool lacks a relevant spend, and txospenderindex is unavailable.");
    }

    for (const auto& prevout : unresolved) {
        std::string error_out;
        const std::optional<TxoSpender> spender{g_txospenderindex->FindSpender(prevout.outpoint, &error_out)};
        if (!error_out.empty()) {
            throw JSONRPCError(RPC_MISC_ERROR, error_out);
        }

        if (spender) {
            UniValue o{make_output(prevout, spender->tx.get())};
            o.pushKV("blockhash", spender->block_hash.GetHex());
            results[prevout.request_index] = std::move(o);
        } else {
            // Only return the input outpoint itself, which indicates it is unspent.
            results[prevout.request_index] = make_output(prevout);
        }
    }

    UniValue result{UniValue::VARR};
    result.reserve(results.size());
    for (auto& output : results) result.push_back(std::move(output));
    return result;
}

static UniValue submitpackage(const JSONRPCRequest& request)
{
    RPCHelpMan{"submitpackage",
        "Submit a package of raw transactions (serialized, hex-encoded) to the local node (-regtest only).\n"
        "The package will be validated according to consensus and mempool policy rules. If all transactions pass, they will be accepted to the mempool.\n"
        "This RPC is experimental and the interface may be unstable.\n"
        "Warning: until package relay is in use, successful submission does not mean the transactions will propagate to other nodes on the network.\n",
        {
            {"package", RPCArg::Type::ARR, RPCArg::Optional::NO, "An array of raw transactions.",
                {
                    {"rawtx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
        },
        RPCResult{
            "{\n"
            "  \"tx-results\": [          (array) the result of every transaction of the package, in the order of the given transactions\n"
            "    {\n"
            "      \"txid\"           (string) The transaction hash in hex\n"
            "      \"allowed\"        (boolean) If the mempool allows this tx to be inserted\n"
            "      \"reject-reason\"  (string) Rejection string (only present when 'allowed' is false)\n"
            "      \"vsize\"          (numeric) Virtual transaction size as defined in BIP 141\n"
            "      \"fees\": {        (object) Transaction fees\n"
            "        \"base\"        (string) transaction fee in " + CURRENCY_UNIT + "\n"
            "      }\n"
            "    }\n"
            "  ]\n"
            "}\n"
        },
        RPCExamples{
            HelpExampleCli("submitpackage", "[rawtx1, rawtx2]")
        },
    }.Check(request);

    if (!Params().MineBlocksOnDemand()) {
        throw std::runtime_error("submitpackage is for regression testing (-regtest mode) only");
    }

    RPCTypeCheck(request.params, {UniValue::VARR});

    const UniValue& raw_transactions = request.params[0].get_array();
    if (raw_transactions.empty() || raw_transactions.size() > MAX_PACKAGE_COUNT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("Array must contain between 1 and %d transactions.", MAX_PACKAGE_COUNT));
    }

    Package package;
    package.reserve(raw_transactions.size());
    for (const auto& rawtx : raw_transactions.getValues()) {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, rawtx.get_str())) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                               "TX decode failed: " + rawtx.get_str() + " Make sure the tx has at least one input.");
        }
        package.emplace_back(MakeTransactionRef(std::move(mtx)));
    }

    PackageMempoolAcceptResult package_result =
        ProcessNewPackage(mempool, package, {} /* max_tx_fees */, /* test_accept */ false);

    if (!package_result.m_package_error.empty()) {
        throw JSONRPCTransactionError(TransactionError::INVALID_PACKAGE, package_result.m_package_error);
    }
    if (package_result.m_result_type == PackageValidationResult::PCKG_TX) {
        for (const auto& tx_result : package_result.m_tx_results) {
            if (!tx_result.allowed) {
                throw JSONRPCTransactionError(TransactionError::MEMPOOL_REJECTED,
                                              strprintf("%s failed: %s", tx_result.txid.GetHex(), tx_result.reject_reason));
            }
        }
        throw JSONRPCTransactionError(TransactionError::MEMPOOL_ERROR, "package rejected");
    }

    // Broadcast the transactions of the package. The transactions are broadcasted one after
    // another, the package itself is not relayed as a unit.
    for (const auto& tx : package) {
        std::string err_string;
        const auto err = BroadcastTransaction(tx, err_string, /*max_tx_fee=*/0, /*relay=*/true, /*wait_callback=*/true);
        if (err != TransactionError::OK) {
            throw JSONRPCTransactionError(err,
                                          strprintf("transaction broadcast failed: %s (all transactions were submitted to the mempool)",
                                                    err_string));
        }
    }

    UniValue result{UniValue::VOBJ};
    UniValue tx_results{UniValue::VARR};
    for (const auto& tx : package) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("txid", tx->GetHash().GetHex());
        entry.pushKV("allowed", true);
        entry.pushKV("vsize", GetVirtualTransactionSize(*tx));
        {
            LOCK(mempool.cs);
            const auto it = mempool.GetIter(tx->GetHash());
            if (it) {
                UniValue fees{UniValue::VOBJ};
                fees.pushKV("base", ValueFromAmount((*it)->GetFee()));
                entry.pushKV("fees", fees);
            }
        }
        tx_results.push_back(std::move(entry));
    }
    result.pushKV("tx-results", tx_results);
    return result;
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "blockchain",         "gettxspendingprevout",   &gettxspendingprevout,   {"outputs", "options"} },
    { "mempool",            "submitpackage",           &submitpackage,           {"package"} },
};
// clang-format on

void RegisterMempoolRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}
