// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <dfi/customtx.h>
#include <dfi/mn_checks.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/setup_common.h>
#include <validation.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(assumevalid_tests, TestChain100Setup)

/**
 * The assumed-valid block is a chain parameter and it must be disabled
 * (null) by default on every network.
 */
BOOST_AUTO_TEST_CASE(chainparams_assumed_valid_disabled_by_default)
{
    const std::vector<std::string> networks{
        CBaseChainParams::MAIN,
        CBaseChainParams::TESTNET,
        CBaseChainParams::CHANGI,
        CBaseChainParams::DEVNET,
        CBaseChainParams::REGTEST,
    };
    for (const auto &network : networks) {
        const auto params = CreateChainParams(network);
        BOOST_REQUIRE(params != nullptr);
        BOOST_CHECK(params->GetConsensus().defaultAssumeValid.IsNull());
    }
}

/**
 * The effective assumed-valid block is chainstate scoped. It defaults to the
 * chainparams value and can be overridden explicitly.
 */
BOOST_AUTO_TEST_CASE(chainstate_assumed_valid_accessor)
{
    auto &chainstate = ::ChainstateActive();

    // The chainparams default is the fallback of the chainstate.
    BOOST_CHECK(chainstate.AssumedValidBlock() == Params().GetConsensus().defaultAssumeValid);
    BOOST_CHECK(chainstate.AssumedValidBlock().IsNull());

    // The value can be overridden (this is what the -assumevalid option does).
    const uint256 assumed = uint256S("0x0000000000000000000000000000000000000000000000000000000000000042");
    chainstate.SetAssumedValidBlock(assumed);
    BOOST_CHECK(chainstate.AssumedValidBlock() == assumed);

    // And it can be disabled again.
    chainstate.SetAssumedValidBlock(uint256());
    BOOST_CHECK(chainstate.AssumedValidBlock().IsNull());
}

/**
 * Build a synthetic chain of block indexes. Every block has the regtest
 * difficulty, hence the chain work grows by GetBlockProof() per block and the
 * "equivalent time" of a block grows by the target spacing (10 minutes) per
 * buried block.
 */
static std::deque<CBlockIndex> BuildIndexChain(int numBlocks)
{
    std::deque<CBlockIndex> chain;
    for (int i = 0; i < numBlocks; ++i) {
        CBlockIndex index;
        index.nHeight = i;
        index.nBits = 0x207fffff;
        index.nTime = 1579045065 + i * 600;
        index.nChainWork = arith_uint256(i + 1) * GetBlockProof(index);
        index.pprev = (i > 0) ? &chain[i - 1] : nullptr;
        chain.push_back(index);
    }
    return chain;
}

/**
 * The decision whether the script verification may be skipped follows a fixed
 * table:
 *  - the block has to be an ancestor of the assumed-valid block,
 *  - the block has to be an ancestor of the best header,
 *  - the best header has to have at least the minimum chain work,
 *  - and the block has to be buried for at least two weeks.
 */
