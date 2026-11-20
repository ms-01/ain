// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <node/mempool_persist.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(mempool_persist_tests, BasicTestingSetup)

namespace {

const int64_t maxInt64 = std::numeric_limits<int64_t>::max();
const int64_t minInt64 = std::numeric_limits<int64_t>::min();

/** Check whether the addition of the two values overflows a signed 64 bit integer. */
bool AdditionOverflows(int64_t a, int64_t b)
{
    if (b > 0 && a > maxInt64 - b) {
        return true;
    }
    if (b < 0 && a < minInt64 - b) {
        return true;
    }
    return false;
}

/**
 * The expiry check of the original implementation. It is only defined for the
 * inputs that did not overflow, for all other inputs the original
 * implementation had undefined behaviour.
 */
bool LegacyIsExpired(int64_t nTime, int64_t nNow, int64_t nExpiry)
{
    if (AdditionOverflows(nTime, nExpiry)) {
        return false;
    }
    return nTime + nExpiry <= nNow;
}

/** The expiry check of the original implementation, including the EVM/DVM structure. */
bool LegacyIsEntryExpired(int64_t nTime, int64_t nNow, int64_t nExpiryTimeout, int64_t nExpiryEVMTimeout, bool isEVMTx)
{
    if (AdditionOverflows(nTime, nExpiryTimeout) || AdditionOverflows(nTime, nExpiryEVMTimeout)) {
        return false;
    }
    const bool dvmOpen = (nTime + nExpiryTimeout) > nNow;
    const bool evmOpen = (nTime + nExpiryEVMTimeout) > nNow;
    return !((isEVMTx && evmOpen) || dvmOpen);
}

} // namespace

BOOST_AUTO_TEST_CASE(mempool_expiry_seconds)
{
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(0), 0);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(1), 3600);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(336), 336 * 3600);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(3), 3 * 3600);

    // Negative values are treated as "no expiry" instead of overflowing.
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(-1), 0);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(minInt64), 0);

    // Huge values saturate instead of overflowing.
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(maxInt64), maxInt64);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(maxInt64 / 3600 + 1), maxInt64);
    BOOST_CHECK_EQUAL(MempoolExpirySeconds(maxInt64 / 3600), (maxInt64 / 3600) * 3600);
}

BOOST_AUTO_TEST_CASE(mempool_size_bytes)
{
    BOOST_CHECK_EQUAL(MempoolSizeBytes(0), 0);
    BOOST_CHECK_EQUAL(MempoolSizeBytes(1), 1000000);
    BOOST_CHECK_EQUAL(MempoolSizeBytes(300), 300000000);

    BOOST_CHECK_EQUAL(MempoolSizeBytes(-1), 0);
    BOOST_CHECK_EQUAL(MempoolSizeBytes(maxInt64), maxInt64);
    BOOST_CHECK_EQUAL(MempoolSizeBytes(maxInt64 / 1000000 + 1), maxInt64);
}

BOOST_AUTO_TEST_CASE(mempool_file_entries)
{
    BOOST_CHECK_EQUAL(MaxMempoolFileEntries(0), 0U);
    BOOST_CHECK_EQUAL(MaxMempoolFileEntries(-1), 0U);
    BOOST_CHECK_EQUAL(MaxMempoolFileEntries(300000000), 300000000U / MIN_MEMPOOL_ENTRY_SIZE + MEMPOOL_FILE_ENTRY_MARGIN);
    // The bound must not overflow for extreme mempool sizes.
    BOOST_CHECK(MaxMempoolFileEntries(maxInt64) > 0);
}

BOOST_AUTO_TEST_CASE(mempool_time_expired)
{
    const int64_t nNow = 1600000000;
    const int64_t nExpiry = 3600;

    // An entry that is exactly on the deadline is expired.
    BOOST_CHECK(IsMempoolTimeExpired(nNow - nExpiry, nNow, nExpiry));
    BOOST_CHECK(!IsMempoolTimeExpired(nNow - nExpiry + 1, nNow, nExpiry));
    BOOST_CHECK(IsMempoolTimeExpired(nNow - nExpiry - 1, nNow, nExpiry));

    // An entry in the future is not expired.
    BOOST_CHECK(!IsMempoolTimeExpired(nNow, nNow, nExpiry));
    BOOST_CHECK(!IsMempoolTimeExpired(nNow + 1, nNow, nExpiry));

    // A zero expiry expires everything that is not in the future.
    BOOST_CHECK(IsMempoolTimeExpired(nNow, nNow, 0));
    BOOST_CHECK(!IsMempoolTimeExpired(nNow + 1, nNow, 0));

    // A negative expiry is treated as zero.
    BOOST_CHECK(IsMempoolTimeExpired(nNow, nNow, -100));

    // Extreme timestamps must not overflow and must not be expired.
    BOOST_CHECK(!IsMempoolTimeExpired(maxInt64, nNow, nExpiry));
    BOOST_CHECK(IsMempoolTimeExpired(minInt64, nNow, nExpiry));

    // A huge expiry never expires an entry.
    BOOST_CHECK(!IsMempoolTimeExpired(0, nNow, maxInt64));
    BOOST_CHECK(!IsMempoolTimeExpired(nNow, nNow, maxInt64));
}

