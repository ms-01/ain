// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dfi/masternodes.h>
#include <dfi/mn_checks.h>
#include <limits>
#include <test/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(applytx_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(neg_token_amounts)
{
    {
        CTokenAmount val{};
        auto res = val.Add(-100);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount: -0.00000100");

        res = val.Sub(-100);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount: -0.00000100");
    }

    { // it is possible to create neg TokenAmount, but can't manipulate with it
        CTokenAmount val{DCT_ID{}, -100};
        auto res = val.Add(100);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount");

        res = val.Sub(100);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "amount -0.00000100 is less than 0.00000100");

        res = val.Sub(-200);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount: -0.00000200");
    }
}

// redundant due to 'neg_token_amounts'
BOOST_AUTO_TEST_CASE(neg_token_balances)
{
    LOCK(cs_main);

    CCustomCSView mnview(*pcustomcsview);

    CScript const owner = CScript(1);
    DCT_ID const DFI{};
    {
        // Initial value
        auto dfi100 = CTokenAmount{DFI, 100};
        auto res = mnview.AddBalance(owner, dfi100);
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);

        // Fail to add negative
        res = mnview.AddBalance(owner, CTokenAmount{DFI, -100});
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount: -0.00000100");
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);

        // Fail to sub negative
        res = mnview.SubBalance(owner, CTokenAmount{DFI, -100});
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.msg, "negative amount: -0.00000100");
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);
    }
}

template <typename T>
CScript CreateMeta(CustomTxType type, const T &msg) {
    CDataStream markedMetadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    markedMetadata << static_cast<unsigned char>(type) << msg;
    CScript scriptMeta;
    scriptMeta << OP_RETURN << ToByteVector(markedMetadata);
    return scriptMeta;
}

CScript CreateMetaA2A(CAccountToAccountMessage const & msg) {
    return CreateMeta(CustomTxType::AccountToAccount, msg);
}

CScript CreateRawMetaA2ADuplicateKey(const CScript &from, const CScript &toScript, CAmount amount1, CAmount amount2, DCT_ID tokenId) {
    CDataStream markedMetadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    markedMetadata << static_cast<unsigned char>(CustomTxType::AccountToAccount);
    markedMetadata << from;
    WriteCompactSize(markedMetadata, 2);
    markedMetadata << toScript;
    markedMetadata << CBalances{{{tokenId, amount1}}};
    markedMetadata << toScript;
    markedMetadata << CBalances{{{tokenId, amount2}}};
    CScript scriptMeta;
    scriptMeta << OP_RETURN << ToByteVector(markedMetadata);
    return scriptMeta;
}

