// Copyright (c) 2011-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <mempool_accept.h>
#include <policy/policy.h>
#include <script/script.h>
#include <timedata.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <limits>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(mempool_tests, BasicTestingSetup)

static constexpr auto REMOVAL_REASON_DUMMY = MemPoolRemovalReason::REPLACED;

BOOST_AUTO_TEST_CASE(MempoolRemoveTest)
{
    // Test CTxMemPool::remove functionality

    TestMemPoolEntryHelper entry;
    // Parent transaction with three children,
    // and three grand-children:
    CMutableTransaction txParent;
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_11;
    txParent.vout.resize(3);
    for (int i = 0; i < 3; i++)
    {
        txParent.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txParent.vout[i].nValue = 33000LL;
    }
    CMutableTransaction txChild[3];
    for (int i = 0; i < 3; i++)
    {
        txChild[i].vin.resize(1);
        txChild[i].vin[0].scriptSig = CScript() << OP_11;
        txChild[i].vin[0].prevout.hash = txParent.GetHash();
        txChild[i].vin[0].prevout.n = i;
        txChild[i].vout.resize(1);
        txChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txChild[i].vout[0].nValue = 11000LL;
    }
    CMutableTransaction txGrandChild[3];
    for (int i = 0; i < 3; i++)
    {
        txGrandChild[i].vin.resize(1);
        txGrandChild[i].vin[0].scriptSig = CScript() << OP_11;
        txGrandChild[i].vin[0].prevout.hash = txChild[i].GetHash();
        txGrandChild[i].vin[0].prevout.n = 0;
        txGrandChild[i].vout.resize(1);
        txGrandChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txGrandChild[i].vout[0].nValue = 11000LL;
    }


    CTxMemPool testPool;
    LOCK2(cs_main, testPool.cs);

    // Nothing in pool, remove should do nothing:
    unsigned int poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);

    // Just the parent:
    testPool.addUnchecked(entry.FromTx(txParent));
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 1);

    // Parent, children, grandchildren:
    testPool.addUnchecked(entry.FromTx(txParent));
    for (int i = 0; i < 3; i++)
    {
        testPool.addUnchecked(entry.FromTx(txChild[i]));
        testPool.addUnchecked(entry.FromTx(txGrandChild[i]));
    }
    // Remove Child[0], GrandChild[0] should be removed:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 2);
    // ... make sure grandchild and child are gone:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txGrandChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);
    // Remove parent, all children/grandchildren should go:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 5);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);

    // Add children and grandchildren, but NOT the parent (simulate the parent being in a block)
    for (int i = 0; i < 3; i++)
    {
        testPool.addUnchecked(entry.FromTx(txChild[i]));
        testPool.addUnchecked(entry.FromTx(txGrandChild[i]));
    }
    // Now remove the parent, as might happen if a block-re-org occurs but the parent cannot be
    // put into the mempool (maybe because it is non-standard):
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 6);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

template<typename name>
static void CheckSort(CTxMemPool &pool, std::vector<std::string> &sortedOrder) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    BOOST_CHECK_EQUAL(pool.size(), sortedOrder.size());
    typename CTxMemPool::indexed_transaction_set::index<name>::type::iterator it = pool.mapTx.get<name>().begin();
    int count=0;
    for (; it != pool.mapTx.get<name>().end(); ++it, ++count) {
        BOOST_CHECK_EQUAL(it->GetTx().GetHash().ToString(), sortedOrder[count]);
    }
}