BOOST_AUTO_TEST_CASE(script_check_skip_decision)
{
    const Consensus::Params &params = Params().GetConsensus();
    const arith_uint256 minimumChainWork = arith_uint256(0);

    // Two forks of the same chain, used to build blocks that are not part of
    // the assumed-valid chain resp. not part of the best header chain.
    auto chain = BuildIndexChain(5000);
    auto fork = BuildIndexChain(5000);

    const CBlockIndex *assumed = &chain[100];
    const CBlockIndex *bestHeader = &chain[4999];
    std::string reason;

    // The assumed-valid block itself and all of its ancestors are inside the
    // range (the block is buried, the chain work is sufficient).
    BOOST_CHECK(CanSkipScriptChecks(assumed, assumed, bestHeader, minimumChainWork, params, reason));
    BOOST_CHECK(CanSkipScriptChecks(assumed, &chain[50], bestHeader, minimumChainWork, params, reason));

    // A block that is not buried long enough is always fully verified.
    BOOST_CHECK(!CanSkipScriptChecks(&chain[4998], &chain[4998], bestHeader, minimumChainWork, params, reason));
    BOOST_CHECK_EQUAL(reason, "block too recent relative to best header");

    // A block above the assumed-valid block is always fully verified.
    BOOST_CHECK(!CanSkipScriptChecks(assumed, &chain[101], bestHeader, minimumChainWork, params, reason));
    BOOST_CHECK_EQUAL(reason, "block height above assumevalid height");

    // A block that is not an ancestor of the assumed-valid block is always
    // fully verified.
    BOOST_CHECK(!CanSkipScriptChecks(assumed, &fork[50], bestHeader, minimumChainWork, params, reason));
    BOOST_CHECK_EQUAL(reason, "block not in assumevalid chain");

    // A block that is not an ancestor of the best header is always fully
    // verified.
    BOOST_CHECK(!CanSkipScriptChecks(assumed, &chain[50], &fork[4999], minimumChainWork, params, reason));
    BOOST_CHECK_EQUAL(reason, "block not in best header chain");

    // A best header with not enough chain work disables the relaxation.
    BOOST_CHECK(!CanSkipScriptChecks(assumed, &chain[50], bestHeader, arith_uint256(1) << 200, params, reason));
    BOOST_CHECK_EQUAL(reason, "best header chainwork below minimumchainwork");

    // An unknown assumed-valid block disables the relaxation.
    BOOST_CHECK(!CanSkipScriptChecks(nullptr, &chain[50], bestHeader, minimumChainWork, params, reason));
    BOOST_CHECK_EQUAL(reason, "assumevalid hash not in headers");
}

/**
 * Build a block on top of the given parent block.
 */
static CBlock BuildBlock(const uint256 &hashPrevBlock,
                         int height,
                         uint32_t nBits,
                         int64_t nTime,
                         const std::vector<CMutableTransaction> &txs)
{
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = hashPrevBlock;
    block.nTime = nTime;
    block.nBits = nBits;

    CMutableTransaction coinbase;
    coinbase.vin.push_back(CTxIn(COutPoint(uint256(), 0xFFFFFFFF), CScript() << height, 0xFFFFFFFF));
    coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));
    block.vtx.push_back(MakeTransactionRef(coinbase));
    for (const auto &tx : txs) {
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

/**
 * A transaction with an empty script signature, its signature is invalid.
 */
static CMutableTransaction InvalidSignatureTx(const uint256 &coinbaseHash)
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    // Spend the (matured) coinbase of the first block of the test chain.
    tx.vin.push_back(CTxIn(COutPoint(coinbaseHash, 0)));
    tx.vout.push_back(CTxOut(0, CScript() << OP_TRUE));
    return tx;
}

/**
 * A transaction with an invalid DeFiChain custom transaction message. The
 * custom transaction is detected by its metadata, an UpdateMasternode ('m')
 * message with a truncated payload cannot be parsed.
 */
static CMutableTransaction InvalidCustomTx(const uint256 &coinbaseHash)
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.vin.push_back(CTxIn(COutPoint(coinbaseHash, 0)));
    const std::vector<unsigned char> metadata{'D', 'f', 'T', 'x', 'm', 0x00, 0x00, 0x00};
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << metadata));
    return tx;
}

/**
 * The assumed-valid range only relaxes the script verification (the
 * transaction signature checks). All DeFiChain consensus rules stay active
 * inside the range.
 */
