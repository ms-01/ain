// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dfi/masternodes.h>
#include <dfi/mn_checks.h>
#include <test/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(dfi_customtx_undo_tests, TestingSetup)

namespace {

template <typename T>
CScript MakeDfTxScript(CustomTxType type, const T &msg) {
    CDataStream markedMetadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    markedMetadata << static_cast<unsigned char>(type) << msg;
    CScript scriptMeta;
    scriptMeta << OP_RETURN << ToByteVector(markedMetadata);
    return scriptMeta;
}

}  // namespace

BOOST_AUTO_TEST_CASE(undos_view_crud_and_iteration) {
    LOCK(cs_main);

    CCustomCSView mnview(*pcustomcsview);

    const UndoKey key1{1000010, uint256S("0x01")};
    const UndoKey key2{1000020, uint256S("0x02")};
    const UndoKey keyMissing{1000099, uint256S("0x99")};

    BOOST_CHECK(!mnview.GetUndo(keyMissing).has_value());

    CUndo undo1;
    undo1.before[TBytes{'a'}] = TBytes{'1'};
    CUndo undo2;
    undo2.before[TBytes{'b'}] = {};

    BOOST_CHECK(mnview.SetUndo(key1, undo1).ok);
    BOOST_CHECK(mnview.SetUndo(key2, undo2).ok);

    auto loaded1 = mnview.GetUndo(key1);
    BOOST_REQUIRE(loaded1.has_value());
    BOOST_REQUIRE(loaded1->before.count(TBytes{'a'}) == 1);
    BOOST_REQUIRE(loaded1->before.at(TBytes{'a'}).has_value());
    BOOST_CHECK(*loaded1->before.at(TBytes{'a'}) == TBytes{'1'});

    auto loaded2 = mnview.GetUndo(key2);
    BOOST_REQUIRE(loaded2.has_value());
    BOOST_REQUIRE(loaded2->before.count(TBytes{'b'}) == 1);
    BOOST_CHECK(!loaded2->before.at(TBytes{'b'}).has_value());

    std::vector<uint32_t> heights;
    mnview.ForEachUndo(
        [&](const UndoKey &k, CLazySerialize<CUndo>) {
            heights.push_back(k.height);
            return true;
        },
        key1);
    BOOST_REQUIRE_EQUAL(heights.size(), 2u);
    BOOST_CHECK_EQUAL(heights[0], 1000010u);
    BOOST_CHECK_EQUAL(heights[1], 1000020u);

    std::vector<uint32_t> fromKey2;
    mnview.ForEachUndo(
        [&](const UndoKey &k, CLazySerialize<CUndo>) {
            fromKey2.push_back(k.height);
            return true;
        },
        key2);
    BOOST_REQUIRE_EQUAL(fromKey2.size(), 1u);
    BOOST_CHECK_EQUAL(fromKey2[0], 1000020u);

    BOOST_CHECK(mnview.DelUndo(key1).ok);
    BOOST_CHECK(!mnview.GetUndo(key1).has_value());
    BOOST_CHECK(mnview.GetUndo(key2).has_value());
}

BOOST_AUTO_TEST_CASE(undo_construct_and_revert_state) {
    LOCK(cs_main);

    CCustomCSView baseView(*pcustomcsview);
    const CScript ownerA = CScript() << OP_1;
    const CScript ownerB = CScript() << OP_2;
    const DCT_ID DFI{0};

    BOOST_CHECK(baseView.AddBalance(ownerA, CTokenAmount{DFI, 500}).ok);
    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerA, DFI).nValue, 500);
    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerB, DFI).nValue, 0);

    // Mutate in child view, construct undo against baseView, flush, then revert
    CCustomCSView txView(baseView);
    BOOST_CHECK(txView.SubBalance(ownerA, CTokenAmount{DFI, 200}).ok);
    BOOST_CHECK(txView.AddBalance(ownerB, CTokenAmount{DFI, 200}).ok);

    auto undo = CUndo::Construct(baseView.GetStorage(), txView.GetStorage().GetRaw());
    BOOST_CHECK(txView.Flush());

    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerA, DFI).nValue, 300);
    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerB, DFI).nValue, 200);

    CUndo::Revert(baseView.GetStorage(), undo);

    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerA, DFI).nValue, 500);
    BOOST_CHECK_EQUAL(baseView.GetBalance(ownerB, DFI).nValue, 0);
}