BOOST_AUTO_TEST_CASE(MempoolIndexingTest)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    /* 3rd highest fee */
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx1));

    /* highest fee */
    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx2.vout[0].nValue = 2 * COIN;
    pool.addUnchecked(entry.Fee(20000LL).FromTx(tx2));

    /* lowest fee */
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx3.vout[0].nValue = 5 * COIN;
    pool.addUnchecked(entry.Fee(0LL).FromTx(tx3));

    /* 2nd highest fee */
    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vout.resize(1);
    tx4.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx4.vout[0].nValue = 6 * COIN;
    pool.addUnchecked(entry.Fee(15000LL).FromTx(tx4));

    /* equal fee rate to tx1, but newer */
    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vout.resize(1);
    tx5.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx5.vout[0].nValue = 11 * COIN;
    entry.nTime = 1;
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx5));
    BOOST_CHECK_EQUAL(pool.size(), 5U);

    std::vector<std::string> sortedOrder;
    sortedOrder.resize(5);
    sortedOrder[0] = tx3.GetHash().ToString(); // 0
    sortedOrder[1] = tx5.GetHash().ToString(); // 10000
    sortedOrder[2] = tx1.GetHash().ToString(); // 10000
    sortedOrder[3] = tx4.GetHash().ToString(); // 15000
    sortedOrder[4] = tx2.GetHash().ToString(); // 20000
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee but with high fee child */
    /* tx6 -> tx7 -> tx8, tx9 -> tx10 */
    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vout.resize(1);
    tx6.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx6.vout[0].nValue = 20 * COIN;
    pool.addUnchecked(entry.Fee(0LL).FromTx(tx6));
    BOOST_CHECK_EQUAL(pool.size(), 6U);
    // Check that at this point, tx6 is sorted low
    sortedOrder.insert(sortedOrder.begin(), tx6.GetHash().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    CTxMemPool::setEntries setAncestors;
    setAncestors.insert(pool.mapTx.find(tx6.GetHash()));
    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(1);
    tx7.vin[0].prevout = COutPoint(tx6.GetHash(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_11;
    tx7.vout.resize(2);
    tx7.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    tx7.vout[1].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[1].nValue = 1 * COIN;

    CTxMemPool::setEntries setAncestorsCalculated;
    std::string dummy;
    BOOST_CHECK_EQUAL(pool.CalculateMemPoolAncestors(entry.Fee(2000000LL).FromTx(tx7), setAncestorsCalculated, 100, 1000000, 1000, 1000000, dummy), true);
    BOOST_CHECK(setAncestorsCalculated == setAncestors);

    pool.addUnchecked(entry.FromTx(tx7), setAncestors);
    BOOST_CHECK_EQUAL(pool.size(), 7U);

    // Now tx6 should be sorted higher (high fee child): tx7, tx6, tx2, ...
    sortedOrder.erase(sortedOrder.begin());
    sortedOrder.push_back(tx6.GetHash().ToString());
    sortedOrder.push_back(tx7.GetHash().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee child of tx7 */
    CMutableTransaction tx8 = CMutableTransaction();
    tx8.vin.resize(1);
    tx8.vin[0].prevout = COutPoint(tx7.GetHash(), 0);
    tx8.vin[0].scriptSig = CScript() << OP_11;
    tx8.vout.resize(1);
    tx8.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx8.vout[0].nValue = 10 * COIN;
    setAncestors.insert(pool.mapTx.find(tx7.GetHash()));
    pool.addUnchecked(entry.Fee(0LL).Time(2).FromTx(tx8), setAncestors);

    // Now tx8 should be sorted low, but tx6/tx both high
    sortedOrder.insert(sortedOrder.begin(), tx8.GetHash().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee child of tx7 */
    CMutableTransaction tx9 = CMutableTransaction();
    tx9.vin.resize(1);
    tx9.vin[0].prevout = COutPoint(tx7.GetHash(), 1);
    tx9.vin[0].scriptSig = CScript() << OP_11;
    tx9.vout.resize(1);
    tx9.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx9.vout[0].nValue = 1 * COIN;
    pool.addUnchecked(entry.Fee(0LL).Time(3).FromTx(tx9), setAncestors);

    // tx9 should be sorted low
    BOOST_CHECK_EQUAL(pool.size(), 9U);
    sortedOrder.insert(sortedOrder.begin(), tx9.GetHash().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    std::vector<std::string> snapshotOrder = sortedOrder;

    setAncestors.insert(pool.mapTx.find(tx8.GetHash()));
    setAncestors.insert(pool.mapTx.find(tx9.GetHash()));
    /* tx10 depends on tx8 and tx9 and has a high fee*/
    CMutableTransaction tx10 = CMutableTransaction();
    tx10.vin.resize(2);
    tx10.vin[0].prevout = COutPoint(tx8.GetHash(), 0);
    tx10.vin[0].scriptSig = CScript() << OP_11;
    tx10.vin[1].prevout = COutPoint(tx9.GetHash(), 0);
    tx10.vin[1].scriptSig = CScript() << OP_11;
    tx10.vout.resize(1);
    tx10.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx10.vout[0].nValue = 10 * COIN;

    setAncestorsCalculated.clear();
    BOOST_CHECK_EQUAL(pool.CalculateMemPoolAncestors(entry.Fee(200000LL).Time(4).FromTx(tx10), setAncestorsCalculated, 100, 1000000, 1000, 1000000, dummy), true);
    BOOST_CHECK(setAncestorsCalculated == setAncestors);

    pool.addUnchecked(entry.FromTx(tx10), setAncestors);

    /**
     *  tx8 and tx9 should both now be sorted higher
     *  Final order after tx10 is added:
     *
     *  tx3 = 0 (1)
     *  tx5 = 10000 (1)
     *  tx1 = 10000 (1)
     *  tx4 = 15000 (1)
     *  tx2 = 20000 (1)
     *  tx9 = 200k (2 txs)
     *  tx8 = 200k (2 txs)
     *  tx10 = 200k (1 tx)
     *  tx6 = 2.2M (5 txs)
     *  tx7 = 2.2M (4 txs)
     */
    sortedOrder.erase(sortedOrder.begin(), sortedOrder.begin()+2); // take out tx9, tx8 from the beginning
    sortedOrder.insert(sortedOrder.begin()+5, tx9.GetHash().ToString());
    sortedOrder.insert(sortedOrder.begin()+6, tx8.GetHash().ToString());
    sortedOrder.insert(sortedOrder.begin()+7, tx10.GetHash().ToString()); // tx10 is just before tx6
    CheckSort<descendant_score>(pool, sortedOrder);

    // there should be 10 transactions in the mempool
    BOOST_CHECK_EQUAL(pool.size(), 10U);

    // Now try removing tx10 and verify the sort order returns to normal
    pool.removeRecursive(pool.mapTx.find(tx10.GetHash())->GetTx(), REMOVAL_REASON_DUMMY);
    CheckSort<descendant_score>(pool, snapshotOrder);

    pool.removeRecursive(pool.mapTx.find(tx9.GetHash())->GetTx(), REMOVAL_REASON_DUMMY);
    pool.removeRecursive(pool.mapTx.find(tx8.GetHash())->GetTx(), REMOVAL_REASON_DUMMY);
}

BOOST_AUTO_TEST_CASE(MempoolAncestorIndexingTest)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    /* 3rd highest fee */
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx1));

    /* highest fee */
    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx2.vout[0].nValue = 2 * COIN;
    pool.addUnchecked(entry.Fee(20000LL).FromTx(tx2));
    uint64_t tx2Size = GetVirtualTransactionSize(CTransaction(tx2));

    /* lowest fee */
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx3.vout[0].nValue = 5 * COIN;
    pool.addUnchecked(entry.Fee(0LL).FromTx(tx3));

    /* 2nd highest fee */
    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vout.resize(1);
    tx4.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx4.vout[0].nValue = 6 * COIN;
    pool.addUnchecked(entry.Fee(15000LL).FromTx(tx4));

    /* equal fee rate to tx1, but newer */
    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vout.resize(1);
    tx5.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx5.vout[0].nValue = 11 * COIN;
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx5));
    BOOST_CHECK_EQUAL(pool.size(), 5U);

    std::vector<std::string> sortedOrder;
    sortedOrder.resize(5);
    sortedOrder[0] = tx2.GetHash().ToString(); // 20000
    sortedOrder[1] = tx4.GetHash().ToString(); // 15000
    // tx1 and tx5 are both 10000
    // Ties are broken by  timestamp, so determine which
    sortedOrder[2] = tx1.GetHash().ToString();
    sortedOrder[3] = tx5.GetHash().ToString();
    
    sortedOrder[4] = tx3.GetHash().ToString(); // 0

    CheckSort<ancestor_score>(pool, sortedOrder);

    /* low fee parent with high fee child */
    /* tx6 (0) -> tx7 (high) */
    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vout.resize(1);
    tx6.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx6.vout[0].nValue = 20 * COIN;
    uint64_t tx6Size = GetVirtualTransactionSize(CTransaction(tx6));

    pool.addUnchecked(entry.Fee(0LL).FromTx(tx6));
    BOOST_CHECK_EQUAL(pool.size(), 6U);
    // Ties are broken by tstamp
    sortedOrder.push_back(tx6.GetHash().ToString());
    
    CheckSort<ancestor_score>(pool, sortedOrder);

    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(1);
    tx7.vin[0].prevout = COutPoint(tx6.GetHash(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_11;
    tx7.vout.resize(1);
    tx7.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    uint64_t tx7Size = GetVirtualTransactionSize(CTransaction(tx7));

    /* set the fee to just below tx2's feerate when including ancestor */
    CAmount fee = (20000/tx2Size)*(tx7Size + tx6Size) - 1;

    pool.addUnchecked(entry.Fee(fee).FromTx(tx7));
    BOOST_CHECK_EQUAL(pool.size(), 7U);
    sortedOrder.insert(sortedOrder.begin()+1, tx7.GetHash().ToString());
    CheckSort<ancestor_score>(pool, sortedOrder);

    /* after tx6 is mined, tx7 should move up in the sort */
    std::vector<CTransactionRef> vtx;
    vtx.push_back(MakeTransactionRef(tx6));
    pool.removeForBlock(vtx, 1);

    sortedOrder.erase(sortedOrder.begin()+1);
    // Ties are broken by tstamp
    sortedOrder.pop_back();
    sortedOrder.insert(sortedOrder.begin(), tx7.GetHash().ToString());
    CheckSort<ancestor_score>(pool, sortedOrder);

    // High-fee parent, low-fee child
    // tx7 -> tx8
    CMutableTransaction tx8 = CMutableTransaction();
    tx8.vin.resize(1);
    tx8.vin[0].prevout  = COutPoint(tx7.GetHash(), 0);
    tx8.vin[0].scriptSig = CScript() << OP_11;
    tx8.vout.resize(1);
    tx8.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx8.vout[0].nValue = 10*COIN;

    // Check that we sort by min(feerate, ancestor_feerate):
    // set the fee so that the ancestor feerate is above tx1/5,
    // but the transaction's own feerate is lower
    pool.addUnchecked(entry.Fee(5000LL).FromTx(tx8));
    sortedOrder.insert(sortedOrder.end()-1, tx8.GetHash().ToString());
    CheckSort<ancestor_score>(pool, sortedOrder);
}


BOOST_AUTO_TEST_CASE(MempoolSizeLimitTest)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vin.resize(1);
    tx1.vin[0].scriptSig = CScript() << OP_1;
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_1 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx1));

    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vin.resize(1);
    tx2.vin[0].scriptSig = CScript() << OP_2;
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_2 << OP_EQUAL;
    tx2.vout[0].nValue = 10 * COIN;
    pool.addUnchecked(entry.Fee(5000LL).FromTx(tx2));

    pool.TrimToSize(pool.DynamicMemoryUsage()); // should do nothing
    BOOST_CHECK(pool.exists(tx1.GetHash()));
    BOOST_CHECK(pool.exists(tx2.GetHash()));

    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4); // should remove the lower-feerate transaction
    BOOST_CHECK(pool.exists(tx1.GetHash()));
    BOOST_CHECK(!pool.exists(tx2.GetHash()));

    pool.addUnchecked(entry.FromTx(tx2));
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vin.resize(1);
    tx3.vin[0].prevout = COutPoint(tx2.GetHash(), 0);
    tx3.vin[0].scriptSig = CScript() << OP_2;
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_3 << OP_EQUAL;
    tx3.vout[0].nValue = 10 * COIN;
    pool.addUnchecked(entry.Fee(20000LL).FromTx(tx3));

    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4); // tx3 should pay for tx2 (CPFP)
    BOOST_CHECK(!pool.exists(tx1.GetHash()));
    BOOST_CHECK(pool.exists(tx2.GetHash()));
    BOOST_CHECK(pool.exists(tx3.GetHash()));

    pool.TrimToSize(GetVirtualTransactionSize(CTransaction(tx1))); // mempool is limited to tx1's size in memory usage, so nothing fits
    BOOST_CHECK(!pool.exists(tx1.GetHash()));
    BOOST_CHECK(!pool.exists(tx2.GetHash()));
    BOOST_CHECK(!pool.exists(tx3.GetHash()));

    CFeeRate maxFeeRateRemoved(25000, GetVirtualTransactionSize(CTransaction(tx3)) + GetVirtualTransactionSize(CTransaction(tx2)));
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(), maxFeeRateRemoved.GetFeePerK() + 1000);

    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vin.resize(2);
    tx4.vin[0].prevout.SetNull();
    tx4.vin[0].scriptSig = CScript() << OP_4;
    tx4.vin[1].prevout.SetNull();
    tx4.vin[1].scriptSig = CScript() << OP_4;
    tx4.vout.resize(2);
    tx4.vout[0].scriptPubKey = CScript() << OP_4 << OP_EQUAL;
    tx4.vout[0].nValue = 10 * COIN;
    tx4.vout[1].scriptPubKey = CScript() << OP_4 << OP_EQUAL;
    tx4.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vin.resize(2);
    tx5.vin[0].prevout = COutPoint(tx4.GetHash(), 0);
    tx5.vin[0].scriptSig = CScript() << OP_4;
    tx5.vin[1].prevout.SetNull();
    tx5.vin[1].scriptSig = CScript() << OP_5;
    tx5.vout.resize(2);
    tx5.vout[0].scriptPubKey = CScript() << OP_5 << OP_EQUAL;
    tx5.vout[0].nValue = 10 * COIN;
    tx5.vout[1].scriptPubKey = CScript() << OP_5 << OP_EQUAL;
    tx5.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vin.resize(2);
    tx6.vin[0].prevout = COutPoint(tx4.GetHash(), 1);
    tx6.vin[0].scriptSig = CScript() << OP_4;
    tx6.vin[1].prevout.SetNull();
    tx6.vin[1].scriptSig = CScript() << OP_6;
    tx6.vout.resize(2);
    tx6.vout[0].scriptPubKey = CScript() << OP_6 << OP_EQUAL;
    tx6.vout[0].nValue = 10 * COIN;
    tx6.vout[1].scriptPubKey = CScript() << OP_6 << OP_EQUAL;
    tx6.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(2);
    tx7.vin[0].prevout = COutPoint(tx5.GetHash(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_5;
    tx7.vin[1].prevout = COutPoint(tx6.GetHash(), 0);
    tx7.vin[1].scriptSig = CScript() << OP_6;
    tx7.vout.resize(2);
    tx7.vout[0].scriptPubKey = CScript() << OP_7 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    tx7.vout[1].scriptPubKey = CScript() << OP_7 << OP_EQUAL;
    tx7.vout[1].nValue = 10 * COIN;

    pool.addUnchecked(entry.Fee(7000LL).FromTx(tx4));
    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx5));
    pool.addUnchecked(entry.Fee(1100LL).FromTx(tx6));
    pool.addUnchecked(entry.Fee(9000LL).FromTx(tx7));

    // we only require this to remove, at max, 2 txn, because it's not clear what we're really optimizing for aside from that
    pool.TrimToSize(pool.DynamicMemoryUsage() - 1);
    BOOST_CHECK(pool.exists(tx4.GetHash()));
    BOOST_CHECK(pool.exists(tx6.GetHash()));
    BOOST_CHECK(!pool.exists(tx7.GetHash()));

    if (!pool.exists(tx5.GetHash()))
        pool.addUnchecked(entry.Fee(1000LL).FromTx(tx5));
    pool.addUnchecked(entry.Fee(9000LL).FromTx(tx7));

    pool.TrimToSize(pool.DynamicMemoryUsage() / 2); // should maximize mempool size by only removing 5/7
    BOOST_CHECK(pool.exists(tx4.GetHash()));
    BOOST_CHECK(!pool.exists(tx5.GetHash()));
    BOOST_CHECK(pool.exists(tx6.GetHash()));
    BOOST_CHECK(!pool.exists(tx7.GetHash()));

    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx5));
    pool.addUnchecked(entry.Fee(9000LL).FromTx(tx7));

    std::vector<CTransactionRef> vtx;
    SetMockTime(42);
    SetMockTime(42 + CTxMemPool::ROLLING_FEE_HALFLIFE);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(), maxFeeRateRemoved.GetFeePerK() + 1000);
    // ... we should keep the same min fee until we get a block
    pool.removeForBlock(vtx, 1);
    SetMockTime(42 + 2*CTxMemPool::ROLLING_FEE_HALFLIFE);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(), llround((maxFeeRateRemoved.GetFeePerK() + 1000)/2.0));
    // ... then feerate should drop 1/2 each halflife

    SetMockTime(42 + 2*CTxMemPool::ROLLING_FEE_HALFLIFE + CTxMemPool::ROLLING_FEE_HALFLIFE/2);
    BOOST_CHECK_EQUAL(pool.GetMinFee(pool.DynamicMemoryUsage() * 5 / 2).GetFeePerK(), llround((maxFeeRateRemoved.GetFeePerK() + 1000)/4.0));
    // ... with a 1/2 halflife when mempool is < 1/2 its target size

    SetMockTime(42 + 2*CTxMemPool::ROLLING_FEE_HALFLIFE + CTxMemPool::ROLLING_FEE_HALFLIFE/2 + CTxMemPool::ROLLING_FEE_HALFLIFE/4);
    BOOST_CHECK_EQUAL(pool.GetMinFee(pool.DynamicMemoryUsage() * 9 / 2).GetFeePerK(), llround((maxFeeRateRemoved.GetFeePerK() + 1000)/8.0));
    // ... with a 1/4 halflife when mempool is < 1/4 its target size

    SetMockTime(42 + 7*CTxMemPool::ROLLING_FEE_HALFLIFE + CTxMemPool::ROLLING_FEE_HALFLIFE/2 + CTxMemPool::ROLLING_FEE_HALFLIFE/4);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(), 1000);
    // ... but feerate should never drop below 1000

    SetMockTime(42 + 8*CTxMemPool::ROLLING_FEE_HALFLIFE + CTxMemPool::ROLLING_FEE_HALFLIFE/2 + CTxMemPool::ROLLING_FEE_HALFLIFE/4);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(), 0);
    // ... unless it has gone all the way to 0 (after getting past 1000/2)

    SetMockTime(0);
}