// redundant due to 'neg_token_amounts'
BOOST_AUTO_TEST_CASE(apply_a2a_neg)
{
    Consensus::Params amkCheated = Params().GetConsensus();
    amkCheated.DF1AMKHeight = 0;

    LOCK(cs_main);

    BlockContext blockCtx{{}, {}, amkCheated};
    auto &mnview = blockCtx.GetView();
    CCoinsViewCache coinview(&::ChainstateActive().CoinsTip());

    CScript owner{424242};
    DCT_ID DFI{};

    // add auth coin to coinview
    auto auth_out = COutPoint(uint256S("0xafaf"),42);
    coinview.AddCoin(auth_out, Coin({1, owner, DFI}, 1, false), false);

    // Initial value
    const auto dfi100 = CTokenAmount{DFI, 100};
    auto res = mnview.AddBalance(owner, dfi100);
    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);

    // create templates for msg and tx:
    CAccountToAccountMessage msg{};
    msg.from = owner;
    CMutableTransaction rawTx;
    rawTx.vout = { CTxOut(0, {}) };
    rawTx.vin = { CTxIn(auth_out) };

    // try to send "A:-1@DFI"
    {
        msg.to = {
            { CScript(0xA), CBalances{{ {DFI, -1} }} }
        };

        rawTx.vout[0].scriptPubKey = CreateMetaA2A(msg);

        const auto tx = CTransaction(rawTx);
        auto txCtx = TransactionContext{
            coinview,
            tx,
            blockCtx,
        };

        res = ApplyCustomTx(blockCtx, txCtx);

        BOOST_CHECK(!res.ok);
        BOOST_CHECK_NE(res.msg.find("negative amount"), std::string::npos);
        // check that nothing changes:
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);
        BOOST_CHECK_EQUAL(mnview.GetBalance(CScript(0xA), DFI), CTokenAmount{});
    }

    // try to send "A:101@DFI"
    {
        msg.to = {
            { CScript(0xA), CBalances{{ {DFI, 101} }} }
        };

        rawTx.vout[0].scriptPubKey = CreateMetaA2A(msg);

        const auto tx = CTransaction(rawTx);
        auto txCtx = TransactionContext{
                coinview,
                tx,
                blockCtx,
        };

        res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(res.code, (uint32_t) CustomTxErrCodes::NotEnoughBalance);
        // check that nothing changes:
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);
        BOOST_CHECK_EQUAL(mnview.GetBalance(CScript(0xA), DFI), CTokenAmount{});
    }

    // try to send "A:10@DFI, B:-1@DFI"
    {
        msg.to = {
            { CScript(0xA), CBalances{{ {DFI, 10} }} },
            { CScript(0xB), CBalances{{ {DFI, -1} }} }
        };

        rawTx.vout[0].scriptPubKey = CreateMetaA2A(msg);

        const auto tx = CTransaction(rawTx);
        auto txCtx = TransactionContext{
                coinview,
                tx,
                blockCtx,
        };

        res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_NE(res.msg.find("negative amount"), std::string::npos);
        // check that nothing changes:
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi100);
        BOOST_CHECK_EQUAL(mnview.GetBalance(CScript(0xA), DFI), CTokenAmount{});
        BOOST_CHECK_EQUAL(mnview.GetBalance(CScript(0xB), DFI), CTokenAmount{});
    }

    // send "A:10@DFI" (success)
    {
        msg.to = {
            { CScript(0xA), CBalances{{ {DFI, 10} }} },
        };

        rawTx.vout[0].scriptPubKey = CreateMetaA2A(msg);

        const auto tx = CTransaction(rawTx);
        auto txCtx = TransactionContext{
                coinview,
                tx,
                blockCtx,
        };

        res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(res.ok);
        // check result balances:
        auto const dfi90 = CTokenAmount{DFI, 90};
        auto const dfi10 = CTokenAmount{DFI, 10};
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI), dfi90);
        BOOST_CHECK_EQUAL(mnview.GetBalance(CScript(0xA), DFI), dfi10);
    }
}

