// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <test/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <string>
#include <unordered_map>

BOOST_AUTO_TEST_SUITE(index_prunelock_tests)

namespace {
// Build a prune lock map with a single lock for the given index.
std::unordered_map<std::string, PruneLockInfo> MakeLocks(const std::string& name, int height_first)
{
    std::unordered_map<std::string, PruneLockInfo> locks;
    PruneLockInfo info;
    info.height_first = height_first;
    locks[name] = info;
    return locks;
}
} // namespace

BOOST_AUTO_TEST_CASE(prune_lock_no_lock)
{
    // Without any prune lock the tip is the highest prunable height, i.e. only
    // the MIN_BLOCKS_TO_KEEP window limits the pruning.
    const std::unordered_map<std::string, PruneLockInfo> locks;
    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 1000);
    BOOST_CHECK(limiting_lock.empty());
}

BOOST_AUTO_TEST_CASE(prune_lock_single)
{
    // The lock keeps the data of the index, the buffer is added on top.
    const auto locks = MakeLocks("basic block filter index", 700);
    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 700 - PRUNE_LOCK_BUFFER - 1);
    BOOST_CHECK_EQUAL(limiting_lock, "basic block filter index");
}

BOOST_AUTO_TEST_CASE(prune_lock_multiple)
{
    // The lowest lock is the limiting one.
    std::unordered_map<std::string, PruneLockInfo> locks = MakeLocks("index a", 700);
    PruneLockInfo info;
    info.height_first = 500;
    locks["index b"] = info;

    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 500 - PRUNE_LOCK_BUFFER - 1);
    BOOST_CHECK_EQUAL(limiting_lock, "index b");
}

BOOST_AUTO_TEST_CASE(prune_lock_never_set)
{
    // A lock which was never updated does not limit the pruning.
    std::unordered_map<std::string, PruneLockInfo> locks;
    PruneLockInfo info;
    info.height_first = std::numeric_limits<int>::max();
    locks["index"] = info;

    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 1000);
    BOOST_CHECK(limiting_lock.empty());
}

BOOST_AUTO_TEST_CASE(prune_lock_floor)
{
    // The returned height never drops below 1, even if the lock is very low.
    const auto locks = MakeLocks("index", 5);
    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 1);
    // The lock is not reported as the limiting one, because the height was
    // raised to the minimum of 1.
    BOOST_CHECK(limiting_lock.empty());
}

BOOST_AUTO_TEST_CASE(prune_lock_above_tip)
{
    // A lock which is above the tip does not limit the pruning.
    const auto locks = MakeLocks("index", 2000);
    std::string limiting_lock;
    BOOST_CHECK_EQUAL(GetLastPrunableHeight(locks, 1000, &limiting_lock), 1000);
    BOOST_CHECK(limiting_lock.empty());
}

BOOST_AUTO_TEST_CASE(prune_lock_update_and_delete)
{
    // The prune locks of the block manager can be updated and removed again.
    LOCK(cs_main);
    g_blockman.UpdatePruneLock("test index", PruneLockInfo{100});
    BOOST_CHECK_EQUAL(g_blockman.m_prune_locks.size(), 1UL);
    BOOST_CHECK_EQUAL(g_blockman.m_prune_locks["test index"].height_first, 100);

    g_blockman.UpdatePruneLock("test index", PruneLockInfo{200});
    BOOST_CHECK_EQUAL(g_blockman.m_prune_locks.size(), 1UL);
    BOOST_CHECK_EQUAL(g_blockman.m_prune_locks["test index"].height_first, 200);

    BOOST_CHECK(g_blockman.DeletePruneLock("test index"));
    BOOST_CHECK(g_blockman.m_prune_locks.empty());
    BOOST_CHECK(!g_blockman.DeletePruneLock("test index"));
}

BOOST_AUTO_TEST_SUITE_END()