inline CTransactionRef make_tx(std::vector<CAmount>&& output_values, std::vector<CTransactionRef>&& inputs=std::vector<CTransactionRef>(), std::vector<uint32_t>&& input_indices=std::vector<uint32_t>())
{
    CMutableTransaction tx = CMutableTransaction();
    tx.vin.resize(inputs.size());
    tx.vout.resize(output_values.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        tx.vin[i].prevout.hash = inputs[i]->GetHash();
        tx.vin[i].prevout.n = input_indices.size() > i ? input_indices[i] : 0;
    }
    for (size_t i = 0; i < output_values.size(); ++i) {
        tx.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout[i].nValue = output_values[i];
    }
    return MakeTransactionRef(tx);
}


BOOST_AUTO_TEST_CASE(MempoolAncestryTests)
{
    size_t ancestors, descendants;

    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    /* Base transaction */
    //
    // [tx1]
    //
    CTransactionRef tx1 = make_tx(/* output_values */ {10 * COIN});
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx1));

    // Ancestors / descendants should be 1 / 1 (itself / itself)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 1ULL);

    /* Child transaction */
    //
    // [tx1].0 <- [tx2]
    //
    CTransactionRef tx2 = make_tx(/* output_values */ {495 * CENT, 5 * COIN}, /* inputs */ {tx1});
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx2));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     2 (tx1,2)
    // tx2          2 (tx1,2)   2 (tx1,2)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 2ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 2ULL);

    /* Grand-child 1 */
    //
    // [tx1].0 <- [tx2].0 <- [tx3]
    //
    CTransactionRef tx3 = make_tx(/* output_values */ {290 * CENT, 200 * CENT}, /* inputs */ {tx2});
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx3));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     3 (tx1,2,3)
    // tx2          2 (tx1,2)   3 (tx1,2,3)
    // tx3          3 (tx1,2,3) 3 (tx1,2,3)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);

    /* Grand-child 2 */
    //
    // [tx1].0 <- [tx2].0 <- [tx3]
    //              |
    //              \---1 <- [tx4]
    //
    CTransactionRef tx4 = make_tx(/* output_values */ {290 * CENT, 250 * CENT}, /* inputs */ {tx2}, /* input_indices */ {1});
    pool.addUnchecked(entry.Fee(10000LL).FromTx(tx4));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     4 (tx1,2,3,4)
    // tx2          2 (tx1,2)   4 (tx1,2,3,4)
    // tx3          3 (tx1,2,3) 4 (tx1,2,3,4)
    // tx4          3 (tx1,2,4) 4 (tx1,2,3,4)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);

    /* Make an alternate branch that is longer and connect it to tx3 */
    //
    // [ty1].0 <- [ty2].0 <- [ty3].0 <- [ty4].0 <- [ty5].0
    //                                              |
    // [tx1].0 <- [tx2].0 <- [tx3].0 <- [ty6] --->--/
    //              |
    //              \---1 <- [tx4]
    //
    CTransactionRef ty1, ty2, ty3, ty4, ty5;
    CTransactionRef* ty[5] = {&ty1, &ty2, &ty3, &ty4, &ty5};
    CAmount v = 5 * COIN;
    for (uint64_t i = 0; i < 5; i++) {
        CTransactionRef& tyi = *ty[i];
        tyi = make_tx(/* output_values */ {v}, /* inputs */ i > 0 ? std::vector<CTransactionRef>{*ty[i - 1]} : std::vector<CTransactionRef>{});
        v -= 50 * CENT;
        pool.addUnchecked(entry.Fee(10000LL).FromTx(tyi));
        pool.GetTransactionAncestry(tyi->GetHash(), ancestors, descendants);
        BOOST_CHECK_EQUAL(ancestors, i+1);
        BOOST_CHECK_EQUAL(descendants, i+1);
    }
    CTransactionRef ty6 = make_tx(/* output_values */ {5 * COIN}, /* inputs */ {tx3, ty5});
    pool.addUnchecked(entry.Fee(10000LL).FromTx(ty6));

    // Ancestors / descendants should be:
    // transaction  ancestors           descendants
    // ============ =================== ===========
    // tx1          1 (tx1)             5 (tx1,2,3,4, ty6)
    // tx2          2 (tx1,2)           5 (tx1,2,3,4, ty6)
    // tx3          3 (tx1,2,3)         5 (tx1,2,3,4, ty6)
    // tx4          3 (tx1,2,4)         5 (tx1,2,3,4, ty6)
    // ty1          1 (ty1)             6 (ty1,2,3,4,5,6)
    // ty2          2 (ty1,2)           6 (ty1,2,3,4,5,6)
    // ty3          3 (ty1,2,3)         6 (ty1,2,3,4,5,6)
    // ty4          4 (y1234)           6 (ty1,2,3,4,5,6)
    // ty5          5 (y12345)          6 (ty1,2,3,4,5,6)
    // ty6          9 (tx123, ty123456) 6 (ty1,2,3,4,5,6)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(ty1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 4ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty5->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 5ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty6->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 9ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
}

