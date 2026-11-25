// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_COMMON_RUN_COMMAND_H
#define DEFI_COMMON_RUN_COMMAND_H

#include <string>
#include <vector>

class UniValue;

/**
 * Execute a command which returns JSON, and parse the result.
 *
 * @param cmd_args The command and arguments
 * @param str_std_in string to pass to stdin
 * @return parsed JSON
 * @throws std::runtime_error if the command could not be run, exited with a
 *         non-zero status or did not answer with valid JSON.
 */
UniValue RunCommandParseJSON(const std::vector<std::string>& cmd_args, const std::string& str_std_in = "");

#endif // DEFI_COMMON_RUN_COMMAND_H
