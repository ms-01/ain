// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <external_signer.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <univalue.h>

#include <exception>
#include <string>
#include <utility>
#include <vector>

UniValue enumeratesigners(const JSONRPCRequest& request)
{
            RPCHelpMan{"enumeratesigners",
            "Returns a list of external signers from -signer.\n"
            "Signers with duplicate master key fingerprints are skipped.\n",
            {},
            RPCResult{
            "{\n"
            "  \"signers\" : [          (array of objects) The connected external signers\n"
            "    {\n"
            "      \"fingerprint\" : \"hex\", (string) The master key fingerprint of the signer\n"
            "      \"name\" : \"name\"          (string) The device name, the model returned by the signer\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
            },
            RPCExamples{
                HelpExampleCli("enumeratesigners", "")
            + HelpExampleRpc("enumeratesigners", "")
            }}.Check(request);

    // The build check and the -signer check are performed in a deterministic
    // order, see doc/external-signer.md.
    std::string error;
    const std::string command = GetSignerCommand(error);
    if (command.empty()) {
        throw JSONRPCError(RPC_MISC_ERROR, error);
    }

    UniValue signers_res = UniValue::VARR;
    try {
        std::vector<ExternalSigner> signers;
        ExternalSigner::Enumerate(command, signers, SignerChainName(Params().NetworkIDString()));
        for (const ExternalSigner& signer : signers) {
            UniValue signer_res = UniValue::VOBJ;
            signer_res.pushKV("fingerprint", signer.m_fingerprint);
            signer_res.pushKV("name", signer.m_name);
            signers_res.push_back(std::move(signer_res));
        }
    } catch (const std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("signers", std::move(signers_res));
    return result;
}

void RegisterSignerRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"signer", "enumeratesigners", &enumeratesigners, {}},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
