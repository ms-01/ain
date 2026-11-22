// Copyright (c) 2026-present The DeFiChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <sync.h>
#include <validation.h>
#include <test/setup_common.h>

#include <map>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(headers_budget_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(allowance_test)
{
    // Nothing is limited as long as the budgets are not reached.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(0, 0, 100, 1000, 1000), 100);

    // The budget of the peer is the limit.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(900, 0, 200, 1000, 10000), 100);

    // The global budget is the limit.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(0, 950, 200, 10000, 1000), 50);

    // The smaller of both budgets is the limit.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(990, 990, 200, 1000, 1000), 10);

    // An exhausted budget does not allow anything.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(1000, 0, 200, 1000, 1000), 0);
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(0, 1000, 200, 1000, 1000), 0);

    // A negative limit disables the budget.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(1000, 1000, 200, -1, -1), 200);

    // There is nothing to add.
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(0, 0, 0, 1000, 1000), 0);
    BOOST_CHECK_EQUAL(GetUnanchoredHeaderAllowance(0, 0, -1, 1000, 1000), 0);
}

BOOST_AUTO_TEST_CASE(unanchored_header_accounting_test)
{
    LOCK(cs_main);

    BlockManager blockman;

    // A chain of four headers. The first one is validated already, the headers
    // above it are not.
    std::vector<CBlockIndex> vIndex(4);
    for (int i = 0; i < 4; ++i) {
        vIndex[i].nHeight = i + 1;
        vIndex[i].pprev = (i == 0) ? nullptr : &vIndex[i - 1];
    }
    vIndex[0].nStatus |= BLOCK_VALID_SCRIPTS;

    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 0);

    // Three headers of the chain are side chain material of two different peers.
    blockman.NoteUnanchoredHeader(&vIndex[1], 7);
    blockman.NoteUnanchoredHeader(&vIndex[2], 7);
    blockman.NoteUnanchoredHeader(&vIndex[3], 9);
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 3);

    // A header is counted only once.
    blockman.NoteUnanchoredHeader(&vIndex[1], 7);
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 3);

    // The chain becomes anchored: the accounting of the headers of the chain is
    // given back to the peers that introduced them.
    const std::map<int64_t, int64_t> unanchored = blockman.UnanchorHeaders(&vIndex[3]);
    BOOST_CHECK_EQUAL(unanchored.size(), 2);
    BOOST_CHECK_EQUAL(unanchored.at(7), 2);
    BOOST_CHECK_EQUAL(unanchored.at(9), 1);
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 0);

    // The accounting of the headers is given back only once.
    const std::map<int64_t, int64_t> unanchored_again = blockman.UnanchorHeaders(&vIndex[3]);
    BOOST_CHECK(unanchored_again.empty());
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 0);
}

BOOST_AUTO_TEST_CASE(unanchored_header_accounting_stops_at_validated_block_test)
{
    LOCK(cs_main);

    BlockManager blockman;

    // A chain of four headers. The second one is validated already, so the
    // headers below it are anchored and must not be given back.
    std::vector<CBlockIndex> vIndex(4);
    for (int i = 0; i < 4; ++i) {
        vIndex[i].nHeight = i + 1;
        vIndex[i].pprev = (i == 0) ? nullptr : &vIndex[i - 1];
    }
    vIndex[1].nStatus |= BLOCK_VALID_SCRIPTS;

    blockman.NoteUnanchoredHeader(&vIndex[0], 3);
    blockman.NoteUnanchoredHeader(&vIndex[1], 3);
    blockman.NoteUnanchoredHeader(&vIndex[2], 3);
    blockman.NoteUnanchoredHeader(&vIndex[3], 3);
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 4);

    // The headers above the validated block are given back, the header below it
    // stays accounted for.
    const std::map<int64_t, int64_t> unanchored = blockman.UnanchorHeaders(&vIndex[3]);
    BOOST_CHECK_EQUAL(unanchored.size(), 1);
    BOOST_CHECK_EQUAL(unanchored.at(3), 2);
    BOOST_CHECK_EQUAL(blockman.GetUnanchoredHeaderCount(), 2);
}

BOOST_AUTO_TEST_SUITE_END()