BOOST_AUTO_TEST_CASE(MempoolWtxidIndexingTest)
{
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.hash = InsecureRand256();
    mtx.vin[0].prevout.n = 0;
    mtx.vin[0].scriptWitness.stack.push_back(std::vector<unsigned char>{0x01, 0x02});
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1 * COIN;

    CTransactionRef tx = MakeTransactionRef(mtx);
    BOOST_CHECK(tx->GetHash() != tx->GetWitnessHash());

    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx));

    const GenTxid gtxid_txid = GenTxid::Txid(tx->GetHash());
    const GenTxid gtxid_wtxid = GenTxid::Wtxid(tx->GetWitnessHash());

    BOOST_CHECK(pool.exists(gtxid_txid));
    BOOST_CHECK(pool.exists(gtxid_wtxid));
    BOOST_CHECK(!pool.exists(GenTxid::Wtxid(tx->GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(tx->GetWitnessHash())));

    TxMempoolInfo info_txid = pool.info(gtxid_txid);
    TxMempoolInfo info_wtxid = pool.info(gtxid_wtxid);
    BOOST_CHECK(info_txid.tx != nullptr);
    BOOST_CHECK(info_wtxid.tx != nullptr);
    BOOST_CHECK(info_txid.tx->GetWitnessHash() == tx->GetWitnessHash());
    BOOST_CHECK(info_wtxid.tx->GetHash() == tx->GetHash());
    BOOST_CHECK(pool.info(GenTxid::Wtxid(InsecureRand256())).tx == nullptr);
}

