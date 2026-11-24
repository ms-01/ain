// Copyright (c) 2026-present The DeFiChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <test/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

namespace {

struct ChainBuilder {
    std::vector<std::unique_ptr<CBlockIndex>> nodes;
    std::vector<std::unique_ptr<uint256>> hashes;

    CBlockIndex *Add(CBlockIndex *prev, uint64_t work_delta, uint32_t status, uint8_t tag) {
        auto hash = std::make_unique<uint256>();
        hash->begin()[0] = tag;
        hash->begin()[1] = static_cast<uint8_t>(nodes.size() & 0xff);
        hash->begin()[2] = static_cast<uint8_t>((nodes.size() >> 8) & 0xff);

        auto idx = std::make_unique<CBlockIndex>();
        idx->phashBlock = hash.get();
        idx->pprev = prev;
        idx->nHeight = prev ? prev->nHeight + 1 : 0;
        idx->nChainWork = (prev ? prev->nChainWork : arith_uint256(0)) + arith_uint256(work_delta);
        idx->nStatus = status;
        if (prev) {
            idx->BuildSkip();
        }

        CBlockIndex *raw = idx.get();
        hashes.push_back(std::move(hash));
        nodes.push_back(std::move(idx));
        return raw;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(header_tip_selection_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(anchored_height_beats_forged_chainwork) {
    ChainBuilder b;
    CBlockIndex *genesis = b.Add(nullptr, 1, BLOCK_VALID_SCRIPTS, 0x01);
    CBlockIndex *h1 = b.Add(genesis, 1, BLOCK_VALID_SCRIPTS, 0x02);
    CBlockIndex *h2 = b.Add(h1, 1, BLOCK_VALID_SCRIPTS, 0x03);
    CBlockIndex *h3 = b.Add(h2, 1, BLOCK_VALID_SCRIPTS, 0x04);

    // Forged fork from h1 with vastly higher claimed nChainWork, unvalidated (BLOCK_VALID_TREE only).
    CBlockIndex *forged1 = b.Add(h1, 1000000, BLOCK_VALID_TREE, 0x10);
    CBlockIndex *forged2 = b.Add(forged1, 1000000, BLOCK_VALID_TREE, 0x11);

    BOOST_CHECK_EQUAL(GetAnchoredHeight(h3, h3), 3);
    BOOST_CHECK_EQUAL(GetAnchoredHeight(forged2, h3), 1);

    // Anchored mode: honest chain (anchored=3) beats forged chain (anchored=1) despite 2,000,000x work.
    BOOST_CHECK(!IsBetterHeaderTip(forged2, h3, h3, HeaderTipMode::ANCHORED));
    BOOST_CHECK(IsBetterHeaderTip(h3, forged2, h3, HeaderTipMode::ANCHORED));

    // Legacy chainwork mode: forged chain wins on raw nChainWork.
    BOOST_CHECK(IsBetterHeaderTip(forged2, h3, h3, HeaderTipMode::CHAINWORK));
    BOOST_CHECK(!IsBetterHeaderTip(h3, forged2, h3, HeaderTipMode::CHAINWORK));
}

BOOST_AUTO_TEST_CASE(lazy_evaluation_reflects_newly_validated_ancestor) {
    ChainBuilder b;
    CBlockIndex *genesis = b.Add(nullptr, 1, BLOCK_VALID_SCRIPTS, 0x01);
    CBlockIndex *h1 = b.Add(genesis, 1, BLOCK_VALID_SCRIPTS, 0x02);

    // Insert headers before h2 is validated.
    CBlockIndex *h2 = b.Add(h1, 1, BLOCK_VALID_TREE, 0x03);
    CBlockIndex *h3 = b.Add(h2, 1, BLOCK_VALID_TREE, 0x04);

    // Before h2 validates, h3's anchored height is 1 (h1).
    BOOST_CHECK_EQUAL(GetAnchoredHeight(h3, h1), 1);

    // Once h2 reaches BLOCK_VALID_SCRIPTS, h3 immediately reflects anchored height 2 without updating h3.
    h2->RaiseValidity(BLOCK_VALID_SCRIPTS);
    BOOST_CHECK_EQUAL(GetAnchoredHeight(h3, h2), 2);
}

BOOST_AUTO_TEST_CASE(bounded_walk_and_active_chain_fast_path) {
    ChainBuilder b;
    CBlockIndex *genesis = b.Add(nullptr, 1, BLOCK_VALID_SCRIPTS, 0x01);
    CBlockIndex *h1 = b.Add(genesis, 1, BLOCK_VALID_SCRIPTS, 0x02);
    CBlockIndex *h2 = b.Add(h1, 1, BLOCK_VALID_SCRIPTS, 0x03);

    // Honest unvalidated extension > MAX_ANCHORED_WALK_DEPTH (64) hops ahead of active_tip (h2).
    CBlockIndex *honest = h2;
    for (int i = 0; i < MAX_ANCHORED_WALK_DEPTH + 30; ++i) {
        honest = b.Add(honest, 1, BLOCK_VALID_TREE, 0x20);
    }
    // Active-chain fast path via pskip returns active_tip->nHeight (2).
    BOOST_CHECK_EQUAL(GetAnchoredHeight(honest, h2), 2);

    // Unvalidated side-chain forking from h1 (not extending active_tip h2) > 64 hops deep.
    CBlockIndex *side = h1;
    for (int i = 0; i < MAX_ANCHORED_WALK_DEPTH + 10; ++i) {
        side = b.Add(side, 1000, BLOCK_VALID_TREE, 0x30);
    }
    // Exceeds MAX_ANCHORED_WALK_DEPTH -> returns 0 and cannot beat an anchored tip.
    BOOST_CHECK_EQUAL(GetAnchoredHeight(side, h2), 0);
    BOOST_CHECK(!IsBetterHeaderTip(side, h2, h2, HeaderTipMode::ANCHORED));
}

BOOST_AUTO_TEST_CASE(deterministic_hash_tie_break) {
    ChainBuilder b;
    CBlockIndex *genesis = b.Add(nullptr, 1, BLOCK_VALID_SCRIPTS, 0x01);
    // Two competing headers with identical anchored height (0) and identical nChainWork (10).
    CBlockIndex *cand_a = b.Add(genesis, 10, BLOCK_VALID_TREE, 0x05);
    CBlockIndex *cand_b = b.Add(genesis, 10, BLOCK_VALID_TREE, 0x06);

    const bool a_lt_b = cand_a->GetBlockHash() < cand_b->GetBlockHash();
    BOOST_CHECK_EQUAL(IsBetterHeaderTip(cand_a, cand_b, genesis, HeaderTipMode::ANCHORED), a_lt_b);
    BOOST_CHECK_EQUAL(IsBetterHeaderTip(cand_b, cand_a, genesis, HeaderTipMode::ANCHORED), !a_lt_b);
    BOOST_CHECK_EQUAL(IsBetterHeaderTip(cand_a, cand_b, genesis, HeaderTipMode::CHAINWORK), a_lt_b);
    BOOST_CHECK_EQUAL(IsBetterHeaderTip(cand_b, cand_a, genesis, HeaderTipMode::CHAINWORK), !a_lt_b);
}

BOOST_AUTO_TEST_CASE(failed_chain_drops_to_zero) {
    ChainBuilder b;
    CBlockIndex *genesis = b.Add(nullptr, 1, BLOCK_VALID_SCRIPTS, 0x01);
    CBlockIndex *h1 = b.Add(genesis, 1, BLOCK_VALID_SCRIPTS, 0x02);
    CBlockIndex *h2 = b.Add(h1, 1, BLOCK_VALID_SCRIPTS | BLOCK_FAILED_VALID, 0x03);
    CBlockIndex *h3 = b.Add(h2, 1, BLOCK_VALID_TREE | BLOCK_FAILED_CHILD, 0x04);

    BOOST_CHECK_EQUAL(GetAnchoredHeight(h2, h1), 0);
    BOOST_CHECK_EQUAL(GetAnchoredHeight(h3, h1), 0);
    BOOST_CHECK(IsBetterHeaderTip(h1, h3, h1, HeaderTipMode::ANCHORED));
}

BOOST_AUTO_TEST_CASE(parse_header_tip_mode_cli) {
    HeaderTipMode mode = HeaderTipMode::CHAINWORK;
    BOOST_CHECK(ParseHeaderTipMode("anchored", mode));
    BOOST_CHECK(mode == HeaderTipMode::ANCHORED);

    BOOST_CHECK(ParseHeaderTipMode("chainwork", mode));
    BOOST_CHECK(mode == HeaderTipMode::CHAINWORK);

    BOOST_CHECK(!ParseHeaderTipMode("", mode));
    BOOST_CHECK(!ParseHeaderTipMode("ANCHORED", mode));
    BOOST_CHECK(!ParseHeaderTipMode("invalid", mode));
}

BOOST_AUTO_TEST_SUITE_END()