BOOST_AUTO_TEST_CASE(dfi_rules_active_in_assumed_valid_range)
{
    LOCK(cs_main);
    const CBlockIndex *tip = ::ChainActive().Tip();
    BOOST_REQUIRE(tip != nullptr);
    const uint256 coinbaseHash = m_coinbase_txns.front()->GetHash();
    const uint256 coinbaseHash2 = m_coinbase_txns[1]->GetHash();

    // The custom transaction is detected by its metadata.
    {
        std::vector<unsigned char> meta;
        const CTransaction tx{InvalidCustomTx(coinbaseHash)};
        BOOST_CHECK_EQUAL(ToString(GuessCustomTxType(tx, meta)), "UpdateMasternode");
    }

    // A chain of three blocks:
    //  - blockA contains a transaction with an invalid signature,
    //  - blockB contains a transaction with an invalid DeFiChain custom
    //    transaction message,
    //  - blockC is only used to have a best header that is a descendant of
    //    both of them.
    const CBlock blockA = BuildBlock(tip->GetBlockHash(), tip->nHeight + 1, tip->nBits, tip->GetMedianTimePast() + 1, {InvalidSignatureTx(coinbaseHash)});
    const CBlock blockB = BuildBlock(blockA.GetHash(), tip->nHeight + 2, tip->nBits, tip->GetMedianTimePast() + 2, {InvalidCustomTx(coinbaseHash2)});
    const CBlock blockC = BuildBlock(blockB.GetHash(), tip->nHeight + 3, tip->nBits, tip->GetMedianTimePast() + 3, {});

    // Process the headers of all blocks, this makes the best header a
    // descendant of the two blocks below.
    {
        CValidationState state;
        const std::vector<CBlockHeader> headers{blockA.GetBlockHeader(), blockB.GetBlockHeader(), blockC.GetBlockHeader()};
        BOOST_REQUIRE(ProcessNewBlockHeaders(headers, state, Params()));
    }

    auto *indexA = const_cast<CBlockIndex *>(LookupBlockIndex(blockA.GetHash()));
    auto *indexB = const_cast<CBlockIndex *>(LookupBlockIndex(blockB.GetHash()));
    auto *indexC = const_cast<CBlockIndex *>(LookupBlockIndex(blockC.GetHash()));
    BOOST_REQUIRE(indexA != nullptr);
    BOOST_REQUIRE(indexB != nullptr);
    BOOST_REQUIRE(indexC != nullptr);

    // Simulate that the blocks are buried for more than two weeks by
    // increasing the chain work of the best header.
    indexC->nChainWork = indexC->nChainWork + GetBlockProof(*indexC) * 3000;

    // Both blocks are inside the assumed-valid range, hence their script
    // verification is skipped.
    std::string reason;
    BOOST_CHECK(CanSkipScriptChecks(indexB, indexA, pindexBestHeader, nMinimumChainWork, Params().GetConsensus(), reason));
    BOOST_CHECK(CanSkipScriptChecks(indexB, indexB, pindexBestHeader, nMinimumChainWork, Params().GetConsensus(), reason));

    // Configure the chainstate to assume the blocks (and their ancestors) to
    // be valid.
    ::ChainstateActive().SetAssumedValidBlock(blockB.GetHash());

    // Connect the block with the invalid signature. The script verification is
    // skipped because the block is inside the assumed-valid range, hence the
    // block is accepted.
    CCoinsViewCache view(&::ChainstateActive().CoinsTip());
    CCustomCSView mnview(*pcustomcsview);
    {
        CValidationState state;
        bool rewardedAnchors = false;
        BOOST_CHECK(::ChainstateActive().ConnectBlock(blockA, state, indexA, view, mnview, Params(), rewardedAnchors, false));
    }

    // Connect the block with the invalid DeFiChain custom transaction. Even
    // though the script verification is skipped, the DeFiChain consensus rules
    // are enforced and the block is rejected.
    {
        CValidationState state;
        bool rewardedAnchors = false;
        BOOST_CHECK(!::ChainstateActive().ConnectBlock(blockB, state, indexB, view, mnview, Params(), rewardedAnchors, false));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-updatemasternode-message");
    }

    // Without the assumed-valid range the block with the invalid signature is
    // rejected because of the failed script verification.
    {
        ::ChainstateActive().SetAssumedValidBlock(uint256());
        CCoinsViewCache viewNoRange(&::ChainstateActive().CoinsTip());
        CCustomCSView mnviewNoRange(*pcustomcsview);
        CValidationState state;
        bool rewardedAnchors = false;
        BOOST_CHECK(!::ChainstateActive().ConnectBlock(blockA, state, indexA, viewNoRange, mnviewNoRange, Params(), rewardedAnchors, false));
    }
}

BOOST_AUTO_TEST_SUITE_END()