// The sum of the transfer amounts must not overflow. From the activation
// height on the overflow is a consensus error, below it the historical
// behaviour is kept (the error is swallowed and the sum stays truncated).
BOOST_AUTO_TEST_CASE(apply_a2a_sum_overflow)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    // The strict rule is active from the start.
    consensus.nBalanceOverflowHeight = 0;

    LOCK(cs_main);
    BlockContext blockCtx{{}, {}, consensus};
    auto &view = blockCtx.GetView();
    CCoinsViewCache coinview(&::ChainstateActive().CoinsTip());

    CScript owner;
    CScript firstRecipient;
    CScript secondRecipient;
    owner << OP_3;
    firstRecipient << OP_1;
    secondRecipient << OP_2;

    const DCT_ID dfi{};
    const CTokenAmount initial{dfi, 100};
    const auto authOut = COutPoint(uint256S("0xbaba"), 0);
    coinview.AddCoin(authOut, Coin({1, owner, dfi}, 1, false), false);
    BOOST_REQUIRE(view.AddBalance(owner, initial).ok);

    const auto ownerBefore = view.GetBalance(owner, dfi).nValue;
    const auto firstBefore = view.GetBalance(firstRecipient, dfi).nValue;
    const auto secondBefore = view.GetBalance(secondRecipient, dfi).nValue;

    // Exercise both orders of individually valid, nonnegative amounts.
    for (const bool reverse : {false, true}) {
        BOOST_TEST_CONTEXT("reverse amount order: " << reverse) {
            const CAmount maximum = std::numeric_limits<CAmount>::max();
            const CAmount firstAmount = reverse ? maximum : 1;
            const CAmount secondAmount = reverse ? 1 : maximum;

            CAccountToAccountMessage msg{};
            msg.from = owner;
            msg.to = {
                {firstRecipient, CBalances{{{dfi, firstAmount}}}},
                {secondRecipient, CBalances{{{dfi, secondAmount}}}},
            };

            CMutableTransaction rawTx;
            rawTx.vin = {CTxIn(authOut)};
            rawTx.vout = {CTxOut(0, CreateMetaA2A(msg))};
            const CTransaction tx(rawTx);
            auto txCtx = TransactionContext{coinview, tx, blockCtx};

            const auto result = ApplyCustomTx(blockCtx, txCtx);
            BOOST_REQUIRE_MESSAGE(!result.ok, "Overflowing transfer must be rejected");
            BOOST_CHECK_NE(result.msg.find("overflow"), std::string::npos);
            // The error is tagged with TransferSumOverflow, so that the
            // mempool is able to drop transactions that were accepted below
            // the activation height.
            BOOST_CHECK_NE(result.code & (uint32_t)CustomTxErrCodes::TransferSumOverflow, 0U);

            // Nothing may change.
            BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, ownerBefore);
            BOOST_CHECK_EQUAL(view.GetBalance(firstRecipient, dfi).nValue, firstBefore);
            BOOST_CHECK_EQUAL(view.GetBalance(secondRecipient, dfi).nValue, secondBefore);
        }
    }
}

