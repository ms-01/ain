// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dfi/res.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <type_traits>

// Regression guards for the `Res` default state.
//
// Up to C++17 `Res` was an aggregate (its default constructor was deleted but
// not user provided), therefore `Res res{};` performed aggregate
// initialisation and yielded `ok == false`, an empty `msg`, `code == 0` and an
// empty `dbgMsg`. C++20 dropped aggregates with user declared constructors, so
// `Res` had to gain an explicit default constructor. The observable state of a
// default constructed `Res` must not change, because `Res` is used on
// consensus critical code paths.
//
// The invariants pinned below fail to compile if the shape of `Res` changes in
// a way that breaks the call sites (e.g. the `Res{ok, msg, code, dbgMsg}`
// factories) or if it silently becomes default constructible into a
// successful state.

BOOST_AUTO_TEST_SUITE(res_tests)

static_assert(std::is_default_constructible_v<Res>,
              "Res must stay default constructible");
static_assert(std::is_constructible_v<Res, bool, std::string, uint32_t, std::string>,
              "Res must stay constructible from (ok, msg, code, dbgMsg)");
static_assert(!std::is_default_constructible_v<ResVal<int>>,
              "ResVal must not be default constructible");

BOOST_AUTO_TEST_CASE(res_default_state)
{
    Res res{};

    BOOST_CHECK(!res.ok);
    BOOST_CHECK_EQUAL(res.msg, "");
    BOOST_CHECK_EQUAL(res.code, 0U);
    BOOST_CHECK_EQUAL(res.dbgMsg, "");
    BOOST_CHECK(!static_cast<bool>(res));
}

BOOST_AUTO_TEST_CASE(res_explicit_state)
{
    Res res{true, "all good", 0, {}};

    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(res.msg, "all good");
    BOOST_CHECK_EQUAL(res.code, 0U);
    BOOST_CHECK_EQUAL(res.dbgMsg, "");
    BOOST_CHECK(static_cast<bool>(res));
}

BOOST_AUTO_TEST_CASE(res_factories)
{
    const auto ok = Res::Ok();
    BOOST_CHECK(ok.ok);
    BOOST_CHECK_EQUAL(ok.msg, "");
    BOOST_CHECK_EQUAL(ok.code, 0U);
    BOOST_CHECK_EQUAL(ok.dbgMsg, "");

    const auto err = Res::Err("some error %d", 42);
    BOOST_CHECK(!err.ok);
    BOOST_CHECK_EQUAL(err.msg, "some error 42");
    BOOST_CHECK_EQUAL(err.code, 0U);

    const auto errCode = Res::ErrCode(NotEnoughBalance, "no balance");
    BOOST_CHECK(!errCode.ok);
    BOOST_CHECK_EQUAL(errCode.msg, "no balance");
    BOOST_CHECK_EQUAL(errCode.code, NotEnoughBalance);

    const auto errDbg = Res::ErrDbg("debug info", "some error");
    BOOST_CHECK(!errDbg.ok);
    BOOST_CHECK_EQUAL(errDbg.msg, "some error");
    BOOST_CHECK_EQUAL(errDbg.dbgMsg, "debug info");
}

BOOST_AUTO_TEST_SUITE_END()
