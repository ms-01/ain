// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <test/setup_common.h>
#include <timedata.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

static void AddMulti(TimeOffsets &offsets, const std::vector<std::chrono::seconds> &to_add)
{
    for (auto offset : to_add) {
        offsets.Add(offset);
    }
}

BOOST_FIXTURE_TEST_SUITE(timeoffsets_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(timeoffsets)
{
    TimeOffsets offsets;
    BOOST_CHECK(offsets.Median() == 0s);

    AddMulti(offsets, {{0s, -1s, -2s, -3s}});
    // median should be zero for < 5 offsets
    BOOST_CHECK(offsets.Median() == 0s);

    offsets.Add(-4s);
    // we now have 5 offsets: [-4, -3, -2, -1, 0]
    BOOST_CHECK(offsets.Median() == -2s);

    AddMulti(offsets, {4, 5s});
    // we now have 9 offsets: [-4, -3, -2, -1, 0, 5, 5, 5, 5]
    BOOST_CHECK(offsets.Median() == 0s);

    AddMulti(offsets, {41, 10s});
    // the TimeOffsets is now at capacity with 50 offsets, oldest offsets is discarded for any additional offset
    BOOST_CHECK(offsets.Median() == 10s);

    AddMulti(offsets, {25, 15s});
    // we now have 25 offsets of 10s followed by 25 offsets of 15s
    BOOST_CHECK(offsets.Median() == 15s);
}

BOOST_AUTO_TEST_CASE(timeoffsets_clear)
{
    TimeOffsets offsets;
    AddMulti(offsets, {{1s, 2s, 3s, 4s, 5s}});
    BOOST_CHECK(offsets.Median() == 3s);

    offsets.Clear();
    BOOST_CHECK(offsets.Median() == 0s);
}

BOOST_AUTO_TEST_CASE(timeoffsets_warning)
{
    // The warning threshold is 10 minutes, the median has to exceed it to raise a warning.
    TimeOffsets offsets;
    AddMulti(offsets, {{-60min, -40min, -30min, 0min, 10min}});
    BOOST_CHECK(offsets.WarnIfOutOfSync());

    TimeOffsets offsetsSmall;
    AddMulti(offsetsSmall, {5, 11min});
    BOOST_CHECK(offsetsSmall.WarnIfOutOfSync());

    TimeOffsets offsetsOk;
    AddMulti(offsetsOk, {4, 60min});
    BOOST_CHECK(!offsetsOk.WarnIfOutOfSync());

    TimeOffsets offsetsOk2;
    AddMulti(offsetsOk2, {100, 3min});
    BOOST_CHECK(!offsetsOk2.WarnIfOutOfSync());
}

BOOST_AUTO_TEST_CASE(timeoffsets_global_instance)
{
    // The global time offset tracker feeds GetTimeOffset() (and with it GetAdjustedTime()).
    auto& offsets = GetTimeOffsets();
    offsets.Clear();
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);

    // Less than 5 samples: the median is not computed and the offset stays 0.
    AddTimeOffsetSample(120);
    AddTimeOffsetSample(120);
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);

    // 5 samples with an offset of 120 seconds: the median is used now.
    AddTimeOffsetSample(120);
    AddTimeOffsetSample(120);
    AddTimeOffsetSample(120);
    BOOST_CHECK_EQUAL(GetTimeOffset(), 120);
    const int64_t adjustedDiff{GetAdjustedTime() - GetTime()};
    BOOST_CHECK(adjustedDiff >= 119 && adjustedDiff <= 121);

    // The applied offset is bounded by -maxtimeadjustment.
    offsets.Clear();
    AddTimeOffsetSample(DEFAULT_MAX_TIME_ADJUSTMENT + 60);
    AddTimeOffsetSample(DEFAULT_MAX_TIME_ADJUSTMENT + 60);
    AddTimeOffsetSample(DEFAULT_MAX_TIME_ADJUSTMENT + 60);
    AddTimeOffsetSample(DEFAULT_MAX_TIME_ADJUSTMENT + 60);
    AddTimeOffsetSample(DEFAULT_MAX_TIME_ADJUSTMENT + 60);
    BOOST_CHECK_EQUAL(GetTimeOffset(), DEFAULT_MAX_TIME_ADJUSTMENT);

    offsets.Clear();
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
