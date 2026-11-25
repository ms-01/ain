// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <external_signer.h>

#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ENABLE_EXTERNAL_SIGNER
#include <common/run_command.h>
#endif // ENABLE_EXTERNAL_SIGNER

const std::string EXTERNAL_SIGNER_DISABLED_ERROR{"External signer support is not enabled in this build"};
const std::string EXTERNAL_SIGNER_MISSING_COMMAND_ERROR{"Error: restart defid with -signer=<cmd>"};

bool IsExternalSignerCompiled()
{
#ifdef ENABLE_EXTERNAL_SIGNER
    return true;
#else
    return false;
#endif
}

std::string SignerChainName(const std::string& network_id)
{
    if (network_id == "main") return "main";
    if (network_id == "regtest") return "regtest";
    // test, changi and devnet all use the testnet chain of the devices.
    return "test";
}

bool IsValidSignerFingerprint(const std::string& fingerprint)
{
    return fingerprint.size() == 8 && IsHex(fingerprint);
}

std::vector<std::string> SplitSignerCommand(const std::string& command)
{
    std::vector<std::string> result;
    std::string::size_type start = 0;
    while (start < command.size()) {
        const std::string::size_type end = command.find_first_of(" \t", start);
        if (end == std::string::npos) {
            result.emplace_back(command.substr(start));
            break;
        }
        if (end > start) {
            result.emplace_back(command.substr(start, end - start));
        }
        start = end + 1;
    }
    return result;
}

std::string GetSignerCommand(std::string& error)
{
    if (!IsExternalSignerCompiled()) {
        error = EXTERNAL_SIGNER_DISABLED_ERROR;
        return {};
    }
    const std::string command = gArgs.GetArg("-signer", "");
    if (command.empty()) {
        error = EXTERNAL_SIGNER_MISSING_COMMAND_ERROR;
        return {};
    }
    return command;
}

ExternalSigner::ExternalSigner(std::vector<std::string> command, std::string chain, std::string fingerprint, std::string name)
    : m_command{std::move(command)}, m_chain{std::move(chain)}, m_fingerprint{std::move(fingerprint)}, m_name{std::move(name)} {}

std::vector<std::string> ExternalSigner::NetworkArg() const
{
    return {"--chain", m_chain};
}

bool ExternalSigner::ParseSignerList(const UniValue& result, const std::string& command, std::vector<ExternalSigner>& signers, const std::string& chain)
{
    if (!result.isArray()) {
        throw std::runtime_error(strprintf("'%s' received invalid response, expected array of signers", command));
    }
    for (const UniValue& signer : result.getValues()) {
        // Check for error
        const UniValue& error = signer.find_value("error");
        if (!error.isNull()) {
            if (!error.isStr()) {
                throw std::runtime_error(strprintf("'%s' error", command));
            }
            throw std::runtime_error(strprintf("'%s' error: %s", command, error.getValStr()));
        }
        // Check if fingerprint is present
        const UniValue& fingerprint = signer.find_value("fingerprint");
        if (fingerprint.isNull()) {
            throw std::runtime_error(strprintf("'%s' received invalid response, missing signer fingerprint", command));
        }
        const std::string& fingerprintStr{fingerprint.get_str()};
        if (!IsValidSignerFingerprint(fingerprintStr)) {
            throw std::runtime_error(strprintf("'%s' received invalid fingerprint, must be 8 hex characters", command));
        }
        // Skip duplicate signer
        bool duplicate = false;
        for (const ExternalSigner& signer : signers) {
            if (signer.m_fingerprint == fingerprintStr) duplicate = true;
        }
        if (duplicate) continue;
        std::string name;
        const UniValue& model_field = signer.find_value("model");
        if (model_field.isStr() && model_field.getValStr() != "") {
            name += model_field.getValStr();
        }
        signers.emplace_back(SplitSignerCommand(command), chain, fingerprintStr, name);
    }
    return true;
}

bool ExternalSigner::Enumerate(const std::string& command, std::vector<ExternalSigner>& signers, const std::string& chain)
{
#ifndef ENABLE_EXTERNAL_SIGNER
    throw std::runtime_error(EXTERNAL_SIGNER_DISABLED_ERROR);
#else
    // Call <command> enumerate
    std::vector<std::string> cmd_args = SplitSignerCommand(command);
    cmd_args.emplace_back("enumerate");

    const UniValue result = RunCommandParseJSON(cmd_args, "");
    return ParseSignerList(result, command, signers, chain);
#endif // ENABLE_EXTERNAL_SIGNER
}

UniValue ExternalSigner::DisplayAddress(const std::string& descriptor) const
{
    std::vector<std::string> cmd_args = m_command;
    cmd_args.emplace_back("--fingerprint");
    cmd_args.emplace_back(m_fingerprint);
    for (const std::string& arg : NetworkArg()) cmd_args.push_back(arg);
    cmd_args.emplace_back("displayaddress");
    cmd_args.emplace_back("--desc");
    cmd_args.emplace_back(descriptor);

#ifndef ENABLE_EXTERNAL_SIGNER
    throw std::runtime_error(EXTERNAL_SIGNER_DISABLED_ERROR);
#else
    return RunCommandParseJSON(cmd_args, "");
#endif // ENABLE_EXTERNAL_SIGNER
}
