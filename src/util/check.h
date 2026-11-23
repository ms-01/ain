// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_UTIL_CHECK_H
#define DEFI_UTIL_CHECK_H

#include <cassert> // IWYU pragma: export

/**
 * Assert is the identity function. Abort if the value compares equal to zero.
 *
 * Use it for conditions that must never be violated and that cannot be
 * recovered from at runtime.
 */
#define Assert(val) assert(val)

/**
 * Assume is the identity function.
 *
 * - Should be used to run non-fatal checks. In debug builds it behaves like
 *   Assert()/assert() to notify developers and testers about non-fatal errors.
 *   In production it doesn't warn or log anything.
 * - For fatal errors, use Assert().
 */
#define Assume(val) assert(val)

#endif // DEFI_UTIL_CHECK_H
