// Copyright (c) 2014-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <init.h>
#include <key.h>
#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <txdb.h>
#include <util/system.h>
#include <validation.h>
#include <validationinterface.h>

#include <test/setup_common.h>

#include <boost/signals2/signal.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

static void TestBlockSubsidyHalvings(const Consensus::Params& consensusParams)
{
    /// OFF due to disabled halvings
    return;

    int maxHalvings = 64;
    CAmount nInitialSubsidy = consensusParams.baseBlockSubsidy;

    CAmount nPreviousSubsidy = nInitialSubsidy * 2; // for height == 0
    BOOST_CHECK_EQUAL(nPreviousSubsidy, nInitialSubsidy * 2);
    for (int nHalvings = 0; nHalvings < maxHalvings; nHalvings++) {
        int nHeight = nHalvings * consensusParams.nSubsidyHalvingInterval;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_CHECK(nSubsidy <= nInitialSubsidy);
        BOOST_CHECK_EQUAL(nSubsidy, nPreviousSubsidy / 2);
        nPreviousSubsidy = nSubsidy;
    }
    BOOST_CHECK_EQUAL(GetBlockSubsidy(maxHalvings * consensusParams.nSubsidyHalvingInterval, consensusParams), 0);
}

static void TestBlockSubsidyHalvings(int nSubsidyHalvingInterval, CAmount baseBlockSubsidy)
{
    Consensus::Params consensusParams;
    consensusParams.nSubsidyHalvingInterval = nSubsidyHalvingInterval;
    consensusParams.baseBlockSubsidy = baseBlockSubsidy;
    TestBlockSubsidyHalvings(consensusParams);
}

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
    TestBlockSubsidyHalvings(chainParams->GetConsensus()); // As in main
    TestBlockSubsidyHalvings(150, 50*COIN); // As in regtest
    TestBlockSubsidyHalvings(1000, 50*COIN); // Just another interval
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    /// OFF due to disabled halvings
//    const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
//    CAmount nSum = 0;
//    for (int nHeight = 0; nHeight < 14000000; nHeight += 1000) {
//        CAmount nSubsidy = GetBlockSubsidy(nHeight, chainParams->GetConsensus());
//        BOOST_CHECK(nSubsidy <= chainParams->GetConsensus().baseBlockSubsidy);
//        nSum += nSubsidy * 1000;
//        BOOST_CHECK(MoneyRange(nSum));
//    }
//    BOOST_CHECK_EQUAL(nSum, CAmount{280000000000000000});
//    BOOST_CHECK_EQUAL(nSum, CAmount{8399999997690000});
//    BOOST_CHECK_EQUAL(nSum, CAmount{2099999997690000});
}

static bool ReturnFalse() { return false; }
static bool ReturnTrue() { return true; }

BOOST_AUTO_TEST_CASE(test_combiner_all)
{
    boost::signals2::signal<bool (), CombinerAll> Test;
    BOOST_CHECK(Test());
    Test.connect(&ReturnFalse);
    BOOST_CHECK(!Test());
    Test.connect(&ReturnTrue);
    BOOST_CHECK(!Test());
    Test.disconnect(&ReturnFalse);
    BOOST_CHECK(Test());
    Test.disconnect(&ReturnTrue);
    BOOST_CHECK(Test());
}

BOOST_AUTO_TEST_CASE(cache_sizes_budget_and_scaling)
{
    const auto savedDbCache = gArgs.GetArg("-dbcache", "");
    const size_t savedCoinCache = nCoinCacheUsage;
    const size_t savedCustomMem = nCustomMemUsage;

    auto runWithDbCache = [](const std::string& mb) {
        gArgs.ForceSetArg("-dbcache", mb);
        CacheSizes sizes{};
        SetupCacheSizes(sizes);
        return sizes;
    };

    // Minimum -dbcache=4: no negative budgets or underflow
    CacheSizes minSizes = runWithDbCache("4");
    BOOST_CHECK_GT(minSizes.customCacheSize, 0);
    BOOST_CHECK_GT(nCustomMemUsage, 0U);
    BOOST_CHECK_GT(nCoinCacheUsage, 0U);

    // Default -dbcache=512: nCustomMemUsage is ~25% of remaining in-memory cache (not 1/256)
    CacheSizes defSizes = runWithDbCache("512");
    BOOST_CHECK_GT(nCustomMemUsage, static_cast<size_t>(50) << 20);
    BOOST_CHECK_GT(nCoinCacheUsage, nCustomMemUsage);
    BOOST_CHECK_LT(defSizes.customCacheSize * 5, 512LL << 20);

    // Large -dbcache=4096: nCustomMemUsage and customCacheSize scale proportionally
    CacheSizes largeSizes = runWithDbCache("4096");
    BOOST_CHECK_GT(nCustomMemUsage, static_cast<size_t>(500) << 20);
    BOOST_CHECK_GE(largeSizes.customCacheSize, defSizes.customCacheSize);
    BOOST_CHECK_LE(largeSizes.customCacheSize * 5, nMaxCoinsDBCache << 20);
    BOOST_CHECK_LE(largeSizes.customCacheSize * 5 + largeSizes.coinDBCache +
                       largeSizes.blockTreeDBCache + largeSizes.txIndexCache +
                       static_cast<int64_t>(nCustomMemUsage + nCoinCacheUsage),
                   4096LL << 20);

    gArgs.ForceSetArg("-dbcache", savedDbCache.empty() ? std::to_string(nDefaultDbCache) : savedDbCache);
    nCoinCacheUsage = savedCoinCache;
    nCustomMemUsage = savedCustomMem;
}

