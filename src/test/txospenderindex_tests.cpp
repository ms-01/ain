// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <index/txospenderindex.h>
#include <primitives/transaction.h>
#include <script/sign.h>
#include <script/script.h>
#include <test/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <thread>

BOOST_AUTO_TEST_SUITE(txospenderindex_tests)

BOOST_FIXTURE_TEST_CASE(txospenderindex_initial_sync, TestChain100Setup)
{
    const uint256 masternodeID = testMasternodeKeys.begin()->first;
    const CScript& coinbase_script = m_coinbase_txns[0]->vout[0].scriptPubKey;

    // Mine one more block, so that the first coinbase output is mature and can
    // be spent.
    CreateAndProcessBlock({}, coinbase_script, masternodeID);

    // Create a transaction that spends the first coinbase output.
    CMutableTransaction spend;
    spend.nVersion = m_coinbase_txns[0]->nVersion;
    spend.vin.resize(1);
    spend.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spend.vout.resize(1);
    spend.vout[0] = CTxOut(m_coinbase_txns[0]->vout[0].nValue, coinbase_script);

    // Sign the input (the coinbase script is a pay-to-public-key script).
    std::vector<unsigned char> vchSig;
    const uint256 hash = SignatureHash(coinbase_script, spend, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    BOOST_REQUIRE(coinbaseKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    spend.vin[0].scriptSig << vchSig;

    // Include the spending transaction in a block.
    const CBlock block = CreateAndProcessBlock({spend}, coinbase_script, masternodeID);
    const uint256 tip_hash = block.GetHash();
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2UL);

    // Start the index and wait until it caught up with the chain.
    TxoSpenderIndex txospenderindex(1 << 20, true);
    BOOST_CHECK(!txospenderindex.BlockUntilSyncedToCurrentChain());
    txospenderindex.Start();

    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txospenderindex.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // The spending transaction is found and the loaded transaction is the
    // expected one (this guards against wrong in-block offsets, e.g. because of
    // the token aware serialization of DeFiChain).
    const COutPoint spent_outpoint{m_coinbase_txns[0]->GetHash(), 0};
    const std::optional<TxoSpender> spender{txospenderindex.FindSpender(spent_outpoint)};
    BOOST_REQUIRE(spender);
    BOOST_CHECK_EQUAL(spender->tx->GetHash(), block.vtx[1]->GetHash());
    BOOST_CHECK_EQUAL(spender->tx->GetHash(), spend.GetHash());
    BOOST_CHECK_EQUAL(spender->block_hash, tip_hash);

    // An output that has not been spent is not found in the index.
    const COutPoint unspent_outpoint{m_coinbase_txns[1]->GetHash(), 0};
    BOOST_CHECK(!txospenderindex.FindSpender(unspent_outpoint).has_value());

    // An output that does not exist at all is not found either.
    const COutPoint unknown_outpoint{uint256S("0000000000000000000000000000000000000000000000000000000000000001"), 0};
    BOOST_CHECK(!txospenderindex.FindSpender(unknown_outpoint).has_value());

    // shutdown sequence (c.f. Shutdown() in init.cpp)
    txospenderindex.Stop();
}

BOOST_FIXTURE_TEST_CASE(txospenderindex_disconnect, TestChain100Setup)
{
    const uint256 masternodeID = testMasternodeKeys.begin()->first;
    const CScript& coinbase_script = m_coinbase_txns[0]->vout[0].scriptPubKey;

    // Mine one more block, so that the first coinbase output is mature and can
    // be spent.
    CreateAndProcessBlock({}, coinbase_script, masternodeID);

    // Create a transaction that spends the first coinbase output.
    CMutableTransaction spend;
    spend.nVersion = m_coinbase_txns[0]->nVersion;
    spend.vin.resize(1);
    spend.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spend.vout.resize(1);
    spend.vout[0] = CTxOut(m_coinbase_txns[0]->vout[0].nValue, coinbase_script);

    // Sign the input (the coinbase script is a pay-to-public-key script).
    std::vector<unsigned char> vchSig;
    const uint256 hash = SignatureHash(coinbase_script, spend, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    BOOST_REQUIRE(coinbaseKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    spend.vin[0].scriptSig << vchSig;

    // Include the spending transaction in a block.
    const CBlock block = CreateAndProcessBlock({spend}, coinbase_script, masternodeID);
    const uint256 tip_hash = block.GetHash();
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2UL);

    // Start the index and wait until it caught up with the chain.
    TxoSpenderIndex txospenderindex(1 << 20, true);
    BOOST_CHECK(!txospenderindex.BlockUntilSyncedToCurrentChain());
    txospenderindex.Start();

    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txospenderindex.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const COutPoint spent_outpoint{m_coinbase_txns[0]->GetHash(), 0};

    // The spending transaction is found and confirmed in the tip.
    const std::optional<TxoSpender> spender{txospenderindex.FindSpender(spent_outpoint)};
    BOOST_REQUIRE(spender);
    BOOST_CHECK_EQUAL(spender->block_hash, tip_hash);
    BOOST_REQUIRE(txospenderindex.GetBestBlockIndex());
    BOOST_CHECK_EQUAL(txospenderindex.GetBestBlockIndex()->GetBlockHash(), tip_hash);

    // Invalidate the block that contains the spending transaction. The block
    // leaves the active chain and the index has to drop its entries.
    CBlockIndex* pindex;
    {
        LOCK(cs_main);
        pindex = LookupBlockIndex(tip_hash);
    }
    BOOST_REQUIRE(pindex);
    CValidationState state;
    BOOST_REQUIRE(InvalidateBlock(state, Params(), pindex));

    // Wait for the validation interface callbacks (the block disconnect) to be
    // processed.
    SyncWithValidationInterfaceQueue();

    // The spending transaction is not part of the active chain anymore, so the
    // index must not report it (and especially not the hash of the
    // disconnected block).
    BOOST_CHECK(!txospenderindex.FindSpender(spent_outpoint).has_value());

    // The index best block moved back to the parent of the disconnected block.
    BOOST_REQUIRE(txospenderindex.GetBestBlockIndex());
    BOOST_REQUIRE(pindex->pprev);
    BOOST_CHECK_EQUAL(txospenderindex.GetBestBlockIndex()->GetBlockHash(), pindex->pprev->GetBlockHash());

    // Mine the spending transaction again. The index has to report the new
    // block and must not trigger a rewind. A different coinbase script is used
    // for the new block, so that it really is a new block and not a copy of the
    // disconnected one.
    const CKey& owner_key{testMasternodeKeys.begin()->second.ownerKey};
    const CScript new_coinbase_script{CScript() << ToByteVector(owner_key.GetPubKey()) << OP_CHECKSIG};
    const CBlock new_block = CreateAndProcessBlock({spend}, new_coinbase_script, masternodeID);
    BOOST_REQUIRE(new_block.GetHash() != tip_hash);
    SyncWithValidationInterfaceQueue();

    const std::optional<TxoSpender> new_spender{txospenderindex.FindSpender(spent_outpoint)};
    BOOST_REQUIRE(new_spender);
    BOOST_CHECK_EQUAL(new_spender->tx->GetHash(), spend.GetHash());
    BOOST_CHECK_EQUAL(new_spender->block_hash, new_block.GetHash());
    BOOST_REQUIRE(txospenderindex.GetBestBlockIndex());
    BOOST_CHECK_EQUAL(txospenderindex.GetBestBlockIndex()->GetBlockHash(), new_block.GetHash());

    // shutdown sequence (c.f. Shutdown() in init.cpp)
    txospenderindex.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
