// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/packages.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(package_tests, BasicTestingSetup)

static CTransactionRef MakePackageTx(const std::vector<COutPoint> &inputs,
                                      const std::vector<CAmount> &output_values,
                                      size_t script_size = 0)
{
    CMutableTransaction tx = CMutableTransaction();
    tx.vin.resize(inputs.size());
    tx.vout.resize(output_values.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        tx.vin[i].prevout = inputs[i];
        tx.vin[i].scriptSig = CScript() << OP_11 << OP_EQUAL;
    }
    for (size_t i = 0; i < output_values.size(); ++i) {
        tx.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        if (script_size > 0) {
            // A big script, to make the transaction big.
            CScript big_script;
            big_script << std::vector<unsigned char>(script_size, 'a') << OP_11 << OP_EQUAL;
            tx.vout[i].scriptPubKey = big_script;
        }
        tx.vout[i].nValue = output_values[i];
    }
    return MakeTransactionRef(tx);
}

static COutPoint OutPointOf(const CTransactionRef &tx, uint32_t n = 0)
{
    return COutPoint(tx->GetHash(), n);
}

BOOST_AUTO_TEST_CASE(check_package_empty)
{
    Package package;
    PackageValidationState state;
    BOOST_CHECK(CheckPackage(package, state));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_RESULT_UNSET);
}

BOOST_AUTO_TEST_CASE(check_package_single_tx)
{
    Package package{MakePackageTx({}, {1000})};
    PackageValidationState state;
    BOOST_CHECK(CheckPackage(package, state));
    BOOST_CHECK(state.IsValid());
}

BOOST_AUTO_TEST_CASE(check_package_too_many_transactions)
{
    Package package;
    for (size_t i = 0; i < MAX_PACKAGE_COUNT + 1; ++i) {
        package.push_back(MakePackageTx({}, {1000}));
    }
    PackageValidationState state;
    BOOST_CHECK(!CheckPackage(package, state));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "package-too-many-transactions");
}

BOOST_AUTO_TEST_CASE(check_package_too_large)
{
    // A single transaction is only limited by the standard transaction size, not by the
    // package size limit.
    Package single{MakePackageTx({}, {1000}, MAX_PACKAGE_SIZE * 1000)};
    PackageValidationState single_state;
    BOOST_CHECK(CheckPackage(single, single_state));

    // A package of two big transactions exceeds the package size limit.
    Package package;
    for (size_t i = 0; i < 2; ++i) {
        package.push_back(MakePackageTx({}, {1000}, MAX_PACKAGE_SIZE * 1000));
    }
    PackageValidationState state;
    BOOST_CHECK(!CheckPackage(package, state));
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "package-too-large");
}

BOOST_AUTO_TEST_CASE(check_package_not_sorted)
{
    const CTransactionRef parent = MakePackageTx({}, {1000});
    const CTransactionRef child = MakePackageTx({OutPointOf(parent)}, {900});

    // The child before its parent is not sorted.
    Package package{child, parent};
    PackageValidationState state;
    BOOST_CHECK(!CheckPackage(package, state));
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "package-not-sorted");

    // The parent before its child is fine.
    Package sorted{parent, child};
    PackageValidationState sorted_state;
    BOOST_CHECK(CheckPackage(sorted, sorted_state));
}

BOOST_AUTO_TEST_CASE(check_package_conflict)
{
    const CTransactionRef parent = MakePackageTx({}, {1000});
    const COutPoint outpoint = OutPointOf(parent);

    // Two transactions of the package spend the same input.
    Package package{MakePackageTx({outpoint}, {900}), MakePackageTx({outpoint}, {800})};
    PackageValidationState state;
    BOOST_CHECK(!CheckPackage(package, state));
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "conflict-in-package");
}

BOOST_AUTO_TEST_CASE(check_package_duplicate_tx)
{
    const CTransactionRef tx = MakePackageTx({COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000001"), 0)}, {1000});
    Package package{tx, tx};
    PackageValidationState state;
    BOOST_CHECK(!CheckPackage(package, state));
    BOOST_CHECK(state.GetResult() == PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "conflict-in-package");
}

BOOST_AUTO_TEST_CASE(is_child_with_parents)
{
    // The transactions have to be different, otherwise they would have the same txid.
    const CTransactionRef parent = MakePackageTx({}, {1000});
    const CTransactionRef other_parent = MakePackageTx({}, {2000});
    const CTransactionRef child = MakePackageTx({OutPointOf(parent), OutPointOf(other_parent)}, {900});
    const CTransactionRef unrelated = MakePackageTx({}, {3000});

    // A package of one transaction is never a child with parents.
    BOOST_CHECK(!IsChildWithParents(Package{child}));

    // The parents of the child.
    BOOST_CHECK(IsChildWithParents(Package{parent, other_parent, child}));

    // A transaction that is not a parent of the child.
    BOOST_CHECK(!IsChildWithParents(Package{parent, unrelated, child}));
}

BOOST_AUTO_TEST_SUITE_END()
