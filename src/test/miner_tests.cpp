// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <miner.h>
#include <primitives/block.h>
#include <timedata.h>
#include <util/time.h>
#include <validation.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(miner_tests, BasicTestingSetup)

namespace {
//! The time offset tracker is global: make sure that the samples of a test case do not leak into
//! the other test cases.
struct TimeOffsetsGuard {
    ~TimeOffsetsGuard() { GetTimeOffsets().Clear(); }
};
} // namespace

/**
 * The mining path uses the unadjusted clock.
 *
 * The validation limits (30 seconds / 5 minutes) are unadjusted as well, so a peer derived time
 * offset could otherwise make the node mine a block that its own validation rejects.
 */
BOOST_AUTO_TEST_CASE(update_time_ignores_peer_time_offset)
{
    const int64_t nOffset{60 * 60}; // one hour
    const int64_t nBefore{GetTime()};

    TimeOffsetsGuard guard;

    // Simulate outbound peers whose clocks are ahead of the local clock.
    GetTimeOffsets().Clear();
    for (int i = 0; i < 5; ++i) {
        AddTimeOffsetSample(nOffset);
    }

    // The peer derived offset is applied to the adjusted clock ...
    BOOST_CHECK_EQUAL(GetTimeOffset(), nOffset);
    BOOST_CHECK(GetAdjustedTime() >= nBefore + nOffset);

    CBlockIndex prev;
    prev.nHeight = 0;
    prev.nTime = nBefore - 1000;

    CBlockHeader header;
    header.nTime = 0;

    UpdateTime(&header, Params().GetConsensus(), &prev);
    const int64_t nAfter{GetTime()};

    // ... but the miner uses the unadjusted clock, so the block timestamp stays inside the
    // unadjusted validation limit and the node cannot mine a block it would reject itself.
    BOOST_CHECK(header.nTime >= prev.GetMedianTimePast() + 1);
    BOOST_CHECK(header.nTime <= nAfter);
    BOOST_CHECK(header.nTime <= nAfter + MAX_FUTURE_BLOCK_TIME_EUNOSPAYA);

    // The adjusted clock on the other hand would have produced a timestamp that is beyond the
    // unadjusted limit.
    BOOST_CHECK(GetAdjustedTime() > nAfter + MAX_FUTURE_BLOCK_TIME_EUNOSPAYA);
}

/**
 * Without any peer samples the applied offset is zero and the miner uses the local clock.
 */
BOOST_AUTO_TEST_CASE(update_time_without_peer_time_offset)
{
    TimeOffsetsGuard guard;
    GetTimeOffsets().Clear();

    CBlockIndex prev;
    prev.nHeight = 0;
    prev.nTime = GetTime() - 1000;

    CBlockHeader header;
    header.nTime = 0;

    UpdateTime(&header, Params().GetConsensus(), &prev);
    const int64_t nAfter{GetTime()};

    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);
    BOOST_CHECK(header.nTime <= nAfter);
    BOOST_CHECK(header.nTime <= nAfter + MAX_FUTURE_BLOCK_TIME_EUNOSPAYA);
}

BOOST_AUTO_TEST_SUITE_END()