BOOST_AUTO_TEST_CASE(MempoolEvmRbfTests)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    const EvmAddressData sender{{0x01, 0x02, 0x03}};
    const EvmAddressWithNonce addrNonce{0, sender};

    // Parent -> EVM tx -> UTXO descendant
    CTransactionRef tx1 = make_tx({10 * COIN});
    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx1));

    CTransactionRef tx2 = make_tx({5 * COIN}, {tx1});
    auto entry2 = entry.Fee(1000LL).FromTx(tx2);
    entry2.SetCustomTxType(CustomTxType::EvmTx);
    entry2.SetEVMAddrAndNonce(addrNonce);
    entry2.SetEVMRbfMinTipFee(100);
    pool.addUnchecked(entry2);

    CTransactionRef tx3 = make_tx({1 * COIN}, {tx2});
    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx3));

    size_t ancestors, descendants;
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(descendants, 3ULL);
    BOOST_CHECK(!pool.getAccountViewDirty());

    // Low-fee replacement fails without mutating mempool
    CTransactionRef txLow = make_tx({5 * COIN}, {tx1});
    auto entryLow = entry.Fee(1000LL).FromTx(txLow);
    entryLow.SetCustomTxType(CustomTxType::EvmTx);
    entryLow.SetEVMAddrAndNonce(addrNonce);

    bool senderLimitFlag{false};
    CTxMemPool::setEntries evmConflicts;
    BOOST_CHECK(!pool.checkAddressNonceAndFee(entryLow, 50, sender, senderLimitFlag, evmConflicts));
    BOOST_CHECK(!senderLimitFlag);
    BOOST_CHECK(evmConflicts.empty());
    BOOST_CHECK(pool.exists(tx2->GetHash()));
    BOOST_CHECK(pool.exists(tx3->GetHash()));
    BOOST_CHECK(!pool.getAccountViewDirty());

    // Valid-fee replacement collects conflicts without mutating mempool yet
    CTransactionRef txHigh = make_tx({5 * COIN}, {tx1});
    auto entryHigh = entry.Fee(2000LL).FromTx(txHigh);
    entryHigh.SetCustomTxType(CustomTxType::EvmTx);
    entryHigh.SetEVMAddrAndNonce(addrNonce);

    BOOST_CHECK(pool.checkAddressNonceAndFee(entryHigh, 100, sender, senderLimitFlag, evmConflicts));
    BOOST_CHECK(!senderLimitFlag);
    BOOST_CHECK_EQUAL(evmConflicts.size(), 1ULL);
    BOOST_CHECK(pool.exists(tx2->GetHash()));
    BOOST_CHECK(pool.exists(tx3->GetHash()));
    BOOST_CHECK(!pool.getAccountViewDirty());

    // Staging descendants + RemoveStaged evicts tx2 and tx3, marks accountsViewDirty,
    // and updates tx1's descendant count
    CTxMemPool::setEntries allConflicting;
    for (const auto &it : evmConflicts) {
        pool.CalculateDescendants(it, allConflicting);
    }
    BOOST_CHECK_EQUAL(allConflicting.size(), 2ULL);
    pool.RemoveStaged(allConflicting, false, MemPoolRemovalReason::REPLACED);

    BOOST_CHECK(!pool.exists(tx2->GetHash()));
    BOOST_CHECK(!pool.exists(tx3->GetHash()));
    BOOST_CHECK(pool.getAccountViewDirty());
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(descendants, 1ULL);

    // Re-add an EVM entry and verify MEMPOOL_MAX_ETH_RBF enforcement via IncrementEvmRbfCount
    pool.addUnchecked(entry2);
    for (uint32_t i = 0; i < MEMPOOL_MAX_ETH_RBF; ++i) {
        pool.IncrementEvmRbfCount(sender);
    }
    senderLimitFlag = false;
    evmConflicts.clear();
    BOOST_CHECK(!pool.checkAddressNonceAndFee(entryHigh, 100, sender, senderLimitFlag, evmConflicts));
    BOOST_CHECK(senderLimitFlag);
    BOOST_CHECK(evmConflicts.empty());
}