BOOST_AUTO_TEST_CASE(mempool_entry_expired_keeps_evm_dvm_semantics)
{
    const int64_t nNow = 1600000000;
    const int64_t dvmExpiry = 3600;
    const int64_t evmExpiry = 60;

    // A DVM transaction only uses the DVM expiry window.
    BOOST_CHECK(IsMempoolEntryExpired(nNow - dvmExpiry, nNow, dvmExpiry, evmExpiry, false));
    BOOST_CHECK(!IsMempoolEntryExpired(nNow - dvmExpiry + 1, nNow, dvmExpiry, evmExpiry, false));

    // An EVM transaction is re-imported as long as either window is open.
    BOOST_CHECK(!IsMempoolEntryExpired(nNow - evmExpiry - 1, nNow, dvmExpiry, evmExpiry, true));
    BOOST_CHECK(!IsMempoolEntryExpired(nNow - dvmExpiry + 1, nNow, dvmExpiry, evmExpiry, true));
    BOOST_CHECK(IsMempoolEntryExpired(nNow - dvmExpiry - 1, nNow, dvmExpiry, evmExpiry, true));
}

BOOST_AUTO_TEST_CASE(mempool_expiry_is_equivalent_to_the_original_implementation)
{
    const int64_t nNow = 1600000000;
    const std::vector<int64_t> times{
        0,
        1,
        nNow - 7200,
        nNow - 3601,
        nNow - 3600,
        nNow - 3599,
        nNow - 1,
        nNow,
        nNow + 1,
        nNow + 3600,
        maxInt64,
        minInt64,
    };
    const std::vector<int64_t> expiries{0, 1, 60, 3600, 86400, maxInt64};

    for (const int64_t nTime : times) {
        for (const int64_t nExpiry : expiries) {
            if (AdditionOverflows(nTime, nExpiry)) {
                // The original implementation had undefined behaviour here, the
                // hardened implementation must not expire the entry.
                BOOST_CHECK(!IsMempoolTimeExpired(nTime, nNow, nExpiry));
                continue;
            }
            BOOST_CHECK_EQUAL(IsMempoolTimeExpired(nTime, nNow, nExpiry),
                              LegacyIsExpired(nTime, nNow, nExpiry));

            for (const bool isEVMTx : {false, true}) {
                BOOST_CHECK_EQUAL(IsMempoolEntryExpired(nTime, nNow, nExpiry, 60, isEVMTx),
                                  LegacyIsEntryExpired(nTime, nNow, nExpiry, 60, isEVMTx));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(mempool_plausible_time)
{
    const int64_t nNow = 1600000000;

    BOOST_CHECK(IsPlausibleMempoolTime(0, nNow));
    BOOST_CHECK(IsPlausibleMempoolTime(nNow - 86400, nNow));
    BOOST_CHECK(IsPlausibleMempoolTime(nNow, nNow));

    // Timestamps before the unix epoch are implausible.
    BOOST_CHECK(!IsPlausibleMempoolTime(-1, nNow));
    BOOST_CHECK(!IsPlausibleMempoolTime(minInt64, nNow));

    // Timestamps that are absurdly far in the future are implausible.
    BOOST_CHECK(IsPlausibleMempoolTime(nNow + MAX_MEMPOOL_TIME_DRIFT, nNow));
    BOOST_CHECK(!IsPlausibleMempoolTime(nNow + MAX_MEMPOOL_TIME_DRIFT + 1, nNow));
    BOOST_CHECK(!IsPlausibleMempoolTime(maxInt64, nNow));
}

BOOST_AUTO_TEST_CASE(mempool_fee_delta)
{
    BOOST_CHECK(IsValidMempoolFeeDelta(0));
    BOOST_CHECK(IsValidMempoolFeeDelta(1));
    BOOST_CHECK(IsValidMempoolFeeDelta(MAX_MONEY));
    BOOST_CHECK(!IsValidMempoolFeeDelta(MAX_MONEY + 1));
    BOOST_CHECK(!IsValidMempoolFeeDelta(-1));
    BOOST_CHECK(!IsValidMempoolFeeDelta(minInt64));
}

BOOST_AUTO_TEST_SUITE_END()
