// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <index/txospenderindex.h>
#include <primitives/transaction.h>
#include <script/sign.h>
#include <test/setup_common.h>
#include <util/time.h>
#include <validation.h>

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

BOOST_AUTO_TEST_SUITE_END()