//! Build a minimal block with a single (valid) coinbase transaction.
static CBlock MakeCheckBlockTestBlock()
{
    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = uint256();
    block.nTime = 1231006505;
    block.nBits = 0x207fffff;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    coinbase.vin[0].nSequence = 0xffffffff;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 50 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

    return block;
}

//! Consensus params with the merkle root check enabled for every height > 0.
static Consensus::Params MerkleRootAlwaysCheckedParams()
{
    Consensus::Params params = Params().GetConsensus();
    params.DF8EunosHeight = 1;
    params.DF9EunosKampungHeight = 1;
    return params;
}

static std::vector<uint8_t> SerializeBlockForTest(const CBlock &block)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << block;
    return std::vector<uint8_t>(ss.begin(), ss.end());
}

BOOST_AUTO_TEST_CASE(check_block_is_stateless)
{
    // CheckBlock() must not cache its verdict on the block object: it has to be a pure
    // function of the block contents and the given parameters.
    const Consensus::Params consensus = MerkleRootAlwaysCheckedParams();

    CBlock block = MakeCheckBlockTestBlock();
    block.hashMerkleRoot = BlockMerkleRoot(block);

    const std::vector<uint8_t> serializedBefore = SerializeBlockForTest(block);

    CheckContextState ctxState;
    CValidationState state;
    BOOST_CHECK(CheckBlock(block, state, consensus, ctxState, false, 100, true));
    BOOST_CHECK(state.IsValid());

    // The block must not be modified by the check.
    BOOST_CHECK(serializedBefore == SerializeBlockForTest(block));

    // A repeated check must yield the same verdict (no hidden "already checked" state).
    CheckContextState ctxState2;
    CValidationState state2;
    BOOST_CHECK(CheckBlock(block, state2, consensus, ctxState2, false, 100, true));
    BOOST_CHECK(state2.IsValid());
    BOOST_CHECK(serializedBefore == SerializeBlockForTest(block));

    // A block mutated after a successful check must be rejected by the next check.
    CMutableTransaction mutated(*block.vtx[0]);
    mutated.vout[0].nValue = 49 * COIN;
    block.vtx[0] = MakeTransactionRef(std::move(mutated));

    CheckContextState ctxState3;
    CValidationState state3;
    BOOST_CHECK(!CheckBlock(block, state3, consensus, ctxState3, false, 100, true));
    BOOST_CHECK(state3.IsInvalid());
    BOOST_CHECK_EQUAL(state3.GetRejectReason(), "bad-txnmrklroot");
}

BOOST_AUTO_TEST_CASE(check_block_merkle_window_parameter_sensitivity)
{
    // A block with a deliberately wrong merkle root.
    CBlock block = MakeCheckBlockTestBlock();
    block.hashMerkleRoot = uint256();

    // Inside the [DF8EunosHeight, DF9EunosKampungHeight) window the merkle root check is
    // skipped, so the block is accepted.
    Consensus::Params windowed = Params().GetConsensus();
    windowed.DF8EunosHeight = 1;
    windowed.DF9EunosKampungHeight = 1000;

    // Outside of that window the merkle root check is active for the very same height.
    Consensus::Params active = Params().GetConsensus();
    active.DF8EunosHeight = 1;
    active.DF9EunosKampungHeight = 50;

    const int height = 100;

    CheckContextState ctxState;
    CValidationState state;
    BOOST_CHECK(CheckBlock(block, state, windowed, ctxState, true, height, true));
    BOOST_CHECK(state.IsValid());

    // The same block must be rejected once the merkle root check applies. A cached
    // "already checked" state would wrongly accept it here.
    CheckContextState ctxState2;
    CValidationState state2;
    BOOST_CHECK(!CheckBlock(block, state2, active, ctxState2, true, height, true));
    BOOST_CHECK(state2.IsInvalid());
    BOOST_CHECK_EQUAL(state2.GetRejectReason(), "bad-txnmrklroot");
}