BOOST_AUTO_TEST_CASE(customtx_malformed_and_auth_errors) {
    Consensus::Params consensus = Params().GetConsensus();
    consensus.DF1AMKHeight = 0;

    LOCK(cs_main);

    BlockContext blockCtx{10, 1000, consensus};
    auto &mnview = blockCtx.GetView();
    CCoinsViewCache coinview(&::ChainstateActive().CoinsTip());

    const CScript owner = CScript() << OP_3;
    const CScript recipient = CScript() << OP_4;
    const DCT_ID DFI{0};
    BOOST_CHECK(mnview.AddBalance(owner, CTokenAmount{DFI, 1000}).ok);

    // 1. Truncated DfTx payload (valid marker + AccountToAccount type, missing body)
    {
        CDataStream truncated(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
        truncated << static_cast<unsigned char>(CustomTxType::AccountToAccount);
        CScript scriptMeta;
        scriptMeta << OP_RETURN << ToByteVector(truncated);

        CMutableTransaction mtx;
        mtx.vout = {CTxOut(0, scriptMeta)};
        const CTransaction tx(mtx);
        TransactionContext txCtx{coinview, tx, blockCtx};

        auto res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI).nValue, 1000);
    }

    // 2. Unknown CustomTxType byte is treated as CustomTxType::None (no-op)
    {
        CDataStream unknownType(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
        unknownType << static_cast<unsigned char>(0xFF);
        CScript scriptMeta;
        scriptMeta << OP_RETURN << ToByteVector(unknownType);

        CMutableTransaction mtx;
        mtx.vout = {CTxOut(0, scriptMeta)};
        const CTransaction tx(mtx);
        std::vector<unsigned char> metadata;
        BOOST_CHECK(GuessCustomTxType(tx, metadata) != CustomTxType::AccountToAccount);

        TransactionContext txCtx{coinview, tx, blockCtx};
        auto res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI).nValue, 1000);
    }

    // 3. Valid AccountToAccount message but missing auth input in coinview
    {
        CAccountToAccountMessage msg;
        msg.from = owner;
        msg.to = {{recipient, CBalances{{{DFI, 100}}}}};

        CMutableTransaction mtx;
        mtx.vout = {CTxOut(0, MakeDfTxScript(CustomTxType::AccountToAccount, msg))};
        const CTransaction tx(mtx);
        TransactionContext txCtx{coinview, tx, blockCtx};

        auto res = ApplyCustomTx(blockCtx, txCtx);
        BOOST_CHECK(!res.ok);
        BOOST_CHECK_EQUAL(mnview.GetBalance(owner, DFI).nValue, 1000);
        BOOST_CHECK_EQUAL(mnview.GetBalance(recipient, DFI).nValue, 0);
    }
}

BOOST_AUTO_TEST_CASE(vault_error_and_undo_paths) {
    LOCK(cs_main);

    CCustomCSView baseView(*pcustomcsview);
    const CVaultId vaultId = uint256S("0x1111");
    const CVaultId missingVaultId = uint256S("0x2222");
    const CScript owner1 = CScript() << OP_5;
    const CScript owner2 = CScript() << OP_6;
    const DCT_ID DFI{0};

    // Error paths on non-existent vault
    BOOST_CHECK(!baseView.GetVault(missingVaultId).has_value());
    BOOST_CHECK(!baseView.EraseVault(missingVaultId).ok);
    BOOST_CHECK(!baseView.UpdateVault(missingVaultId, CVaultMessage{owner2, "MIN150"}).ok);

    // Store vault in child view and verify undo rollback
    CCustomCSView txView(baseView);
    CVaultData vaultData;
    vaultData.ownerAddress = owner1;
    vaultData.schemeId = "MIN150";
    BOOST_CHECK(txView.StoreVault(vaultId, vaultData).ok);
    BOOST_CHECK(txView.AddVaultCollateral(vaultId, CTokenAmount{DFI, 500}).ok);

    // Negative and over-withdrawal collateral error paths
    BOOST_CHECK(!txView.AddVaultCollateral(vaultId, CTokenAmount{DFI, -10}).ok);
    BOOST_CHECK(!txView.SubVaultCollateral(vaultId, CTokenAmount{DFI, -10}).ok);
    BOOST_CHECK(!txView.SubVaultCollateral(vaultId, CTokenAmount{DFI, 600}).ok);

    // Valid update & partial withdrawal
    BOOST_CHECK(txView.UpdateVault(vaultId, CVaultMessage{owner2, "MIN200"}).ok);
    BOOST_CHECK(txView.SubVaultCollateral(vaultId, CTokenAmount{DFI, 200}).ok);

    auto undo = CUndo::Construct(baseView.GetStorage(), txView.GetStorage().GetRaw());
    BOOST_CHECK(txView.Flush());

    auto stored = baseView.GetVault(vaultId);
    BOOST_REQUIRE(stored.has_value());
    BOOST_CHECK(stored->ownerAddress == owner2);
    BOOST_CHECK_EQUAL(stored->schemeId, "MIN200");
    auto collaterals = baseView.GetVaultCollaterals(vaultId);
    BOOST_REQUIRE(collaterals.has_value());
    BOOST_CHECK_EQUAL(collaterals->balances.at(DFI), 300);

    // Revert via CUndo
    CUndo::Revert(baseView.GetStorage(), undo);
    BOOST_CHECK(!baseView.GetVault(vaultId).has_value());
    BOOST_CHECK(!baseView.GetVaultCollaterals(vaultId).has_value());
}