// The transfer sum is checked in all message types that carry it.
BOOST_AUTO_TEST_CASE(apply_transfer_sum_validation)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.nBalanceOverflowHeight = 0;
    consensus.DF2BayfrontHeight = 0;
    consensus.DF3BayfrontMarinaHeight = 0;
    consensus.DF4BayfrontGardensHeight = 0;

    LOCK(cs_main);

    // mode: 0 = UTXO recipients, 1 = account inputs, 2 = account outputs.
    for (int mode = 0; mode < 3; ++mode) {
        // Two overflowing amount orders and one valid control.
        for (int scenario = 0; scenario < 3; ++scenario) {
            BOOST_TEST_CONTEXT("mode=" << mode << ", scenario=" << scenario) {
                BlockContext blockCtx{{}, {}, consensus};
                auto &view = blockCtx.GetView();
                CCoinsViewCache coins(&::ChainstateActive().CoinsTip());

                CScript senderA, senderB, recipientA, recipientB;
                senderA << OP_3;
                senderB << OP_4;
                recipientA << OP_1;
                recipientB << OP_2;
                const DCT_ID dfi{};

                BOOST_REQUIRE(view.AddBalance(senderA, {dfi, 100}).ok);
                BOOST_REQUIRE(view.AddBalance(senderB, {dfi, 100}).ok);
                BOOST_REQUIRE(view.AddBalance(recipientA, {dfi, 7}).ok);
                BOOST_REQUIRE(view.AddBalance(recipientB, {dfi, 9}).ok);

                const auto beforeA = view.GetBalance(senderA, dfi).nValue;
                const auto beforeB = view.GetBalance(senderB, dfi).nValue;
                const auto beforeRecipientA = view.GetBalance(recipientA, dfi).nValue;
                const auto beforeRecipientB = view.GetBalance(recipientB, dfi).nValue;

                const COutPoint authA(uint256S("0xcaca"), 0);
                const COutPoint authB(uint256S("0xcaca"), 1);
                coins.AddCoin(authA, Coin({1, senderA, dfi}, 1, false), false);
                coins.AddCoin(authB, Coin({1, senderB, dfi}, 1, false), false);

                const bool valid = scenario == 2;
                const CAmount maximum = std::numeric_limits<CAmount>::max();
                const CAmount first = valid || scenario == 0 ? 1 : maximum;
                const CAmount second = valid || scenario == 1 ? 1 : maximum;
                const CAccounts amounts{
                    {recipientA, CBalances{{{dfi, first}}}},
                    {recipientB, CBalances{{{dfi, second}}}},
                };

                auto apply = [&](CustomTxType type, const auto &message, CAmount burnt) {
                    CDataStream metadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
                    metadata << static_cast<unsigned char>(type) << message;
                    CScript script;
                    script << OP_RETURN << ToByteVector(metadata);

                    CMutableTransaction rawTx;
                    rawTx.vin = {CTxIn(authA), CTxIn(authB)};
                    rawTx.vout = {CTxOut(burnt, script)};
                    const CTransaction tx(rawTx);
                    auto txCtx = TransactionContext{coins, tx, blockCtx};
                    return ApplyCustomTx(blockCtx, txCtx);
                };

                const auto result = [&]() -> Res {
                    if (mode == 0) {
                        CUtxosToAccountMessage message{};
                        message.to = amounts;
                        return apply(CustomTxType::UtxosToAccount, message, valid ? 2 : 1);
                    }

                    CAnyAccountsToAccountsMessage message{};
                    if (mode == 1) {
                        message.from = {
                            {senderA, CBalances{{{dfi, first}}}},
                            {senderB, CBalances{{{dfi, second}}}},
                        };
                        message.to = {
                            {recipientA, CBalances{{{dfi, valid ? 2 : 1}}}},
                        };
                    } else {
                        message.from = {{senderA, CBalances{{{dfi, 2}}}}};
                        message.to = amounts;
                    }
                    return apply(CustomTxType::AnyAccountsToAccounts, message, 0);
                }();

                if (!valid) {
                    BOOST_REQUIRE_MESSAGE(!result.ok, "Overflow must be rejected");
                    BOOST_CHECK_NE(result.msg.find("overflow"), std::string::npos);
                    BOOST_CHECK_NE(result.code & (uint32_t)CustomTxErrCodes::TransferSumOverflow, 0U);
                    BOOST_CHECK_EQUAL(view.GetBalance(senderA, dfi).nValue, beforeA);
                    BOOST_CHECK_EQUAL(view.GetBalance(senderB, dfi).nValue, beforeB);
                    BOOST_CHECK_EQUAL(view.GetBalance(recipientA, dfi).nValue, beforeRecipientA);
                    BOOST_CHECK_EQUAL(view.GetBalance(recipientB, dfi).nValue, beforeRecipientB);
                } else {
                    BOOST_REQUIRE_MESSAGE(result.ok, result.msg);
                    BOOST_CHECK_EQUAL(view.GetBalance(senderA, dfi).nValue,
                                      beforeA - (mode == 0 ? 0 : mode == 1 ? 1 : 2));
                    BOOST_CHECK_EQUAL(view.GetBalance(senderB, dfi).nValue,
                                      beforeB - (mode == 1 ? 1 : 0));
                    BOOST_CHECK_EQUAL(view.GetBalance(recipientA, dfi).nValue,
                                      beforeRecipientA + (mode == 1 ? 2 : 1));
                    BOOST_CHECK_EQUAL(view.GetBalance(recipientB, dfi).nValue,
                                      beforeRecipientB + (mode == 1 ? 0 : 1));
                }
            }
        }
    }
}

