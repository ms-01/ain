// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <external_signer.h>

#include <test/setup_common.h>
#include <tinyformat.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(external_signer_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(fingerprint_validation)
{
    BOOST_CHECK(IsValidSignerFingerprint("00000001"));
    BOOST_CHECK(IsValidSignerFingerprint("deadbeef"));
    BOOST_CHECK(IsValidSignerFingerprint("DEADBEEF"));

    BOOST_CHECK(!IsValidSignerFingerprint(""));
    BOOST_CHECK(!IsValidSignerFingerprint("0000001"));
    BOOST_CHECK(!IsValidSignerFingerprint("000000001"));
    BOOST_CHECK(!IsValidSignerFingerprint("0000000g"));
    BOOST_CHECK(!IsValidSignerFingerprint("zzzzzzzz"));
}

BOOST_AUTO_TEST_CASE(chain_name_mapping)
{
    BOOST_CHECK_EQUAL(SignerChainName("main"), "main");
    BOOST_CHECK_EQUAL(SignerChainName("test"), "test");
    BOOST_CHECK_EQUAL(SignerChainName("changi"), "test");
    BOOST_CHECK_EQUAL(SignerChainName("devnet"), "test");
    BOOST_CHECK_EQUAL(SignerChainName("regtest"), "regtest");
}

BOOST_AUTO_TEST_CASE(command_splitting)
{
    const std::vector<std::string> expected{"python3", "signer.py"};
    BOOST_CHECK(SplitSignerCommand("python3 signer.py") == expected);
    BOOST_CHECK(SplitSignerCommand("python3  signer.py") == expected);
    BOOST_CHECK(SplitSignerCommand("python3\tsigner.py") == expected);
    BOOST_CHECK(SplitSignerCommand("").empty());
}

BOOST_AUTO_TEST_CASE(signer_command_precedence)
{
    std::string error;
    const std::string command = GetSignerCommand(error);
    BOOST_CHECK(command.empty());
    BOOST_CHECK(!error.empty());
    if (IsExternalSignerCompiled()) {
        // The build check comes first, a node without -signer reports the
        // missing command.
        BOOST_CHECK_EQUAL(error, EXTERNAL_SIGNER_MISSING_COMMAND_ERROR);
    } else {
        BOOST_CHECK_EQUAL(error, EXTERNAL_SIGNER_DISABLED_ERROR);
    }
}

BOOST_AUTO_TEST_CASE(parse_signer_list)
{
    const std::string command{"signer.py"};
    const std::string chain{"regtest"};

    // A valid answer with a duplicate fingerprint: the first occurrence wins.
    UniValue result = UniValue::VARR;
    UniValue first = UniValue::VOBJ;
    first.pushKV("fingerprint", "00000001");
    first.pushKV("type", "trezor");
    first.pushKV("model", "trezor_t");
    result.push_back(first);
    UniValue duplicate = UniValue::VOBJ;
    duplicate.pushKV("fingerprint", "00000001");
    duplicate.pushKV("model", "trezor_t");
    result.push_back(duplicate);
    UniValue second = UniValue::VOBJ;
    second.pushKV("fingerprint", "00000002");
    second.pushKV("model", "trezor_one");
    result.push_back(second);

    std::vector<ExternalSigner> signers;
    BOOST_CHECK(ExternalSigner::ParseSignerList(result, command, signers, chain));
    BOOST_CHECK_EQUAL(signers.size(), 2U);
    BOOST_CHECK_EQUAL(signers[0].m_fingerprint, "00000001");
    BOOST_CHECK_EQUAL(signers[0].m_name, "trezor_t");
    BOOST_CHECK_EQUAL(signers[1].m_fingerprint, "00000002");
    BOOST_CHECK_EQUAL(signers[1].m_name, "trezor_one");

    // An answer that is not an array.
    UniValue not_an_array = UniValue::VOBJ;
    BOOST_CHECK_EXCEPTION(ExternalSigner::ParseSignerList(not_an_array, command, signers, chain),
                          std::runtime_error,
                          [&command](const std::runtime_error& e) {
                              return std::string(e.what()) == strprintf("'%s' received invalid response, expected array of signers", command);
                          });

    // An error object of the signer.
    UniValue error_answer = UniValue::VARR;
    UniValue error_entry = UniValue::VOBJ;
    error_entry.pushKV("error", "fingerprint not found");
    error_answer.push_back(error_entry);
    BOOST_CHECK_EXCEPTION(ExternalSigner::ParseSignerList(error_answer, command, signers, chain),
                          std::runtime_error,
                          [&command](const std::runtime_error& e) {
                              return std::string(e.what()) == strprintf("'%s' error: fingerprint not found", command);
                          });

    // A signer without a fingerprint.
    UniValue no_fingerprint = UniValue::VARR;
    UniValue no_fingerprint_entry = UniValue::VOBJ;
    no_fingerprint_entry.pushKV("model", "trezor_t");
    no_fingerprint.push_back(no_fingerprint_entry);
    BOOST_CHECK_EXCEPTION(ExternalSigner::ParseSignerList(no_fingerprint, command, signers, chain),
                          std::runtime_error,
                          [&command](const std::runtime_error& e) {
                              return std::string(e.what()) == strprintf("'%s' received invalid response, missing signer fingerprint", command);
                          });

    // A signer with an invalid fingerprint.
    UniValue invalid_fingerprint = UniValue::VARR;
    UniValue invalid_fingerprint_entry = UniValue::VOBJ;
    invalid_fingerprint_entry.pushKV("fingerprint", "0000001");
    invalid_fingerprint.push_back(invalid_fingerprint_entry);
    BOOST_CHECK_EXCEPTION(ExternalSigner::ParseSignerList(invalid_fingerprint, command, signers, chain),
                          std::runtime_error,
                          [&command](const std::runtime_error& e) {
                              return std::string(e.what()) == strprintf("'%s' received invalid fingerprint, must be 8 hex characters", command);
                          });
}

BOOST_AUTO_TEST_SUITE_END()
