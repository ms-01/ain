// Copyright (c) 2024-present The DeFiChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the adaptive block download stalling timeout

#include <net_processing_stalling.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>

BOOST_FIXTURE_TEST_SUITE(net_processing_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(stalling_timeout_constants)
{
    BOOST_CHECK_EQUAL(BLOCK_STALLING_TIMEOUT_DEFAULT.count(), 2);
    BOOST_CHECK_EQUAL(BLOCK_STALLING_TIMEOUT_MAX.count(), 64);
    BOOST_CHECK_EQUAL(MANUAL_PEER_BLOCK_DOWNLOAD_COOLDOWN.count(), 2);
    BOOST_CHECK_EQUAL(std::chrono::duration_cast<std::chrono::seconds>(MANUAL_PEER_BLOCK_DOWNLOAD_COOLDOWN).count(), 120);
}

BOOST_AUTO_TEST_CASE(stalling_timeout_is_doubled_on_stall)
{
    auto timeout = BLOCK_STALLING_TIMEOUT_DEFAULT;

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 4);

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 8);

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 16);

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 32);

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 64);
}

BOOST_AUTO_TEST_CASE(stalling_timeout_is_capped_at_maximum)
{
    auto timeout = BLOCK_STALLING_TIMEOUT_MAX;

    timeout = NextStallingTimeoutAfterStall(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), BLOCK_STALLING_TIMEOUT_MAX.count());

    // The maximum is never exceeded, no matter how often a peer stalls.
    for (int i = 0; i < 10; ++i) {
        timeout = NextStallingTimeoutAfterStall(timeout);
        BOOST_CHECK_EQUAL(timeout.count(), BLOCK_STALLING_TIMEOUT_MAX.count());
    }
}

BOOST_AUTO_TEST_CASE(stalling_timeout_decays_after_connected_block)
{
    auto timeout = std::chrono::seconds{64};

    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 54);

    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 45);

    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 38);
}

BOOST_AUTO_TEST_CASE(stalling_timeout_recovers_to_default)
{
    auto timeout = std::chrono::seconds{4};

    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 3);

    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), 2);

    // The default value is never undercut.
    timeout = NextStallingTimeoutAfterBlock(timeout);
    BOOST_CHECK_EQUAL(timeout.count(), BLOCK_STALLING_TIMEOUT_DEFAULT.count());

    for (int i = 0; i < 10; ++i) {
        timeout = NextStallingTimeoutAfterBlock(timeout);
        BOOST_CHECK_EQUAL(timeout.count(), BLOCK_STALLING_TIMEOUT_DEFAULT.count());
    }
}

BOOST_AUTO_TEST_SUITE_END()