// A mismatch between the sum of the inputs and the sum of the outputs is an
// error as well, but it is not related to the overflow. It must not carry the
// TransferSumOverflow code - only the overflow may evict a transaction from
// the mempool.
BOOST_AUTO_TEST_CASE(apply_transfer_sum_mismatch)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.nBalanceOverflowHeight = 0;
    consensus.DF2BayfrontHeight = 0;
    consensus.DF3BayfrontMarinaHeight = 0;
    consensus.DF4BayfrontGardensHeight = 0;

    LOCK(cs_main);
    BlockContext blockCtx{{}, {}, consensus};
    auto &view = blockCtx.GetView();
    CCoinsViewCache coins(&::ChainstateActive().CoinsTip());

    CScript senderA, senderB, recipientA;
    senderA << OP_3;
    senderB << OP_4;
    recipientA << OP_1;
    const DCT_ID dfi{};

    BOOST_REQUIRE(view.AddBalance(senderA, {dfi, 100}).ok);
    BOOST_REQUIRE(view.AddBalance(senderB, {dfi, 100}).ok);
    BOOST_REQUIRE(view.AddBalance(recipientA, {dfi, 7}).ok);

    const auto beforeA = view.GetBalance(senderA, dfi).nValue;
    const auto beforeB = view.GetBalance(senderB, dfi).nValue;
    const auto beforeRecipientA = view.GetBalance(recipientA, dfi).nValue;

    const COutPoint authA(uint256S("0xcaca"), 0);
    const COutPoint authB(uint256S("0xcaca"), 1);
    coins.AddCoin(authA, Coin({1, senderA, dfi}, 1, false), false);
    coins.AddCoin(authB, Coin({1, senderB, dfi}, 1, false), false);

    // The inputs sum up to 2, the outputs to 3.
    CAnyAccountsToAccountsMessage message{};
    message.from = {
        {senderA, CBalances{{{dfi, 1}}}},
        {senderB, CBalances{{{dfi, 1}}}},
    };
    message.to = {{recipientA, CBalances{{{dfi, 3}}}}};

    CDataStream metadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    metadata << static_cast<unsigned char>(CustomTxType::AnyAccountsToAccounts) << message;
    CScript script;
    script << OP_RETURN << ToByteVector(metadata);

    CMutableTransaction rawTx;
    rawTx.vin = {CTxIn(authA), CTxIn(authB)};
    rawTx.vout = {CTxOut(0, script)};
    const CTransaction tx(rawTx);
    auto txCtx = TransactionContext{coins, tx, blockCtx};

    const auto result = ApplyCustomTx(blockCtx, txCtx);
    BOOST_REQUIRE_MESSAGE(!result.ok, "The mismatch must be rejected");
    BOOST_CHECK_NE(result.msg.find("sum of inputs"), std::string::npos);
    // Only the overflow is tagged - a mismatch must not evict the
    // transaction from the mempool.
    BOOST_CHECK_EQUAL(result.code & (uint32_t)CustomTxErrCodes::TransferSumOverflow, 0U);

    // Nothing may change.
    BOOST_CHECK_EQUAL(view.GetBalance(senderA, dfi).nValue, beforeA);
    BOOST_CHECK_EQUAL(view.GetBalance(senderB, dfi).nValue, beforeB);
    BOOST_CHECK_EQUAL(view.GetBalance(recipientA, dfi).nValue, beforeRecipientA);
}