BOOST_AUTO_TEST_CASE(loan_error_and_undo_paths) {
    LOCK(cs_main);

    CCustomCSView baseView(*pcustomcsview);
    const CVaultId vaultId = uint256S("0x3333");
    const DCT_ID DUSD{1};

    // Non-existent loan scheme lookup returns empty
    BOOST_CHECK(!baseView.GetLoanScheme("NONEXISTENT").has_value());

    CCustomCSView txView(baseView);

    CLoanSchemeMessage scheme;
    scheme.identifier = "MIN150";
    scheme.ratio = 150;
    scheme.rate = 5 * COIN;
    BOOST_CHECK(txView.StoreLoanScheme(scheme).ok);
    BOOST_CHECK(txView.StoreDefaultLoanScheme("MIN150").ok);

    // Loan token error paths: unregistered loan token & negative amounts
    BOOST_CHECK(!txView.AddLoanToken(vaultId, CTokenAmount{DUSD, -50}).ok);
    BOOST_CHECK(!txView.AddLoanToken(vaultId, CTokenAmount{DUSD, 1000}).ok);
    BOOST_CHECK(!txView.SubLoanToken(vaultId, CTokenAmount{DUSD, -10}).ok);
    BOOST_CHECK(!txView.SubLoanToken(vaultId, CTokenAmount{DUSD, 1500}).ok);

    BOOST_CHECK(txView.SetLoanLiquidationPenalty(5 * COIN).ok);

    auto undo = CUndo::Construct(baseView.GetStorage(), txView.GetStorage().GetRaw());
    BOOST_CHECK(txView.Flush());

    BOOST_CHECK(baseView.GetLoanScheme("MIN150").has_value());
    BOOST_CHECK_EQUAL(baseView.GetDefaultLoanScheme().value_or(""), "MIN150");
    BOOST_CHECK_EQUAL(baseView.GetLoanLiquidationPenalty(), 5 * COIN);

    // Revert via CUndo
    CUndo::Revert(baseView.GetStorage(), undo);
    BOOST_CHECK(!baseView.GetLoanScheme("MIN150").has_value());
    BOOST_CHECK(!baseView.GetDefaultLoanScheme().has_value());
}