BOOST_AUTO_TEST_CASE(MempoolUseExactRbfFeeIncrementTests)
{
    Consensus::Params consensus = Params().GetConsensus();

    // below the activation height the legacy whole-percent interpretation is used
    consensus.DF25Height = 100;
    BOOST_CHECK(!UseExactRbfFeeIncrement(0, consensus));
    BOOST_CHECK(!UseExactRbfFeeIncrement(99, consensus));

    // from the activation height on the exact fraction is used
    BOOST_CHECK(UseExactRbfFeeIncrement(100, consensus));
    BOOST_CHECK(UseExactRbfFeeIncrement(101, consensus));

    // a disabled activation height keeps the legacy behaviour for every height
    consensus.DF25Height = std::numeric_limits<int>::max();
    BOOST_CHECK(!UseExactRbfFeeIncrement(0, consensus));
    BOOST_CHECK(!UseExactRbfFeeIncrement(100, consensus));
    BOOST_CHECK(!UseExactRbfFeeIncrement(std::numeric_limits<uint32_t>::max(), consensus));
}

/**
 * The mempool finality cutoff.
 *
 * A time locked transaction is only accepted by the mempool if it can be mined in the next block.
 * The cutoff of the mempool acceptance rule is evaluated against the median time past of the
 * chain tip (LOCKTIME_MEDIAN_TIME_PAST), so the peer derived time offset does not influence the
 * mempool acceptance rule. The node level IsFinalTx() (used by the wallet interface) uses the
 * applied offset instead, which is the median of the collected peer samples.
 */