/**
 * The minter key of a header must be recovered on demand: CBlockHeader must not keep the
 * recovered key in a mutable member, because a block is shared between the net thread, the
 * validation thread and the RPC layer via a std::shared_ptr<const CBlock>.
 */
BOOST_AUTO_TEST_CASE(extract_minter_key_is_pure_and_reentrant)
{
    CBlock block = MakeCheckBlockTestBlock();

    CKey key;
    key.MakeNewKey(true);
    BOOST_REQUIRE(key.SignCompact(block.GetHashToSign(), block.sig));
    const CKeyID expectedKeyID = key.GetPubKey().GetID();

    CKeyID recoveredKeyID;
    BOOST_REQUIRE(block.ExtractMinterKey(recoveredKeyID));
    BOOST_CHECK(recoveredKeyID == expectedKeyID);

    // The extraction must not modify the header in any way.
    const std::vector<uint8_t> serializedBefore = SerializeBlockForTest(block);
    CKeyID recoveredKeyID2;
    BOOST_REQUIRE(block.ExtractMinterKey(recoveredKeyID2));
    BOOST_CHECK(recoveredKeyID2 == expectedKeyID);
    BOOST_CHECK(serializedBefore == SerializeBlockForTest(block));

    // Concurrent readers of the very same (shared, immutable) block must all see the same
    // results - there is no shared mutable state that could be raced on.
    std::shared_ptr<const CBlock> sharedBlock = std::make_shared<const CBlock>(block);
    const uint256 expectedHash = sharedBlock->GetHash();

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&sharedBlock, &expectedKeyID, expectedHash, &failures]() {
            for (int j = 0; j < 100; ++j) {
                CKeyID keyID;
                if (!sharedBlock->ExtractMinterKey(keyID) || keyID != expectedKeyID) {
                    ++failures;
                }
                if (sharedBlock->GetHash() != expectedHash) {
                    ++failures;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    BOOST_CHECK_EQUAL(failures.load(), 0);
}

/**
 * The stateless block check must not require cs_main: it is executed outside of the critical
 * section of the block admission path.
 */
BOOST_AUTO_TEST_CASE(check_block_does_not_require_cs_main)
{
    const Consensus::Params consensus = MerkleRootAlwaysCheckedParams();

    CBlock block = MakeCheckBlockTestBlock();
    block.hashMerkleRoot = BlockMerkleRoot(block);

    std::promise<void> done;
    auto future = done.get_future();
    std::thread worker([&block, &consensus, &done]() {
        CheckContextState ctxState;
        CValidationState state;
        CheckBlock(block, state, consensus, ctxState, false, 1, true);
        done.set_value();
    });

    {
        // While cs_main is held by this thread the stateless check has to make progress.
        LOCK(cs_main);
        BOOST_CHECK(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }
    worker.join();
}

/**
 * Collects the reject reason of the last block that failed the stateless check.
 */
class BlockCheckedCatcher : public CValidationInterface
{
public:
    std::string rejectReason;

    void BlockChecked(const CBlock& block, const CValidationState& state) override
    {
        rejectReason = state.GetRejectReason();
    }
};

/**
 * A block with an unknown parent must still be rejected with "prev-blk-not-found".
 */
BOOST_AUTO_TEST_CASE(process_new_block_rejects_unknown_parent)
{
    CBlock block = MakeCheckBlockTestBlock();
    block.hashPrevBlock = uint256S("0x01");

    BlockCheckedCatcher catcher;
    RegisterValidationInterface(&catcher);

    bool newBlock{false};
    const bool processed = ProcessNewBlock(Params(), std::make_shared<CBlock>(block), true, &newBlock);

    UnregisterValidationInterface(&catcher);

    BOOST_CHECK(!processed);
    BOOST_CHECK_EQUAL(catcher.rejectReason, "prev-blk-not-found");
}

/**
 * A block with a manipulated merkle root must still be rejected with "bad-txnmrklroot".
 */
BOOST_AUTO_TEST_CASE(process_new_block_rejects_manipulated_merkle_root)
{
    CBlock block = MakeCheckBlockTestBlock();
    block.hashPrevBlock = Params().GenesisBlock().GetHash();
    block.hashMerkleRoot = uint256();

    BlockCheckedCatcher catcher;
    RegisterValidationInterface(&catcher);

    bool newBlock{false};
    const bool processed = ProcessNewBlock(Params(), std::make_shared<CBlock>(block), true, &newBlock);

    UnregisterValidationInterface(&catcher);

    BOOST_CHECK(!processed);
    BOOST_CHECK_EQUAL(catcher.rejectReason, "bad-txnmrklroot");
}

BOOST_AUTO_TEST_SUITE_END()