BOOST_AUTO_TEST_CASE(poolpairs_error_and_undo_paths) {
    LOCK(cs_main);

    CCustomCSView baseView(*pcustomcsview);
    const DCT_ID poolId{10};
    const DCT_ID missingPoolId{99};
    const DCT_ID tokenA{0};
    const DCT_ID tokenB{1};
    const CScript owner = CScript() << OP_7;

    // Updating non-existent pool fails
    BOOST_CHECK(!baseView.UpdatePoolPair(missingPoolId, 10, true, 0, owner, {}).ok);

    CCustomCSView txView(baseView);

    // Same tokenA == tokenB fails
    CPoolPair badPool;
    badPool.idTokenA = tokenA;
    badPool.idTokenB = tokenA;
    badPool.commission = 0;
    badPool.ownerAddress = owner;
    badPool.status = true;
    BOOST_CHECK(!txView.SetPoolPair(poolId, 10, badPool).ok);

    // Valid pool pair creation
    CPoolPair goodPool;
    goodPool.idTokenA = tokenA;
    goodPool.idTokenB = tokenB;
    goodPool.commission = 200000;
    goodPool.ownerAddress = owner;
    goodPool.status = true;
    BOOST_CHECK(txView.SetPoolPair(poolId, 10, goodPool).ok);

    // Duplicate token pair fails
    BOOST_CHECK(!txView.SetPoolPair(DCT_ID{11}, 10, goodPool).ok);

    // DEX fee pct set & verify
    BOOST_CHECK(txView.SetDexFeePct(poolId, tokenA, 100000).ok);

    // AddLiquidity / RemoveLiquidity boundary errors on pool object
    auto poolOpt = txView.GetPoolPair(poolId);
    BOOST_REQUIRE(poolOpt.has_value());
    BOOST_CHECK(!poolOpt->AddLiquidity(0, 100, [](CAmount) { return Res::Ok(); }).ok);
    BOOST_CHECK(!poolOpt->AddLiquidity(-1, 100, [](CAmount) { return Res::Ok(); }).ok);
    BOOST_CHECK(!poolOpt->RemoveLiquidity(0, [](CAmount, CAmount) { return Res::Ok(); }).ok);
    BOOST_CHECK(!poolOpt->RemoveLiquidity(100, [](CAmount, CAmount) { return Res::Ok(); }).ok);

    auto undo = CUndo::Construct(baseView.GetStorage(), txView.GetStorage().GetRaw());
    BOOST_CHECK(txView.Flush());

    BOOST_CHECK(baseView.GetPoolPair(poolId).has_value());
    BOOST_CHECK_EQUAL(baseView.GetDexFeeInPct(poolId, tokenA), 100000);

    // Revert via CUndo
    CUndo::Revert(baseView.GetStorage(), undo);
    BOOST_CHECK(!baseView.GetPoolPair(poolId).has_value());
    BOOST_CHECK_EQUAL(baseView.GetDexFeeInPct(poolId, tokenA), 0);
}

BOOST_AUTO_TEST_CASE(proposals_error_and_undo_paths) {
    LOCK(cs_main);

    CCustomCSView baseView(*pcustomcsview);
    const CProposalId propId = uint256S("0x4444");
    const CProposalId missingPropId = uint256S("0x5555");
    const uint256 mnId = uint256S("0x6666");

    // Error paths on non-existent proposal
    BOOST_CHECK(!baseView.GetProposal(missingPropId).has_value());
    BOOST_CHECK(!baseView.UpdateProposalStatus(missingPropId, 10, CProposalStatusType::Rejected).ok);
    BOOST_CHECK(!baseView.AddProposalVote(missingPropId, mnId, CProposalVoteType::VoteYes).ok);

    CCustomCSView txView(baseView);

    CCreateProposalMessage msg;
    msg.type = CProposalType::CommunityFundProposal;
    msg.address = CScript() << OP_8;
    msg.nAmount = 100 * COIN;
    msg.nCycles = 2;
    msg.title = "Test Proposal";
    msg.context = "https://example.invalid/proposal";
    msg.options = 0;

    BOOST_CHECK(txView.CreateProposal(propId, 100, msg, 10 * COIN).ok);
    BOOST_CHECK(txView.AddProposalVote(propId, mnId, CProposalVoteType::VoteYes).ok);
    BOOST_CHECK(txView.AddProposalVote(propId, mnId, CProposalVoteType::VoteNo).ok);

    auto undo = CUndo::Construct(baseView.GetStorage(), txView.GetStorage().GetRaw());
    BOOST_CHECK(txView.Flush());

    auto prop = baseView.GetProposal(propId);
    BOOST_REQUIRE(prop.has_value());
    BOOST_CHECK_EQUAL(prop->title, "Test Proposal");
    auto vote = baseView.GetProposalVote(propId, 1, mnId);
    BOOST_REQUIRE(vote.has_value());
    BOOST_CHECK(*vote == CProposalVoteType::VoteNo);

    // Revert via CUndo
    CUndo::Revert(baseView.GetStorage(), undo);
    BOOST_CHECK(!baseView.GetProposal(propId).has_value());
    BOOST_CHECK(!baseView.GetProposalVote(propId, 1, mnId).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