BOOST_FIXTURE_TEST_CASE(MempoolFinalityCutoffTests, TestChain100Setup)
{
    LOCK(cs_main);

    // The time offset tracker is global: make sure that the samples of this test case do not
    // leak into the other test cases.
    struct TimeOffsetsGuard {
        ~TimeOffsetsGuard() { GetTimeOffsets().Clear(); }
    } guard;

    const int64_t nOffset{60 * 60}; // one hour
    const int64_t nLockTime{GetTime() + 30 * 60};

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TX_VERSION_2;
    tx.nLockTime = nLockTime;
    tx.vin.resize(1);
    tx.vout.resize(1);
    // The sequence is not SEQUENCE_FINAL, so the absolute lock time is enforced.
    tx.vin[0].nSequence = 0xFFFFFFFE;
    tx.vin[0].scriptSig = CScript() << OP_11;
    tx.vin[0].prevout = COutPoint(uint256S("1"), 0);
    tx.vout[0].nValue = CENT;
    tx.vout[0].scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Without any peer sample the applied offset is zero and the transaction is not final yet.
    GetTimeOffsets().Clear();
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);
    BOOST_CHECK(!CheckFinalTx(CTransaction(tx), 0));

    // The mempool cutoff uses the median time past of the chain tip, so the transaction is not
    // final either.
    BOOST_CHECK(!CheckFinalTx(CTransaction(tx), STANDARD_LOCKTIME_VERIFY_FLAGS));

    // With a positive peer derived offset the node level cutoff (adjusted clock) moves forward
    // and the transaction is final, while the mempool cutoff stays unaffected by the offset.
    for (int i = 0; i < 5; ++i) {
        AddTimeOffsetSample(nOffset);
    }
    BOOST_CHECK_EQUAL(GetTimeOffset(), nOffset);
    BOOST_CHECK(CheckFinalTx(CTransaction(tx), 0));
    BOOST_CHECK(!CheckFinalTx(CTransaction(tx), STANDARD_LOCKTIME_VERIFY_FLAGS));

    // The mempool rejects the time locked transaction as non-final.
    CValidationState state;
    BOOST_CHECK(!AcceptToMemoryPool(mempool, state, MakeTransactionRef(tx),
                                    nullptr /* pfMissingInputs */,
                                    nullptr /* plTxnReplaced */,
                                    true /* bypass_limits */,
                                    0 /* nAbsurdFee */));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "non-final");
    BOOST_CHECK_EQUAL(mempool.size(), 0UL);

    GetTimeOffsets().Clear();
}