// Below the activation height the historical behaviour has to be reproduced
// bit-exactly (the overflow error is swallowed, the sum stays truncated at the
// first amount and the loop continues), from the activation height on the
// overflow is an error.
BOOST_AUTO_TEST_CASE(apply_a2a_activation_boundary)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    constexpr int activation = 100;
    consensus.nBalanceOverflowHeight = activation;
    LOCK(cs_main);
    for (const auto height : {99U, 100U, 101U}) {
        // 0: small amount first; 1: large amount first; 2: valid control.
        for (int scenario = 0; scenario < 3; ++scenario) {
            BOOST_TEST_CONTEXT("height=" << height << ", scenario=" << scenario) {
                BlockContext blockCtx{height, 0, consensus};
                auto &view = blockCtx.GetView();
                CCoinsViewCache coins(&::ChainstateActive().CoinsTip());
                CScript owner, a, b;
                owner << OP_3;
                a << OP_1;
                b << OP_2;
                const DCT_ID dfi{};
                BOOST_REQUIRE(view.AddBalance(owner, {dfi, 100}).ok);
                const COutPoint auth(uint256S("0xdada"), 0);
                coins.AddCoin(auth, Coin({1, owner, dfi}, 1, false), false);
                const auto maximum = std::numeric_limits<CAmount>::max();
                const CAmount first = scenario == 1 ? maximum : 1;
                const CAmount second = scenario == 0 ? maximum : 1;
                CAccountToAccountMessage message{};
                message.from = owner;
                message.to = {{a, CBalances{{{dfi, first}}}},
                              {b, CBalances{{{dfi, second}}}}};
                CMutableTransaction raw;
                raw.vin = {CTxIn(auth)};
                raw.vout = {CTxOut(0, CreateMetaA2A(message))};
                const CTransaction tx(raw);
                auto txCtx = TransactionContext{coins, tx, blockCtx};
                const auto result = ApplyCustomTx(blockCtx, txCtx);
                if (scenario == 2 || (height < activation && scenario == 0)) {
                    // Below the activation height the truncated sum is used:
                    // the source is debited with the first amount only.
                    BOOST_REQUIRE_MESSAGE(result.ok, result.msg);
                    BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue,
                                      scenario == 2 ? 98 : 99);
                    BOOST_CHECK_EQUAL(view.GetBalance(a, dfi).nValue, first);
                    BOOST_CHECK_EQUAL(view.GetBalance(b, dfi).nValue, second);
                } else {
                    BOOST_REQUIRE(!result.ok);
                    BOOST_CHECK_NE(result.msg.find(height < activation ? "less than" : "overflow"),
                                   std::string::npos);
                    if (height >= activation) {
                        BOOST_CHECK_NE(result.code & (uint32_t)CustomTxErrCodes::TransferSumOverflow, 0U);
                    }
                    BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, 100);
                    BOOST_CHECK_EQUAL(view.GetBalance(a, dfi).nValue, 0);
                    BOOST_CHECK_EQUAL(view.GetBalance(b, dfi).nValue, 0);
                }
            }
        }
    }
}

// Documents the failed exploit attempt of block 6,565,374:
// The attacker supplied the same destination script twice, with 1 sat and INT64_MAX.
// Because CAccounts is a std::map, deserialization (m.insert) keeps only the first entry.
// The node debited 1 sat, credited 1 sat, and discarded the INT64_MAX amount completely.
BOOST_AUTO_TEST_CASE(apply_a2a_duplicate_recipient_script)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.nBalanceOverflowHeight = 0;

    LOCK(cs_main);
    BlockContext blockCtx{{}, {}, consensus};
    auto &view = blockCtx.GetView();
    CCoinsViewCache coinview(&::ChainstateActive().CoinsTip());

    CScript owner;
    CScript recipient;
    owner << OP_3;
    recipient << OP_1;

    const DCT_ID dfi{};
    const CTokenAmount initial{dfi, 100};
    const auto authOut = COutPoint(uint256S("0xbaba"), 0);
    coinview.AddCoin(authOut, Coin({1, owner, dfi}, 1, false), false);
    BOOST_REQUIRE(view.AddBalance(owner, initial).ok);

    const auto maxAmount = std::numeric_limits<CAmount>::max();
    CMutableTransaction rawTx;
    rawTx.vin = {CTxIn(authOut)};
    rawTx.vout = {CTxOut(0, CreateRawMetaA2ADuplicateKey(owner, recipient, 1, maxAmount, dfi))};
    const CTransaction tx(rawTx);
    auto txCtx = TransactionContext{coinview, tx, blockCtx};

    // The transaction should succeed because the duplicate key is dropped during deserialization:
    // only 1 satoshi is transferred.
    const auto result = ApplyCustomTx(blockCtx, txCtx);
    BOOST_REQUIRE_MESSAGE(result.ok, result.msg);

    // Verify balances: sender debited by 1 sat, recipient credited by 1 sat, NO inflation.
    BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, 99);
    BOOST_CHECK_EQUAL(view.GetBalance(recipient, dfi).nValue, 1);
}

