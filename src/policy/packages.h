// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_POLICY_PACKAGES_H
#define DEFI_POLICY_PACKAGES_H

#include <policy/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

/** Default maximum number of transactions in a package. */
static constexpr uint32_t MAX_PACKAGE_COUNT{25};
/** Default maximum total virtual size of transactions in a package in KvB. */
static constexpr uint32_t MAX_PACKAGE_SIZE{101};

/**
 * A "reason" why a package was invalid. It may be that one or more of the included
 * transactions is invalid or the package itself violates our rules.
 * We don't distinguish between consensus and policy violations right now.
 */
enum class PackageValidationResult {
    PCKG_RESULT_UNSET = 0, //!< Initial value. The package has not yet been rejected.
    PCKG_POLICY,           //!< The package itself is invalid (e.g. too many transactions).
    PCKG_TX,               //!< At least one tx is invalid.
    PCKG_MEMPOOL_ERROR,    //!< Mempool logic error.
};

/** A package is an ordered list of transactions. The transactions cannot conflict with (spend the
 * same inputs as) one another. */
using Package = std::vector<CTransactionRef>;

/**
 * Validation state of a package.
 *
 * The chain does not provide the templated `ValidationState<T>` helper of newer Bitcoin Core
 * releases, therefore a small stand-alone class with the same semantics is used here: a state is
 * either valid (nothing was rejected yet) or invalid, carrying the `PackageValidationResult`
 * kind of the failure and a stable reject reason string.
 */
class PackageValidationState
{
public:
    /** Mark the package as invalid. Always returns false, so it can be used as `return state.Invalid(...)`. */
    bool Invalid(PackageValidationResult result, const std::string &reject_reason)
    {
        m_result = result;
        m_reject_reason = reject_reason;
        return false;
    }

    bool IsValid() const { return m_result == PackageValidationResult::PCKG_RESULT_UNSET; }
    bool IsInvalid() const { return !IsValid(); }
    PackageValidationResult GetResult() const { return m_result; }
    const std::string &GetRejectReason() const { return m_reject_reason; }

private:
    PackageValidationResult m_result{PackageValidationResult::PCKG_RESULT_UNSET};
    std::string m_reject_reason;
};

/**
 * Result of the validation of a single transaction of a package.
 */
struct PackageTxResult {
    /** The txid of the transaction this result belongs to. */
    uint256 txid;
    /** Whether the transaction would be accepted by the mempool. */
    bool allowed{false};
    /** The reject reason, only set when the transaction was not allowed. */
    std::string reject_reason;
};

/**
 * Result of a package validation.
 */
struct PackageMempoolAcceptResult {
    /** The kind of failure. PCKG_RESULT_UNSET if the package was accepted. */
    PackageValidationResult m_result_type{PackageValidationResult::PCKG_RESULT_UNSET};
    /** The package level error, empty if the package itself is valid. */
    std::string m_package_error;
    /** One entry per transaction of the package, in the order of the input transactions. */
    std::vector<PackageTxResult> m_tx_results;

    /** Whether the package itself was accepted (the transactions may still be rejected). */
    bool PackageAccepted() const
    {
        return m_result_type == PackageValidationResult::PCKG_RESULT_UNSET && m_package_error.empty();
    }
};

/**
 * Context-free package policy checks:
 * 1. The number of transactions cannot exceed MAX_PACKAGE_COUNT.
 * 2. The total virtual size cannot exceed MAX_PACKAGE_SIZE.
 * 3. If any dependencies exist between transactions, parents must appear before children.
 * 4. Transactions cannot conflict, i.e., spend the same inputs.
 */
bool CheckPackage(const Package &txns, PackageValidationState &state);

/** Context-free check that a package is exactly one child and its parents; not all parents need to
 * be present, but the package must not contain any transactions that are not the child's parents.
 * It is expected to be sorted, which means the last transaction must be the child.
 */
bool IsChildWithParents(const Package &package);

#endif // DEFI_POLICY_PACKAGES_H