BOOST_FIXTURE_TEST_CASE(MempoolRulesChangedBetweenTests, TestChain100Setup)
{
    LOCK2(cs_main, mempool.cs);

    const auto &consensus = Params().GetConsensus();

    // Start from a clean mempool state (accountsViewHeight == 0, accountsViewDirty == false).
    mempool.clear();
    BOOST_CHECK(!mempool.getAccountViewDirty());

    // Calling removeForBlock with an empty block vector at a height that crosses an active
    // upgrade height (e.g. DF25Height or nBalanceOverflowHeight in regtest) triggers
    // MempoolRulesChangedBetween() -> rebuildAccountsView() and leaves the view clean.
    if (consensus.DF25Height > 0 && consensus.DF25Height < std::numeric_limits<int>::max()) {
        const unsigned int beforeHeight = static_cast<unsigned int>(consensus.DF25Height - 1);
        mempool.removeForBlock({}, beforeHeight);
        BOOST_CHECK(!mempool.getAccountViewDirty());

        // Subsequent block above the activation height does not leave the view dirty.
        mempool.removeForBlock({}, static_cast<unsigned int>(consensus.DF25Height));
        BOOST_CHECK(!mempool.getAccountViewDirty());
    }

    if (consensus.nBalanceOverflowHeight > 0 &&
        consensus.nBalanceOverflowHeight < std::numeric_limits<int>::max()) {
        mempool.clear();
        BOOST_CHECK(!mempool.getAccountViewDirty());
        const unsigned int crossingBlock = static_cast<unsigned int>(consensus.nBalanceOverflowHeight - 1);
        mempool.removeForBlock({}, crossingBlock);
        BOOST_CHECK(!mempool.getAccountViewDirty());
    }

    // Explicitly marking the accounts view dirty is cleared by removeForBlock() rebuild.
    mempool.setAccountViewDirty();
    BOOST_CHECK(mempool.getAccountViewDirty());
    mempool.removeForBlock({}, static_cast<unsigned int>(::ChainActive().Height()));
    BOOST_CHECK(!mempool.getAccountViewDirty());
}

BOOST_AUTO_TEST_CASE(MempoolEvictionAndExpiryEdgeCases)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    // Build a low-fee parent -> child chain and a high-fee independent transaction.
    CTransactionRef txLowParent = make_tx({10 * COIN});
    CTransactionRef txLowChild = make_tx({5 * COIN}, {txLowParent});
    CTransactionRef txHigh = make_tx({11 * COIN});

    pool.addUnchecked(entry.Fee(100LL).Time(1000).FromTx(txLowParent));
    pool.addUnchecked(entry.Fee(100LL).Time(2000).FromTx(txLowChild));
    pool.addUnchecked(entry.Fee(100000LL).Time(2000).FromTx(txHigh));
    BOOST_CHECK_EQUAL(pool.size(), 3UL);

    // Expiring transactions with timestamp < 1500 removes txLowParent and its descendant
    // txLowChild, while preserving the independent txHigh.
    pool.Expire(1500, 1500);
    BOOST_CHECK(!pool.exists(txLowParent->GetHash()));
    BOOST_CHECK(!pool.exists(txLowChild->GetHash()));
    BOOST_CHECK(pool.exists(txHigh->GetHash()));
    BOOST_CHECK_EQUAL(pool.size(), 1UL);

    // Re-add the low-fee chain and verify TrimToSize evicts the low-fee package first
    // and raises the rolling minimum fee.
    pool.addUnchecked(entry.Fee(100LL).Time(2000).FromTx(txLowParent));
    pool.addUnchecked(entry.Fee(100LL).Time(2000).FromTx(txLowChild));
    BOOST_CHECK_EQUAL(pool.size(), 3UL);

    const size_t sizeWithOneTx = pool.DynamicMemoryUsage() / 2;
    pool.TrimToSize(sizeWithOneTx);
    BOOST_CHECK(pool.exists(txHigh->GetHash()));
    BOOST_CHECK(!pool.exists(txLowChild->GetHash()));
    BOOST_CHECK(pool.GetMinFee(1).GetFeePerK() > 0);

    // Trimming to 0 bytes evicts all remaining transactions.
    pool.TrimToSize(0);
    BOOST_CHECK_EQUAL(pool.size(), 0UL);
}

BOOST_AUTO_TEST_CASE(MempoolAncestorDescendantLimitEdgeCases)
{
    CTxMemPool pool;
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    // Build a 3-tx chain: tx1 -> tx2 -> tx3
    CTransactionRef tx1 = make_tx({10 * COIN});
    CTransactionRef tx2 = make_tx({9 * COIN}, {tx1});
    CTransactionRef tx3 = make_tx({8 * COIN}, {tx2});
    CTransactionRef tx4 = make_tx({7 * COIN}, {tx3});

    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx1));
    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx2));
    pool.addUnchecked(entry.Fee(1000LL).FromTx(tx3));

    CTxMemPoolEntry entry4 = entry.Fee(1000LL).FromTx(tx4);
    CTxMemPool::setEntries ancestors;
    std::string errString;

    // Within limits (4 ancestors allowed): succeeds and finds 3 in-mempool ancestors.
    BOOST_CHECK(pool.CalculateMemPoolAncestors(entry4, ancestors, 4, 100000, 10, 100000, errString));
    BOOST_CHECK_EQUAL(ancestors.size(), 3UL);

    // Exceeding ancestor count limit (limit = 3, chain would be 4): fails.
    ancestors.clear();
    errString.clear();
    BOOST_CHECK(!pool.CalculateMemPoolAncestors(entry4, ancestors, 3, 100000, 10, 100000, errString));
    BOOST_CHECK(!errString.empty());

    // Exceeding descendant count limit on tx1 (limit = 3, chain would be 4): fails.
    ancestors.clear();
    errString.clear();
    BOOST_CHECK(!pool.CalculateMemPoolAncestors(entry4, ancestors, 10, 100000, 3, 100000, errString));
    BOOST_CHECK(!errString.empty());

    // Exceeding ancestor size limit (1 byte): fails.
    ancestors.clear();
    errString.clear();
    BOOST_CHECK(!pool.CalculateMemPoolAncestors(entry4, ancestors, 10, 1, 10, 100000, errString));
    BOOST_CHECK(!errString.empty());
}

BOOST_AUTO_TEST_SUITE_END()