BOOST_AUTO_TEST_CASE(apply_a2a_account_max_money_boundary)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.nBalanceOverflowHeight = 0;
    constexpr int activation = 100;
    consensus.DF25Height = activation;

    LOCK(cs_main);
    for (const auto height : {99U, 100U, 101U}) {
        // 0: recipient would exceed MAX_MONEY (MAX_MONEY + 1)
        // 1: recipient reaches exact MAX_MONEY (control, must always succeed)
        for (int scenario = 0; scenario < 2; ++scenario) {
            BOOST_TEST_CONTEXT("height=" << height << ", scenario=" << scenario) {
                BlockContext blockCtx{height, 0, consensus};
                auto &view = blockCtx.GetView();
                CCoinsViewCache coins(&::ChainstateActive().CoinsTip());

                CScript owner, recipient;
                owner << OP_3;
                recipient << OP_1;
                const DCT_ID dfi{};
                const CAmount initialRecipient = scenario == 0 ? MAX_MONEY : (MAX_MONEY - 1);
                BOOST_REQUIRE(view.AddBalance(owner, {dfi, 10}).ok);
                BOOST_REQUIRE(view.AddBalance(recipient, {dfi, initialRecipient}).ok);

                const COutPoint auth(uint256S("0xcafe"), 0);
                coins.AddCoin(auth, Coin({1, owner, dfi}, 1, false), false);

                CAccountToAccountMessage message{};
                message.from = owner;
                message.to = {{recipient, CBalances{{{dfi, 1}}}}};
                CMutableTransaction raw;
                raw.vin = {CTxIn(auth)};
                raw.vout = {CTxOut(0, CreateMetaA2A(message))};
                const CTransaction tx(raw);
                auto txCtx = TransactionContext{coins, tx, blockCtx};

                const auto result = ApplyCustomTx(blockCtx, txCtx);
                if (scenario == 1 || height < activation) {
                    BOOST_REQUIRE_MESSAGE(result.ok, result.msg);
                    BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, 9);
                    BOOST_CHECK_EQUAL(view.GetBalance(recipient, dfi).nValue, initialRecipient + 1);
                } else {
                    BOOST_REQUIRE(!result.ok);
                    BOOST_CHECK_NE(result.code & (uint32_t)CustomTxErrCodes::AccountBalanceOutOfRange, 0U);
                    BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, 10);
                    BOOST_CHECK_EQUAL(view.GetBalance(recipient, dfi).nValue, initialRecipient);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(apply_a2a_account_max_money_disabled_int_max)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.DF25Height = std::numeric_limits<int>::max();

    LOCK(cs_main);
    for (const auto height : {10000000U, std::numeric_limits<uint32_t>::max()}) {
        BlockContext blockCtx{height, 0, consensus};
        auto &view = blockCtx.GetView();
        CScript owner;
        owner << OP_1;
        const DCT_ID dfi{};
        BOOST_REQUIRE(view.AddBalance(owner, {dfi, MAX_MONEY}).ok);
        BOOST_REQUIRE(view.AddBalance(owner, {dfi, 1}).ok);
        BOOST_CHECK_EQUAL(view.GetBalance(owner, dfi).nValue, MAX_MONEY + 1);
    }
}

BOOST_AUTO_TEST_CASE(apply_a2a_account_max_money_non_dfi_token)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.DF25Height = 1;

    LOCK(cs_main);
    BlockContext blockCtx{100U, 0, consensus};
    auto &view = blockCtx.GetView();
    CScript owner;
    owner << OP_1;
    const DCT_ID customToken{1};
    BOOST_REQUIRE(view.AddBalance(owner, {customToken, MAX_MONEY}).ok);
    BOOST_REQUIRE(view.AddBalance(owner, {customToken, 1}).ok);
    BOOST_CHECK_EQUAL(view.GetBalance(owner, customToken).nValue, MAX_MONEY + 1);
}

