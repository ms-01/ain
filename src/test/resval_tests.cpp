// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dfi/res.h>

#include <amount.h>

#include <boost/test/unit_test.hpp>

#include <type_traits>

// Regression guards for the silent `ResVal` -> `bool` value collapse.
//
// `ResVal<T>` used to expose an *implicit* `operator bool()` next to an
// *explicit* `operator T()`. Every assignment of a `ResVal<T>` to an
// arithmetic type therefore resolved to the boolean conversion and stored
// `0`/`1` instead of the payload (e.g. `CPoolPair::AddLiquidity` stored
// `reserveA = 1` instead of `11`), which additionally corrupted the pool
// state enough to make the legacy reward path overflow.
//
// The invariants pinned below fail to compile if the collapse becomes
// possible again.

BOOST_AUTO_TEST_SUITE(resval_tests)

static_assert(!std::is_convertible_v<ResVal<CAmount>, CAmount>,
              "ResVal must not be implicitly convertible to its value type");
static_assert(!std::is_convertible_v<ResVal<CAmount>, bool>,
              "ResVal must not be implicitly convertible to bool");
static_assert(!std::is_convertible_v<ResVal<CAmount>, int>,
              "ResVal must not be implicitly convertible to an arithmetic type");
static_assert(!std::is_assignable_v<CAmount &, ResVal<CAmount>>,
              "ResVal must not be assignable to an arithmetic type");
static_assert(std::is_constructible_v<bool, ResVal<CAmount>>,
              "ResVal must stay explicitly convertible to bool");
static_assert(std::is_constructible_v<CAmount, ResVal<CAmount>>,
              "ResVal must stay explicitly convertible to its value type");

BOOST_AUTO_TEST_CASE(resval_value_roundtrip)
{
    constexpr CAmount expected = 1048;
    ResVal<CAmount> res(expected, Res::Ok());

    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(*res, expected);
    BOOST_CHECK_EQUAL(res.val.value(), expected);

    // explicit conversions stay available for value contexts
    BOOST_CHECK_EQUAL(static_cast<CAmount>(res), expected);
    BOOST_CHECK_EQUAL(static_cast<bool>(res), true);
}

BOOST_AUTO_TEST_CASE(resval_error_has_no_value)
{
    ResVal<CAmount> res(Res::Err("some error"));

    BOOST_CHECK(!res.ok);
    BOOST_CHECK(!res.val.has_value());
    BOOST_CHECK_EQUAL(static_cast<bool>(res), false);
}

BOOST_AUTO_TEST_SUITE_END()
