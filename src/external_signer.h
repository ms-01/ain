// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_EXTERNAL_SIGNER_H
#define DEFI_EXTERNAL_SIGNER_H

#include <univalue.h>

#include <string>
#include <vector>

/**
 * Error message of the RPCs when the build has no external signer support.
 *
 * The RPC methods are registered in every build (a stable RPC surface), a
 * build without external signer support answers with this error.
 */
extern const std::string EXTERNAL_SIGNER_DISABLED_ERROR;

/**
 * Error message of the RPCs when the node was not started with -signer=<cmd>.
 */
extern const std::string EXTERNAL_SIGNER_MISSING_COMMAND_ERROR;

/**
 * Whether this build has external signer support compiled in.
 */
bool IsExternalSignerCompiled();

/**
 * Maps a DeFiChain network id to the chain name that is handed to the device.
 *
 * main -> main, test/changi/devnet -> test, regtest -> regtest.
 */
std::string SignerChainName(const std::string& network_id);

/**
 * Whether the given master key fingerprint is valid: exactly 8 hex characters.
 */
bool IsValidSignerFingerprint(const std::string& fingerprint);

/**
 * Splits a signer command into its arguments.
 *
 * The command is split on whitespace, quoting and escaping are not supported.
 */
std::vector<std::string> SplitSignerCommand(const std::string& command);

/**
 * Returns the external signer command of the node.
 *
 * The preconditions are checked in a deterministic order:
 *  1. the build must have external signer support enabled,
 *  2. the node must have been started with -signer=<cmd>.
 *
 * @param[out] error is set to the error message if a precondition is not met
 * @returns the signer command or an empty string if a precondition is not met
 */
std::string GetSignerCommand(std::string& error);

/**
 * Enables interaction with an external signing device or service, such as
 * a hardware wallet. See doc/external-signer.md
 */
class ExternalSigner
{
private:
    //! The command which handles interaction with the external signer.
    std::vector<std::string> m_command;

    //! The chain name that is handed to the device ("main", "test", "regtest").
    std::string m_chain;

    std::vector<std::string> NetworkArg() const;

public:
    //! @param[in] command      the command which handles interaction with the external signer
    //! @param[in] chain        "main", "test" or "regtest"
    //! @param[in] fingerprint  master key fingerprint of the signer
    //! @param[in] name         device name
    ExternalSigner(std::vector<std::string> command, std::string chain, std::string fingerprint, std::string name);

    //! Master key fingerprint of the signer
    std::string m_fingerprint;

    //! Name of signer
    std::string m_name;

    //! Obtain a list of signers. Calls `<command> enumerate`.
    //! @param[in]      command the command which handles interaction with the external signer
    //! @param[in,out]  signers vector to which new signers (with a unique master key fingerprint) are added
    //! @param[in]      chain   "main", "test" or "regtest"
    //! @returns success
    static bool Enumerate(const std::string& command, std::vector<ExternalSigner>& signers, const std::string& chain);

    /**
     * Parses the answer of a signer command into the list of signers.
     *
     * Signers with a duplicate master key fingerprint are skipped, an invalid
     * answer is rejected with an exception.
     *
     * @param[in]      result  the parsed answer of the signer command
     * @param[in]      command the command which handles interaction with the external signer
     * @param[in,out]  signers vector to which new signers (with a unique master key fingerprint) are added
     * @param[in]      chain   "main", "test" or "regtest"
     * @returns success
     */
    static bool ParseSignerList(const UniValue& result, const std::string& command, std::vector<ExternalSigner>& signers, const std::string& chain);

    //! Display address on the device. Calls `<command> --fingerprint <fingerprint> --chain <chain>
    //! displayaddress --desc <descriptor>`.
    //! @param[in] descriptor Descriptor specifying which address to display.
    //!            Must include a public key or xpub, as well as key origin.
    UniValue DisplayAddress(const std::string& descriptor) const;
};

#endif // DEFI_EXTERNAL_SIGNER_H