BOOST_AUTO_TEST_CASE(apply_df25_auction_bid_refund_max_money)
{
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;
    consensus.DF10EunosPayaHeight = 0;
    consensus.DF11FortCanningHeight = 0;
    consensus.DF12FortCanningMuseumHeight = 0;
    constexpr int activation = 100;
    consensus.DF25Height = activation;

    LOCK(cs_main);
    for (const auto height : {99U, 100U}) {
        BOOST_TEST_CONTEXT("height=" << height) {
            BlockContext blockCtx{height, 0, consensus};
            auto &view = blockCtx.GetView();
            CCoinsViewCache coins(&::ChainstateActive().CoinsTip());

            CScript vaultOwner, prevBidder, newBidder;
            vaultOwner << OP_1;
            prevBidder << OP_2;
            newBidder << OP_3;
            const DCT_ID dfi{};
            const CVaultId vaultId = uint256S("0x1234");

            CVaultData vault{};
            vault.ownerAddress = vaultOwner;
            vault.schemeId = "MIN150";
            vault.isUnderLiquidation = true;
            BOOST_REQUIRE(view.StoreVault(vaultId, vault).ok);

            CAuctionData auction{};
            auction.batchCount = 1;
            auction.liquidationHeight = height + 10;
            auction.liquidationPenalty = 0;
            BOOST_REQUIRE(view.StoreAuction(vaultId, auction).ok);

            CAuctionBatch batch{};
            batch.loanAmount = {dfi, 100};
            BOOST_REQUIRE(view.StoreAuctionBatch({vaultId, 0}, batch).ok);
            BOOST_REQUIRE(view.StoreAuctionBid({vaultId, 0}, {prevBidder, {dfi, 100}}).ok);

            BOOST_REQUIRE(view.AddBalance(prevBidder, {dfi, MAX_MONEY}).ok);
            BOOST_REQUIRE(view.AddBalance(newBidder, {dfi, 200}).ok);

            const COutPoint auth(uint256S("0xbeef"), 0);
            coins.AddCoin(auth, Coin({1, newBidder, dfi}, 1, false), false);

            CAuctionBidMessage bidMsg{vaultId, 0, newBidder, {dfi, 102}};
            CMutableTransaction raw;
            raw.vin = {CTxIn(auth)};
            raw.vout = {CTxOut(0, CreateMeta(CustomTxType::AuctionBid, bidMsg))};
            const CTransaction tx(raw);
            auto txCtx = TransactionContext{coins, tx, blockCtx};

            const auto result = ApplyCustomTx(blockCtx, txCtx);
            if (height < activation) {
                BOOST_REQUIRE_MESSAGE(result.ok, result.msg);
                BOOST_CHECK_EQUAL(view.GetBalance(prevBidder, dfi).nValue, MAX_MONEY + 100);
                BOOST_CHECK_EQUAL(view.GetBalance(newBidder, dfi).nValue, 98);
            } else {
                BOOST_REQUIRE(!result.ok);
                BOOST_CHECK_NE(result.code & (uint32_t)CustomTxErrCodes::AccountBalanceOutOfRange, 0U);
                BOOST_CHECK_EQUAL(view.GetBalance(prevBidder, dfi).nValue, MAX_MONEY);
                BOOST_CHECK_EQUAL(view.GetBalance(newBidder, dfi).nValue, 200);
                auto storedBid = view.GetAuctionBid({vaultId, 0});
                BOOST_REQUIRE(storedBid.has_value());
                BOOST_CHECK(storedBid->first == prevBidder);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(stats_token_balances_no_overflow_ub)
{
    CStatsTokenBalances stats;
    const DCT_ID dfi{};
    const auto maxVal = std::numeric_limits<CAmount>::max();
    const auto minVal = std::numeric_limits<CAmount>::min();

    BOOST_REQUIRE(stats.Add({dfi, maxVal}).ok);
    BOOST_REQUIRE(stats.Add({dfi, 1}).ok);
    BOOST_CHECK_EQUAL(stats.balances[dfi], minVal);

    BOOST_REQUIRE(stats.Sub({dfi, 1}).ok);
    BOOST_CHECK_EQUAL(stats.balances[dfi], maxVal);
}

BOOST_AUTO_TEST_SUITE_END()

