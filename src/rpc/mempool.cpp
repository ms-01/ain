// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <index/txospenderindex.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <threadsafety.h>
#include <txmempool.h>
#include <uint256.h>
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

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "blockchain",         "gettxspendingprevout",   &gettxspendingprevout,   {"outputs", "options"} },
};
// clang-format on

void RegisterMempoolRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}
