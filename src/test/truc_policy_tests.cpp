// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/packages.h>
#include <policy/truc_policy.h>
#include <primitives/transaction.h>
#include <test/setup_common.h>
#include <txmempool.h>
#include <util/rbf.h>

#include <boost/test/unit_test.hpp>

namespace {

CTransactionRef MakeTx(int32_t version, const std::vector<COutPoint> &inputs, size_t num_outputs = 1)
{
    CMutableTransaction mtx;
    mtx.nVersion = version;
    for (const auto &outpoint : inputs) {
        mtx.vin.emplace_back(outpoint, CScript(), uint32_t{CTxIn::SEQUENCE_FINAL});
    }
    for (size_t i = 0; i < num_outputs; ++i) {
        mtx.vout.emplace_back(1000 * COIN, CScript() << OP_TRUE);
    }
    return MakeTransactionRef(std::move(mtx));
}

CTxMemPoolEntry MakeEntry(const CTransactionRef &tx, CAmount fee = 1000)
{
    return CTxMemPoolEntry(tx, fee, /*nTime=*/0, /*nHeight=*/1, /*spendsCoinbase=*/false, /*sigOpCost=*/0, LockPoints());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(truc_policy_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(truc_signals_rbf)
{
    const COutPoint dummy(uint256S("01"), 0);
    auto v3_tx = MakeTx(TRUC_VERSION, {dummy});
    auto v4_tx = MakeTx(CTransaction::CURRENT_VERSION, {dummy});

    // TRUC (v3) transactions implicitly signal RBF even with SEQUENCE_FINAL.
    BOOST_CHECK(IsTrucTx(*v3_tx));
    BOOST_CHECK(SignalsOptInRBF(*v3_tx));

    // Non-TRUC (v4) transactions with SEQUENCE_FINAL do not signal RBF.
    BOOST_CHECK(!IsTrucTx(*v4_tx));
    BOOST_CHECK(!SignalsOptInRBF(*v4_tx));
}

BOOST_AUTO_TEST_CASE(single_truc_checks)
{
    CTxMemPool pool;
    LOCK(pool.cs);

    const COutPoint confirmed_a(uint256S("aa"), 0);
    const COutPoint confirmed_b(uint256S("bb"), 0);

    // 1. Standalone v3 tx: allowed up to TRUC_MAX_VSIZE, rejected above.
    auto v3_parent = MakeTx(TRUC_VERSION, {confirmed_a}, 2);
    BOOST_CHECK(!SingleTRUCChecks(pool, v3_parent, {}, TRUC_MAX_VSIZE).has_value());
    auto too_big = SingleTRUCChecks(pool, v3_parent, {}, TRUC_MAX_VSIZE + 1);
    BOOST_REQUIRE(too_big.has_value());
    BOOST_CHECK(too_big->second == nullptr);
    BOOST_CHECK(too_big->first.find("is too big") != std::string::npos);

    // Add v3_parent to mempool.
    pool.addUnchecked(MakeEntry(v3_parent));

    // 2. Non-v3 (v4) tx cannot spend unconfirmed v3 parent.
    auto v4_child = MakeTx(CTransaction::CURRENT_VERSION, {COutPoint(v3_parent->GetHash(), 0)});
    auto v4_from_v3 = SingleTRUCChecks(pool, v4_child, {}, 200);
    BOOST_REQUIRE(v4_from_v3.has_value());
    BOOST_CHECK(v4_from_v3->second == nullptr);
    BOOST_CHECK(v4_from_v3->first.find("non-version=3") != std::string::npos);

    // 3. v3 child spending v3_parent: allowed up to TRUC_CHILD_MAX_VSIZE, rejected above.
    auto v3_child1 = MakeTx(TRUC_VERSION, {COutPoint(v3_parent->GetHash(), 0)});
    BOOST_CHECK(!SingleTRUCChecks(pool, v3_child1, {}, TRUC_CHILD_MAX_VSIZE).has_value());
    auto child_too_big = SingleTRUCChecks(pool, v3_child1, {}, TRUC_CHILD_MAX_VSIZE + 1);
    BOOST_REQUIRE(child_too_big.has_value());
    BOOST_CHECK(child_too_big->second == nullptr);
    BOOST_CHECK(child_too_big->first.find("child tx") != std::string::npos);

    // Add v3_child1 to mempool.
    pool.addUnchecked(MakeEntry(v3_child1));

    // 4. Grandchild of v3_parent (ancestor count = 3 > 2): rejected without sibling eviction.
    auto v3_grandchild = MakeTx(TRUC_VERSION, {COutPoint(v3_child1->GetHash(), 0)});
    auto gc_res = SingleTRUCChecks(pool, v3_grandchild, {}, 200);
    BOOST_REQUIRE(gc_res.has_value());
    BOOST_CHECK(gc_res->second == nullptr);
    BOOST_CHECK(gc_res->first.find("too many ancestors") != std::string::npos);

    // 5. Second child of v3_parent (sibling): returns v3_child1 as sibling eviction candidate.
    auto v3_child2 = MakeTx(TRUC_VERSION, {COutPoint(v3_parent->GetHash(), 1)});
    auto sib_res = SingleTRUCChecks(pool, v3_child2, {}, 200);
    BOOST_REQUIRE(sib_res.has_value());
    BOOST_REQUIRE(sib_res->second != nullptr);
    BOOST_CHECK_EQUAL(sib_res->second->GetHash(), v3_child1->GetHash());

    // 6. Direct conflict with v3_child1 is allowed (normal replacement, not sibling eviction).
    BOOST_CHECK(!SingleTRUCChecks(pool, v3_child2, {v3_child1->GetHash()}, 200).has_value());

    // 7. v3 tx cannot spend unconfirmed non-v3 (v4) parent.
    auto v4_parent = MakeTx(CTransaction::CURRENT_VERSION, {confirmed_b});
    pool.addUnchecked(MakeEntry(v4_parent));
    auto v3_from_v4 = MakeTx(TRUC_VERSION, {COutPoint(v4_parent->GetHash(), 0)});
    auto mix_res = SingleTRUCChecks(pool, v3_from_v4, {}, 200);
    BOOST_REQUIRE(mix_res.has_value());
    BOOST_CHECK(mix_res->second == nullptr);
    BOOST_CHECK(mix_res->first.find("cannot spend from non-version=3") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(package_truc_checks)
{
    CTxMemPool pool;
    LOCK(pool.cs);

    const COutPoint confirmed_a(uint256S("cc"), 0);

    // 1. Valid 1P1C v3 package.
    auto v3_parent = MakeTx(TRUC_VERSION, {confirmed_a}, 2);
    auto v3_child = MakeTx(TRUC_VERSION, {COutPoint(v3_parent->GetHash(), 0)});
    Package valid_pkg{v3_parent, v3_child};
    BOOST_CHECK(!PackageTRUCChecks(pool, v3_parent, 500, valid_pkg).has_value());
    BOOST_CHECK(!PackageTRUCChecks(pool, v3_child, TRUC_CHILD_MAX_VSIZE, valid_pkg).has_value());

    // 2. Child exceeding TRUC_CHILD_MAX_VSIZE in package is rejected.
    auto big_child_err = PackageTRUCChecks(pool, v3_child, TRUC_CHILD_MAX_VSIZE + 1, valid_pkg);
    BOOST_REQUIRE(big_child_err.has_value());
    BOOST_CHECK(big_child_err->find("is too big") != std::string::npos);

    // 3. v3 parent + v4 child in package is rejected.
    auto v4_child = MakeTx(CTransaction::CURRENT_VERSION, {COutPoint(v3_parent->GetHash(), 0)});
    Package mixed_pkg{v3_parent, v4_child};
    auto mixed_err = PackageTRUCChecks(pool, v4_child, 200, mixed_pkg);
    BOOST_REQUIRE(mixed_err.has_value());
    BOOST_CHECK(mixed_err->find("non-version=3") != std::string::npos);

    // 4. 3-tx v3 chain (parent -> child -> grandchild) in package is rejected.
    auto v3_grandchild = MakeTx(TRUC_VERSION, {COutPoint(v3_child->GetHash(), 0)});
    Package chain_pkg{v3_parent, v3_child, v3_grandchild};
    auto chain_err = PackageTRUCChecks(pool, v3_child, 200, chain_pkg);
    BOOST_REQUIRE(chain_err.has_value());
    BOOST_CHECK(chain_err->find("too many ancestors") != std::string::npos);

    // 5. Two v3 siblings in same package are rejected.
    auto v3_sibling = MakeTx(TRUC_VERSION, {COutPoint(v3_parent->GetHash(), 1)});
    Package sib_pkg{v3_parent, v3_child, v3_sibling};
    auto sib_err = PackageTRUCChecks(pool, v3_child, 200, sib_pkg);
    BOOST_REQUIRE(sib_err.has_value());
    BOOST_CHECK(sib_err->find("exceed descendant count limit") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
