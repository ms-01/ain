#include <chainparams.h>
#include <dfi/govvariables/attributes.h>
#include <dfi/masternodes.h>
#include <dfi/mn_checks.h>
#include <dfi/poolpairs.h>
#include <validation.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

std::pair<CFeeDir, CFeeDir> asymmetricFee{CFeeDir{FeeDirValues::Both}, CFeeDir{FeeDirValues::Both}};

inline uint256 NextTx()
{
    static int txs_counter = 1;

    std::stringstream stream;
    stream << std::hex << txs_counter++;
    return uint256S(stream.str() );
}

DCT_ID CreateToken(CCustomCSView &mnview, std::string const & symbol, uint8_t flags = (uint8_t)CToken::TokenFlags::Default)
{
    CTokenImplementation token;
    token.creationTx = NextTx();
    token.symbol = symbol;
    token.flags = flags;

    BlockContext dummyContext{std::numeric_limits<uint32_t>::max(), {}, Params().GetConsensus()};
    auto res = mnview.CreateToken(token, dummyContext);
    if (!res.ok) printf("%s\n", res.msg.c_str());
    BOOST_REQUIRE(res.ok);
    return *res.val;
}


// tokenz A,B,LP
std::tuple<DCT_ID, DCT_ID, DCT_ID> CreatePoolNTokens(CCustomCSView &mnview, std::string const & symbolA, std::string const & symbolB)
{
    DCT_ID idA = CreateToken(mnview, symbolA);
    DCT_ID idB = CreateToken(mnview, symbolB);

    DCT_ID idPool = CreateToken(mnview, symbolA + "-" +  symbolB, (uint8_t)CToken::TokenFlags::Default | (uint8_t)CToken::TokenFlags::DAT | (uint8_t)CToken::TokenFlags::LPS);
    {
        CPoolPair pool{};
        pool.idTokenA = idA;
        pool.idTokenB = idB;
        pool.commission = 1000000; // 1%
    //    poolMsg.ownerAddress = CScript();
        pool.status = true;
        BOOST_REQUIRE(mnview.SetPoolPair(idPool, 1, pool).ok);
    }
    return std::tuple<DCT_ID, DCT_ID, DCT_ID>(idA, idB, idPool); // ! simple initialization list (as "{a,b,c}")  doesn't work here under ubuntu 16.04 - due to older gcc?
}

Res AddPoolLiquidity(CCustomCSView &mnview, DCT_ID idPool, CAmount amountA, CAmount amountB, CScript const & shareAddress)
{
    auto optPool = mnview.GetPoolPair(idPool);
    BOOST_REQUIRE(optPool);

    const auto res = optPool->AddLiquidity(amountA, amountB, [&] /*onMint*/(CAmount liqAmount) -> Res {
        BOOST_CHECK(liqAmount > 0);

        auto add = mnview.AddBalance(shareAddress, { idPool, liqAmount });
        if (!add.ok) {
            return add;
        }
        //insert update ByShare index
        const auto setShare = mnview.SetShare(idPool, shareAddress, 1);
        if (!setShare.ok) {
            return setShare;
        }
        return Res::Ok();
    });
    BOOST_REQUIRE(res.ok);
    return mnview.SetPoolPair(idPool, 1, *optPool);
}

struct CLiquidityRewardsTest : public TestingSetup
{
    CLiquidityRewardsTest()
    {
        const_cast<int&>(Params().GetConsensus().DF14FortCanningHillHeight) = 7;
    }
};

BOOST_FIXTURE_TEST_SUITE(liquidity_tests, CLiquidityRewardsTest)

BOOST_AUTO_TEST_CASE(math_liquidity_and_trade)
{
    auto FAIL_onMint = [](CAmount) { BOOST_REQUIRE(false); return Res::Err("it should not happen"); };
    auto FAIL_onSwap = [](const CTokenAmount &, const CTokenAmount &) { BOOST_REQUIRE(false); return Res::Err("it should not happen"); };

    CCustomCSView mnview(*pcustomcsview);

    DCT_ID idA, idB, idPool;
    std::tie(idA, idB, idPool) = CreatePoolNTokens(mnview, "AAA", "BBB");
    auto optPool = mnview.GetPoolPair(idPool);
    BOOST_REQUIRE(optPool);

    Res res{};
    { // basic fails
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(-1, 1000, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg == "amounts should be positive");
        res = pool.AddLiquidity(0, 1000, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg == "amounts should be positive");
        res = pool.AddLiquidity(1, 1000, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg == "liquidity too low");
        res = pool.AddLiquidity(10, 100000, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg == "liquidity too low"); // median == MINIMUM_LIQUIDITY
    }

    { // amounts a bit larger MINIMUM_LIQUIDITY
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(11, 100000, [](CAmount liq)-> Res {
            BOOST_CHECK(liq == 48); // sqrt (11*100000) - MINIMUM_LIQUIDITY
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK(pool.reserveA == 11);
        BOOST_CHECK(pool.reserveB == 100000);
        // total liquidity is the minted amount plus the minimum liquidity locked in the pool
        BOOST_CHECK(pool.totalLiquidity == 48 + CPoolPair::MINIMUM_LIQUIDITY);

        // the values must survive the round trip through the storage layer as well
        BOOST_REQUIRE(mnview.SetPoolPair(idPool, 1, pool).ok);
        auto optStoredPool = mnview.GetPoolPair(idPool);
        BOOST_REQUIRE(optStoredPool);
        BOOST_CHECK_EQUAL(optStoredPool->reserveA, 11);
        BOOST_CHECK_EQUAL(optStoredPool->reserveB, 100000);
        BOOST_CHECK_EQUAL(optStoredPool->totalLiquidity, 48 + CPoolPair::MINIMUM_LIQUIDITY);
    }

    {   // one limit
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(std::numeric_limits<CAmount>::max(), 1, [](CAmount liq)-> Res {
            BOOST_CHECK(liq == 3036999499); // == sqrt(limit)-MINIMUM_LIQUIDITY
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK(pool.reserveA == std::numeric_limits<CAmount>::max());
        BOOST_CHECK(pool.reserveB == 1);
        BOOST_CHECK(pool.totalLiquidity == 3036999499 + CPoolPair::MINIMUM_LIQUIDITY);

        // plus 1
        res = pool.AddLiquidity(1, 1, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg == "amounts too low, zero liquidity");

        // we can't swap forward even 1 satoshi
        res = pool.Swap(CTokenAmount{pool.idTokenA, 1}, 0, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, FAIL_onSwap);
        BOOST_CHECK(!res.ok && res.msg == "Lack of liquidity.");

        // and backward too
        res = pool.Swap(CTokenAmount{pool.idTokenB, 2}, 0, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, FAIL_onSwap);
        BOOST_CHECK(!res.ok && res.msg == "Lack of liquidity.");

        // thats all, we can't place anything here until removing. trading disabled due to reserveB < SLOPE_SWAP_RATE
    }

    {   // two limits
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(std::numeric_limits<CAmount>::max(), std::numeric_limits<CAmount>::max(), [](CAmount liq)-> Res {
            BOOST_CHECK(liq == std::numeric_limits<CAmount>::max() - CPoolPair::MINIMUM_LIQUIDITY);
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK(pool.reserveA == std::numeric_limits<CAmount>::max());
        BOOST_CHECK(pool.reserveB == std::numeric_limits<CAmount>::max());
        BOOST_CHECK(pool.totalLiquidity == std::numeric_limits<CAmount>::max());

        res = pool.AddLiquidity(1, 1, FAIL_onMint);
        BOOST_CHECK(!res.ok && res.msg.find("can't add") != res.msg.npos); // in fact we got liquidity overflows

        // thats all, we can't do anything here until removing
    }

    // trying to swap moooore than reserved on low reserves (sliding)
    // it works extremely bad on low reserves, but it's okay, just bad trade.
    {
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(1001, 1001, [](CAmount liq)-> Res {
            BOOST_CHECK(liq == 1001 - CPoolPair::MINIMUM_LIQUIDITY);
            return Res::Ok();
        });
        auto dexfeeInPct = 5 * COIN / 100;
        res = pool.Swap(CTokenAmount{pool.idTokenA, 1000000}, dexfeeInPct, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, [&] (CTokenAmount const &df, CTokenAmount const &ta) -> Res{
            auto trade = MultiplyAmounts(1000000, pool.commission);
            auto amount = 1000000 - trade;
            BOOST_CHECK_EQUAL(df.nValue, MultiplyAmounts(amount, dexfeeInPct));
            BOOST_CHECK_EQUAL(ta.nValue, 999);
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(pool.blockCommissionA, 10000);
        BOOST_CHECK_EQUAL(pool.reserveA, 941501);
        BOOST_CHECK_EQUAL(pool.reserveB, 2);
    }

    // trying to swap moooore than reserved (sliding), but on "resonable" reserves
    {
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(COIN, COIN, [](CAmount liq)-> Res {
            return Res::Ok();
        });
        auto dexfeeInPct = 1 * COIN / 100;
        res = pool.Swap(CTokenAmount{pool.idTokenA, 2*COIN}, dexfeeInPct, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, [&] (CTokenAmount const &df, CTokenAmount const &ta) -> Res{
            auto trade = MultiplyAmounts(2*COIN, pool.commission);
            auto amount = 2*COIN - trade;
            BOOST_CHECK_EQUAL(df.nValue, MultiplyAmounts(amount, dexfeeInPct));
            BOOST_CHECK_EQUAL(ta.nValue, 66218498); // pre-optimization: 66464593
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(pool.blockCommissionA, 2000000);
        BOOST_CHECK_EQUAL(pool.reserveA, 296020000);
        BOOST_CHECK_EQUAL(pool.reserveB, 33781502); // pre-optimization: 33535407
    }

    {
//        printf("2 COIN (1:1000)\n");
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(COIN, 1000*COIN, [](CAmount liq)-> Res {
            return Res::Ok();
        });
        auto dexfeeInPct = 12 * COIN / 100;
        res = pool.Swap(CTokenAmount{pool.idTokenA, 2*COIN}, dexfeeInPct, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, [&] (CTokenAmount const &df, CTokenAmount const &ta) -> Res{
            auto trade = MultiplyAmounts(2*COIN, pool.commission);
            auto amount = 2*COIN - trade;
            BOOST_CHECK_EQUAL(df.nValue, MultiplyAmounts(amount, dexfeeInPct));
            BOOST_CHECK_EQUAL(ta.nValue, 63535589264); // pre-optimization: 66465256146
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(pool.blockCommissionA, 2000000);
        BOOST_CHECK_EQUAL(pool.reserveA, 274240000);
        BOOST_CHECK_EQUAL(pool.reserveB, 36464410736); // pre-optimization: 33534743854
    }
    {
//        printf("1 COIN (1:1000)\n");
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(COIN, 1000*COIN, [](CAmount liq)-> Res {
            return Res::Ok();
        });
        res = pool.Swap(CTokenAmount{pool.idTokenA, COIN}, 0, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, [&] (CTokenAmount const &df, CTokenAmount const &ta) -> Res{
            BOOST_CHECK_EQUAL(df.nValue, 0);
            BOOST_CHECK_EQUAL(ta.nValue, 49748743718); // pre-optimization: 49773755285
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(pool.blockCommissionA, 1000000);
        BOOST_CHECK_EQUAL(pool.reserveA, 199000000);
        BOOST_CHECK_EQUAL(pool.reserveB, 50251256282); // pre-optimization: 50226244715
    }
    {
//        printf("COIN/1000 (1:1000) (no slope due to commission)\n");
        CPoolPair pool = *optPool;
        res = pool.AddLiquidity(COIN, 1000*COIN, [](CAmount liq)-> Res {
            return Res::Ok();
        });
        res = pool.Swap(CTokenAmount{pool.idTokenA, COIN/1000}, 0, PoolPrice{std::numeric_limits<CAmount>::max(), 0}, asymmetricFee, [&] (CTokenAmount const &df, CTokenAmount const &ta) -> Res{
            BOOST_CHECK_EQUAL(df.nValue, 0);
            BOOST_CHECK_EQUAL(ta.nValue, 98902086); // pre-optimization: 99000000
            return Res::Ok();
        });
        BOOST_CHECK(res.ok);
        BOOST_CHECK_EQUAL(pool.blockCommissionA, 1000);
        BOOST_CHECK_EQUAL(pool.reserveA, 100099000);
        BOOST_CHECK_EQUAL(pool.reserveB, 99901097914); // pre-optimization: 99901000000

//       printf("comissionA = %ld\n", pool.blockCommissionA);
//       printf("reserveA = %ld\n", pool.reserveA);
//       printf("reserveB = %ld\n", pool.reserveB);
    }
}

void SetPoolRewardPct(CCustomCSView & mnview, DCT_ID idPool, CAmount pct)
{
    BOOST_REQUIRE(mnview.SetRewardPct(idPool, 1, pct));
}

void SetPoolTradeFees(CCustomCSView & mnview, DCT_ID idPool, CAmount A, CAmount B)
{
    auto optPool = mnview.GetPoolPair(idPool);
    BOOST_REQUIRE(optPool);
    optPool->blockCommissionA = A;
    optPool->blockCommissionB = B;
    optPool->swapEvent = true;
    mnview.SetPoolPair(idPool, 1, *optPool);
}

BOOST_AUTO_TEST_CASE(math_rewards)
{
    const int PoolCount = 10; // less than DCT_ID_START!
    const int ProvidersCount = 10000;

    CCustomCSView mnview(*pcustomcsview);

    // create pools
    for (int i = 0; i < PoolCount; ++i) {
        DCT_ID idA, idB, idPool;
        std::tie(idA, idB, idPool) = CreatePoolNTokens(mnview, "A"+std::to_string(i), "B"+std::to_string(i));
        auto optPool = mnview.GetPoolPair(idPool);
        BOOST_REQUIRE(optPool);

    }
    // create shares
    mnview.ForEachPoolId([&] (DCT_ID const & idPool) {
//            printf("pool id = %s\n", idPool.ToString().c_str());
        for (int i = 0; i < ProvidersCount; ++i) {
            CScript shareAddress = CScript(idPool.v * ProvidersCount + i);
            Res res = AddPoolLiquidity(mnview, idPool, idPool.v*COIN, idPool.v*COIN, shareAddress);
            BOOST_CHECK(res.ok);
        }
        return true;
    });

    {
        CCustomCSView cache(mnview);

        // set pool rewards rates
        const DCT_ID RWD50 = DCT_ID{1};
        const DCT_ID RWD25 = DCT_ID{2};
        SetPoolRewardPct(cache, RWD50, COIN/2); // 50%
        SetPoolRewardPct(cache, RWD25, COIN/4); // 25%

        SetPoolRewardPct(cache, DCT_ID{3}, COIN/10); // 10%
        SetPoolRewardPct(cache, DCT_ID{4}, COIN/10); // 10%
        SetPoolRewardPct(cache, DCT_ID{5}, COIN/100);
        SetPoolRewardPct(cache, DCT_ID{6}, COIN/100);
        SetPoolRewardPct(cache, DCT_ID{7}, COIN/100);
        SetPoolRewardPct(cache, DCT_ID{8}, COIN/100);
        SetPoolRewardPct(cache, DCT_ID{9}, COIN/100);
        /// DCT_ID{10} - 0

        // set "traded fees" here too, just to estimate proc.load
        cache.ForEachPoolId([&] (DCT_ID const & idPool) {
            SetPoolTradeFees(cache, idPool, idPool.v * COIN, idPool.v * COIN*2);
            return true;
        });

        // distribute 100 coins
        CAmount totalRwd = 100*COIN*2880;
        cache.SetDailyReward(1, totalRwd);
        // fund community
        cache.AddCommunityBalance(CommunityAccountType::IncentiveFunding, totalRwd);

        // check it
        auto rwd25 = 25 * COIN / ProvidersCount;
        auto rwd50 = 50 * COIN / ProvidersCount;
        cache.ForEachPoolShare([&] (DCT_ID const & id, CScript const & owner, uint32_t) {

            cache.CalculateOwnerRewards(owner, 2); // one block
            // check only first couple of pools and the last (zero)
            if (id == RWD25 && owner != CScript(id.v * ProvidersCount)) { // first got slightly less due to MINIMUM_LIQUIDITY
                CAmount rwd = cache.GetBalance(owner, DCT_ID{0}).nValue;
                BOOST_CHECK(rwd == rwd25);
            }
            if (id == RWD50 && owner != CScript(id.v * ProvidersCount)) { // first got slightly less due to MINIMUM_LIQUIDITY
                CAmount rwd = cache.GetBalance(owner, DCT_ID{0}).nValue;
                BOOST_CHECK(rwd == rwd50);
            }
            if (id == DCT_ID{10} ) { // first got slightly less due to MINIMUM_LIQUIDITY
                CAmount rwd = cache.GetBalance(owner, DCT_ID{0}).nValue;
                BOOST_CHECK(rwd == 0);
            }
            return true;
        });

        // check trade comissions for one of pools
        {
            DCT_ID idPool = DCT_ID{1};
            auto optPool = cache.GetPoolPair(idPool);
            cache.ForEachPoolShare([&] (DCT_ID const & id, CScript const & owner, uint32_t) {
                if (id != idPool)
                    return false;

                if (owner != CScript(id.v * ProvidersCount)) { // first got slightly less due to MINIMUM_LIQUIDITY
                    CAmount rwdA = cache.GetBalance(owner, DCT_ID{optPool->idTokenA}).nValue;
                    CAmount rwdB = cache.GetBalance(owner, DCT_ID{optPool->idTokenB}).nValue;
                    BOOST_CHECK(rwdA == id.v * COIN / ProvidersCount);
                    BOOST_CHECK(rwdB == id.v * COIN * 2 / ProvidersCount);
                }
                return true;
            }, PoolShareKey{idPool, {}});
        }
    }
}

BOOST_AUTO_TEST_CASE(owner_rewards)
{
    CCustomCSView mnview(*pcustomcsview);

    constexpr const int PoolCount = 10;
    CScript shareAddress[PoolCount];

    // create pools
    for (int i = 0; i < PoolCount; ++i) {
        DCT_ID idA, idB, idPool;
        std::tie(idA, idB, idPool) = CreatePoolNTokens(mnview, "A"+std::to_string(i), "B"+std::to_string(i));
        shareAddress[i] = CScript(idPool.v * PoolCount + i);
        auto optPool = mnview.GetPoolPair(idPool);
        BOOST_REQUIRE(optPool);
    }

    // create shares
    mnview.ForEachPoolId([&] (DCT_ID const & idPool) {
        for (int i = 0; i < PoolCount; ++i) {
            Res res = AddPoolLiquidity(mnview, idPool, idPool.v*COIN, idPool.v*COIN, shareAddress[i]);
            BOOST_CHECK(res.ok);
        }
        return true;
    });

    mnview.ForEachPoolPair([&] (DCT_ID const & idPool, CPoolPair pool) {
        pool.blockCommissionA = idPool.v * COIN;
        pool.blockCommissionB = idPool.v * COIN * 2;
        pool.swapEvent = true;
        pool.ownerAddress = shareAddress[0];
        mnview.SetPoolPair(idPool, 1, pool);
        mnview.SetRewardPct(idPool, 1, COIN/(idPool.v + 1));
        return true;
    });

    mnview.SetDailyReward(3, COIN);

    auto oldRewardCalculation = [](CAmount liquidity, const CPoolPair& pool) -> CAmount {
        constexpr const uint32_t PRECISION = 10000;
        int32_t liqWeight = liquidity * PRECISION / pool.totalLiquidity;
        return COIN / 2880 * pool.rewardPct / COIN * liqWeight / PRECISION;
    };

    auto oldCommissionCalculation = [](CAmount liquidity, const CPoolPair& pool) -> std::pair<CAmount, CAmount> {
        constexpr const uint32_t PRECISION = 10000;
        int32_t liqWeight = liquidity * PRECISION / pool.totalLiquidity;
        auto feeA = pool.blockCommissionA * liqWeight / PRECISION;
        auto feeB = pool.blockCommissionB * liqWeight / PRECISION;
        return std::make_pair(feeA, feeB);
    };

    mnview.ForEachPoolPair([&] (DCT_ID const & idPool, CPoolPair pool) {
        auto onLiquidity = [&]() -> CAmount {
            return mnview.GetBalance(shareAddress[0], idPool).nValue;
        };
        mnview.CalculatePoolRewards(idPool, onLiquidity, 1, 10,
            [&](RewardType type, CTokenAmount amount, uint32_t height) {
                switch(type) {
                case RewardType::Coinbase:
                    BOOST_CHECK_EQUAL(amount.nValue, oldRewardCalculation(onLiquidity(), pool));
                    break;
                case RewardType::Commission:
                    if (amount.nTokenId == pool.idTokenA) {
                        BOOST_CHECK_EQUAL(amount.nValue, oldCommissionCalculation(onLiquidity(), pool).first);
                    } else {
                        BOOST_CHECK_EQUAL(amount.nValue, oldCommissionCalculation(onLiquidity(), pool).second);
                    }
                    break;
                default:
                    BOOST_REQUIRE(false);
                }
                BOOST_REQUIRE(mnview.AddBalance(shareAddress[0], amount).ok);
            }
        );
        return true;
    });

    // new calculation
    const_cast<int&>(Params().GetConsensus().DF4BayfrontGardensHeight) = 6;

    auto newRewardCalculation = [](CAmount liquidity, const CPoolPair& pool) -> CAmount {
        return COIN / 2880 * pool.rewardPct / COIN * liquidity / pool.totalLiquidity;
    };

    auto newCommissionCalculation = [](CAmount liquidity, const CPoolPair& pool) -> std::pair<CAmount, CAmount> {
        auto feeA = pool.blockCommissionA * liquidity / pool.totalLiquidity;
        auto feeB = pool.blockCommissionB * liquidity / pool.totalLiquidity;
        return std::make_pair(feeA, feeB);
    };

    mnview.ForEachPoolPair([&] (DCT_ID const & idPool, CPoolPair pool) {
        pool.swapEvent = true;
        pool.ownerAddress = shareAddress[1];
        pool.rewards = CBalances{TAmounts{{DCT_ID{idPool.v+1}, COIN}}};
        mnview.SetPoolPair(idPool, 8, pool);
        return false;
    });

    mnview.ForEachPoolPair([&] (DCT_ID const & idPool, CPoolPair pool) {
        auto onLiquidity = [&] () -> CAmount {
            return mnview.GetBalance(shareAddress[1], idPool).nValue;
        };
        mnview.CalculatePoolRewards(idPool, onLiquidity, 1, 10,
            [&](RewardType type, CTokenAmount amount, uint32_t height) {
                if (height >= static_cast<uint32_t>(Params().GetConsensus().DF4BayfrontGardensHeight)) {
                    if (type == RewardType::Pool) {
                        for (const auto& reward : pool.rewards.balances) {
                            auto providerReward = static_cast<CAmount>((arith_uint256(reward.second) * arith_uint256(onLiquidity()) / arith_uint256(pool.totalLiquidity)).GetLow64());
                            BOOST_CHECK_EQUAL(amount.nValue, providerReward);
                        }
                    } else if (type == RewardType::Coinbase) {
                        BOOST_CHECK_EQUAL(amount.nValue, newRewardCalculation(onLiquidity(), pool));
                    } else if (amount.nTokenId == pool.idTokenA) {
                        BOOST_CHECK_EQUAL(amount.nValue, newCommissionCalculation(onLiquidity(), pool).first);
                    } else {
                        BOOST_CHECK_EQUAL(amount.nValue, newCommissionCalculation(onLiquidity(), pool).second);
                    }
                } else {
                    if (type & RewardType::Rewards) {
                        BOOST_CHECK_EQUAL(amount.nValue, oldRewardCalculation(onLiquidity(), pool));
                    } else if (amount.nTokenId == pool.idTokenA) {
                        BOOST_CHECK_EQUAL(amount.nValue, oldCommissionCalculation(onLiquidity(), pool).first);
                    } else {
                        BOOST_CHECK_EQUAL(amount.nValue, oldCommissionCalculation(onLiquidity(), pool).second);
                    }
                }
                BOOST_REQUIRE(mnview.AddBalance(shareAddress[1], amount).ok);
            }
        );
        return false;
    });
}

namespace {

struct RewardRecord {
    uint32_t height;
    RewardType type;
    uint32_t tokenId;
    CAmount value;

    bool operator==(const RewardRecord &other) const {
        return height == other.height && type == other.type && tokenId == other.tokenId && value == other.value;
    }
};

using RewardRecords = std::vector<RewardRecord>;

// The rewards of a single height, calculated by the per-height implementation
static RewardRecords CollectPerHeightRewards(CCustomCSView &view,
                                             DCT_ID idPool,
                                             const CScript &owner,
                                             uint32_t begin,
                                             uint32_t end) {
    RewardRecords records;
    auto onLiquidity = [&]() -> CAmount { return view.GetBalance(owner, idPool).nValue; };
    view.CalculatePoolRewards(idPool, onLiquidity, begin, end, [&](RewardType type, CTokenAmount amount, uint32_t height) {
        records.push_back({height, type, amount.nTokenId.v, amount.nValue});
    });
    return records;
}

// The very same rewards, calculated by the segmented implementation
static RewardRecords CollectSegmentedRewards(CCustomCSView &view,
                                             DCT_ID idPool,
                                             const CScript &owner,
                                             uint32_t begin,
                                             uint32_t end) {
    RewardRecords records;
    auto onLiquidity = [&]() -> CAmount { return view.GetBalance(owner, idPool).nValue; };
    view.CalculatePoolRewardsSegmented(
        idPool, onLiquidity, begin, end, [&](const std::vector<PoolRewardEntry> &rewards, uint32_t height, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                for (const auto &reward : rewards) {
                    records.push_back({height + i, reward.type, reward.amount.nTokenId.v, reward.amount.nValue});
                }
            }
        });
    return records;
}

// The owner rewards as they were calculated before the segmentation: every
// single reward of every single height is credited on its own.
static void CalculateOwnerRewardsReference(CCustomCSView &view, const CScript &owner, const uint32_t targetHeight) {
    const auto balanceHeight = view.GetBalancesHeight(owner);
    if (balanceHeight >= targetHeight) {
        return;
    }
    const auto targetPerBlockHeight =
        targetHeight >= Params().GetConsensus().DF24Height ? Params().GetConsensus().DF24Height : targetHeight;

    view.ForEachPoolId([&](DCT_ID const &poolId) {
        const auto height = view.GetShare(poolId, owner);
        if (!height || *height >= targetHeight) {
            return true;
        }
        auto onLiquidity = [&]() -> CAmount { return view.GetBalance(owner, poolId).nValue; };
        const auto beginHeight = std::max(*height, balanceHeight);
        auto onReward = [&](RewardType, const CTokenAmount &amount, const uint32_t) {
            if (auto res = view.AddBalance(owner, amount); !res) {
                // the very same behaviour as the segmented implementation
            }
        };
        if (beginHeight < Params().GetConsensus().DF24Height) {
            view.CalculatePoolRewards(poolId, onLiquidity, beginHeight, targetPerBlockHeight, onReward);
        }
        return true;
    });

    view.UpdateBalancesHeight(owner, targetHeight);
}

void CompareViews(CCustomCSView &reference, CCustomCSView &segmented, const std::vector<DCT_ID> &tokens,
                  const CScript &owner) {
    for (const auto &tokenId : tokens) {
        BOOST_CHECK_EQUAL(reference.GetBalance(owner, tokenId).nValue, segmented.GetBalance(owner, tokenId).nValue);
    }
    // the raw changes have to be identical as well, they are part of the merkle root
    const auto &referenceRaw = reference.GetStorage().GetRaw();
    const auto &segmentedRaw = segmented.GetStorage().GetRaw();
    BOOST_CHECK_EQUAL(referenceRaw.size(), segmentedRaw.size());
    for (const auto &[key, value] : referenceRaw) {
        const auto it = segmentedRaw.find(key);
        BOOST_REQUIRE(it != segmentedRaw.end());
        BOOST_CHECK(value == it->second);
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(pool_rewards_segmented)
{
    CCustomCSView mnview(*pcustomcsview);

    // The reward history of a pool starts at the grand central height, so the
    // test is placed right above it. The calculation changes at the bayfront
    // gardens height, which is moved into the middle of the tested range.
    const auto grandCentralHeight = static_cast<uint32_t>(Params().GetConsensus().DF20GrandCentralHeight);
    auto &df4Height = const_cast<int&>(Params().GetConsensus().DF4BayfrontGardensHeight);
    const auto oldDf4Height = df4Height;
    const auto forkHeight = grandCentralHeight + 10;
    df4Height = forkHeight;

    constexpr const uint32_t startHeightOffset = 0;
    constexpr const uint32_t targetHeightOffset = 20;
    const auto startHeight = grandCentralHeight + startHeightOffset;
    const auto targetHeight = grandCentralHeight + targetHeightOffset;

    // ---------------------------------------------------------------------
    // a pool with a changing total liquidity (including a period without any
    // liquidity), changing rewards, a loan reward, a custom reward and swaps
    // with a commission
    // ---------------------------------------------------------------------
    DCT_ID idA, idB, idPool;
    std::tie(idA, idB, idPool) = CreatePoolNTokens(mnview, "RA", "RB");
    const CScript owner = CScript(idPool.v * 100 + 1);
    BOOST_REQUIRE(mnview.AddBalance(owner, {idPool, 500 * COIN}).ok);
    BOOST_REQUIRE(mnview.SetShare(idPool, owner, startHeight).ok);

    mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPool, startHeight}, 1000 * COIN);
    mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPool, startHeight + 3}, 0);  // no liquidity
    mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPool, startHeight + 8}, 2000 * COIN);
    mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idPool, startHeight + 1}, 100 * COIN);
    mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idPool, startHeight + 7}, 300 * COIN);
    mnview.WriteBy<CPoolPairView::ByPoolLoanReward>(PoolHeightKey{idPool, startHeight + 2}, 50 * COIN);
    mnview.WriteBy<CPoolPairView::ByCustomReward>(PoolHeightKey{idPool, startHeight + 5},
                                                 CBalances{TAmounts{{idA, 10 * COIN}}});
    // the first swap is skipped as there is no liquidity at its height
    mnview.WriteBy<CPoolPairView::ByPoolSwap>(PoolHeightKey{idPool, startHeight + 6},
                                              PoolSwapValue{true, 3 * COIN, 4 * COIN});
    // the second swap is credited with a commission
    mnview.WriteBy<CPoolPairView::ByPoolSwap>(PoolHeightKey{idPool, startHeight + 12},
                                              PoolSwapValue{true, 5 * COIN, 6 * COIN});
    mnview.Flush();

    // the per-height and the segmented implementation have to return exactly
    // the same rewards
    {
        CCustomCSView reference(*pcustomcsview);
        CCustomCSView segmented(*pcustomcsview);
        const auto referenceRewards = CollectPerHeightRewards(reference, idPool, owner, startHeight, targetHeight);
        const auto segmentedRewards = CollectSegmentedRewards(segmented, idPool, owner, startHeight, targetHeight);
        BOOST_REQUIRE(!referenceRewards.empty());
        BOOST_CHECK_EQUAL(referenceRewards.size(), segmentedRewards.size());
        for (size_t i = 0; i < referenceRewards.size() && i < segmentedRewards.size(); ++i) {
            BOOST_CHECK(referenceRewards[i] == segmentedRewards[i]);
        }
    }

    // the owner rewards have to be identical as well
    {
        CCustomCSView reference(*pcustomcsview);
        CCustomCSView segmented(*pcustomcsview);
        CalculateOwnerRewardsReference(reference, owner, targetHeight);
        segmented.CalculateOwnerRewards(owner, targetHeight);
        CompareViews(reference, segmented, {DCT_ID{0}, idA, idB, idPool}, owner);
    }

    // ---------------------------------------------------------------------
    // a custom reward which is denominated in the pool's own LP token: the
    // liquidity of the owner changes with every single reward
    // ---------------------------------------------------------------------
    {
        DCT_ID idC, idD, idPoolLp;
        std::tie(idC, idD, idPoolLp) = CreatePoolNTokens(mnview, "RC", "RD");
        const CScript lpOwner = CScript(idPoolLp.v * 100 + 2);
        BOOST_REQUIRE(mnview.AddBalance(lpOwner, {idPoolLp, 500 * COIN}).ok);
        BOOST_REQUIRE(mnview.SetShare(idPoolLp, lpOwner, startHeight).ok);

        mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPoolLp, startHeight}, 1000 * COIN);
        mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idPoolLp, startHeight}, 100 * COIN);
        mnview.WriteBy<CPoolPairView::ByCustomReward>(PoolHeightKey{idPoolLp, startHeight},
                                                      CBalances{TAmounts{{idPoolLp, 10 * COIN}}});
        mnview.Flush();

        CCustomCSView reference(*pcustomcsview);
        CCustomCSView segmented(*pcustomcsview);
        CalculateOwnerRewardsReference(reference, lpOwner, targetHeight);
        segmented.CalculateOwnerRewards(lpOwner, targetHeight);
        CompareViews(reference, segmented, {DCT_ID{0}, idC, idD, idPoolLp}, lpOwner);
    }

    // ---------------------------------------------------------------------
    // the account balance limit is reached within the segment: every single
    // reward has to be applied on its own
    // ---------------------------------------------------------------------
    {
        DCT_ID idE, idF, idPoolLimit;
        std::tie(idE, idF, idPoolLimit) = CreatePoolNTokens(mnview, "RE", "RF");
        const CScript limitOwner = CScript(idPoolLimit.v * 100 + 3);
        BOOST_REQUIRE(mnview.AddBalance(limitOwner, {idPoolLimit, 500 * COIN}).ok);
        BOOST_REQUIRE(mnview.AddBalance(limitOwner, {DCT_ID{0}, MAX_MONEY - 50 * COIN}).ok);
        BOOST_REQUIRE(mnview.SetShare(idPoolLimit, limitOwner, startHeight).ok);

        mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPoolLimit, startHeight}, 1000 * COIN);
        mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idPoolLimit, startHeight}, 100 * COIN);
        mnview.Flush();

        CCustomCSView reference(*pcustomcsview);
        CCustomCSView segmented(*pcustomcsview);
        reference.SetEnforceAccountMaxMoney(true);
        segmented.SetEnforceAccountMaxMoney(true);
        CalculateOwnerRewardsReference(reference, limitOwner, targetHeight);
        segmented.CalculateOwnerRewards(limitOwner, targetHeight);
        CompareViews(reference, segmented, {DCT_ID{0}, idE, idF, idPoolLimit}, limitOwner);
    }

    // ---------------------------------------------------------------------
    // a range without any reward and without any liquidity must not change
    // any balance at all
    // ---------------------------------------------------------------------
    {
        DCT_ID idG, idH, idPoolEmpty;
        std::tie(idG, idH, idPoolEmpty) = CreatePoolNTokens(mnview, "RG", "RH");
        const CScript emptyOwner = CScript(idPoolEmpty.v * 100 + 4);
        BOOST_REQUIRE(mnview.AddBalance(emptyOwner, {idPoolEmpty, 500 * COIN}).ok);
        BOOST_REQUIRE(mnview.SetShare(idPoolEmpty, emptyOwner, startHeight).ok);

        mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPoolEmpty, startHeight}, 0);
        mnview.Flush();

        CCustomCSView reference(*pcustomcsview);
        CCustomCSView segmented(*pcustomcsview);
        CalculateOwnerRewardsReference(reference, emptyOwner, targetHeight);
        segmented.CalculateOwnerRewards(emptyOwner, targetHeight);
        CompareViews(reference, segmented, {DCT_ID{0}, idG, idH, idPoolEmpty}, emptyOwner);
        // no balance has been changed at all, only the balances height is updated
        BOOST_CHECK_EQUAL(reference.GetStorage().GetRaw().size(), 1UL);
        BOOST_CHECK_EQUAL(segmented.GetStorage().GetRaw().size(), 1UL);
    }

    df4Height = oldDf4Height;
}

BOOST_AUTO_TEST_CASE(legacy_pool_reward_math_df25)
{
    auto &consensus = const_cast<Consensus::Params &>(Params().GetConsensus());
    const int oldDf4Height = consensus.DF4BayfrontGardensHeight;
    const int oldDf8Height = consensus.DF8EunosHeight;
    const int oldDf25Height = consensus.DF25Height;

    // Configure DF25Height < DF4BayfrontGardensHeight so the legacy branch is exercised
    // both below and at/above DF25Height.
    consensus.DF25Height = 100;
    consensus.DF4BayfrontGardensHeight = 200;
    consensus.DF8EunosHeight = 300;

    auto restore = [&]() {
        consensus.DF4BayfrontGardensHeight = oldDf4Height;
        consensus.DF8EunosHeight = oldDf8Height;
        consensus.DF25Height = oldDf25Height;
    };

    try {
        CCustomCSView mnview(*pcustomcsview);
        DCT_ID idA, idB, idPool;
        std::tie(idA, idB, idPool) = CreatePoolNTokens(mnview, "LRA", "LRB");

        const CScript provider = CScript(idPool.v * 100 + 1);
        constexpr uint32_t preHeight = 95;
        constexpr uint32_t postHeight = 105;
        constexpr CAmount totalLiq = 1000 * COIN;
        constexpr CAmount poolReward = 123456789;
        constexpr CAmount commA = 9876543;
        constexpr CAmount commB = 4567891;

        mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idPool, preHeight}, totalLiq);
        mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idPool, preHeight}, poolReward);
        mnview.WriteBy<CPoolPairView::ByPoolSwap>(PoolHeightKey{idPool, preHeight},
                                                  PoolSwapValue{true, commA, commB});
        mnview.WriteBy<CPoolPairView::ByPoolSwap>(PoolHeightKey{idPool, postHeight},
                                                  PoolSwapValue{true, commA, commB});

        // 1. Valid states (0 <= liquidity <= totalLiquidity): bit-identical below and at/above DF25Height,
        //    and identical between CalculatePoolRewards and CalculatePoolRewardsSegmented.
        for (const CAmount liq : {CAmount{0}, CAmount{1}, 333 * COIN, 500 * COIN, totalLiq}) {
            auto runCalc = [&](uint32_t h) {
                std::map<std::pair<RewardType, uint32_t>, CAmount> totals;
                mnview.CalculatePoolRewards(
                    idPool,
                    [liq]() { return liq; },
                    h,
                    h + 1,
                    [&](RewardType type, CTokenAmount amount, uint32_t) {
                        totals[{type, amount.nTokenId.v}] += amount.nValue;
                    });
                return totals;
            };
            auto runSeg = [&](uint32_t h) {
                std::map<std::pair<RewardType, uint32_t>, CAmount> totals;
                mnview.CalculatePoolRewardsSegmented(
                    idPool,
                    [liq]() { return liq; },
                    h,
                    h + 1,
                    [&](const std::vector<PoolRewardEntry> &entries, uint32_t, uint32_t count) {
                        for (const auto &entry : entries) {
                            totals[{entry.type, entry.amount.nTokenId.v}] += entry.amount.nValue * count;
                        }
                    });
                return totals;
            };

            const auto preCalc = runCalc(preHeight);
            const auto postCalc = runCalc(postHeight);
            const auto preSeg = runSeg(preHeight);
            const auto postSeg = runSeg(postHeight);

            BOOST_CHECK(preCalc == postCalc);
            BOOST_CHECK(preCalc == preSeg);
            BOOST_CHECK(postCalc == postSeg);
        }

        // 2. Overflow / uint32_t truncation repro inputs at/above DF25Height:
        //    totalLiquidity = 1, liquidity = 99999000, poolReward = 28800000000000,
        //    large blockCommissionA/B. Must not overflow signed int64 and must satisfy MoneyRange.
        {
            DCT_ID idC, idD, idCorruptPool;
            std::tie(idC, idD, idCorruptPool) = CreatePoolNTokens(mnview, "OVFA", "OVFB");

            const CScript corruptProvider = CScript(idCorruptPool.v * 100 + 2);
            constexpr CAmount corruptTotalLiq = 1;
            constexpr CAmount corruptLiq = 99999000;
            constexpr CAmount hugeReward = 28800000000000LL;
            constexpr CAmount hugeCommA = 28800000000000LL;
            constexpr CAmount smallCommB = 1000;

            mnview.WriteBy<CPoolPairView::ByTotalLiquidity>(PoolHeightKey{idCorruptPool, postHeight}, corruptTotalLiq);
            mnview.WriteBy<CPoolPairView::ByPoolReward>(PoolHeightKey{idCorruptPool, postHeight}, hugeReward);
            mnview.WriteBy<CPoolPairView::ByPoolSwap>(PoolHeightKey{idCorruptPool, postHeight},
                                                      PoolSwapValue{true, hugeCommA, smallCommB});
            BOOST_REQUIRE(mnview.SetShare(idCorruptPool, corruptProvider, postHeight).ok);

            std::vector<CTokenAmount> calcRewards;
            mnview.CalculatePoolRewards(
                idCorruptPool,
                []() { return corruptLiq; },
                postHeight,
                postHeight + 1,
                [&](RewardType, CTokenAmount amount, uint32_t) {
                    BOOST_CHECK(MoneyRange(amount.nValue));
                    calcRewards.push_back(amount);
                });

            std::vector<CTokenAmount> segRewards;
            mnview.CalculatePoolRewardsSegmented(
                idCorruptPool,
                []() { return corruptLiq; },
                postHeight,
                postHeight + 1,
                [&](const std::vector<PoolRewardEntry> &entries, uint32_t, uint32_t) {
                    for (const auto &entry : entries) {
                        BOOST_CHECK(MoneyRange(entry.amount.nValue));
                        segRewards.push_back(entry.amount);
                    }
                });

            BOOST_REQUIRE_EQUAL(calcRewards.size(), segRewards.size());
            for (size_t i = 0; i < calcRewards.size(); ++i) {
                BOOST_CHECK_EQUAL(calcRewards[i].nTokenId.v, segRewards[i].nTokenId.v);
                BOOST_CHECK_EQUAL(calcRewards[i].nValue, segRewards[i].nValue);
            }
            // hugeReward * corruptLiq > MAX_MONEY -> clamped to 0;
            // smallCommB * corruptLiq = 99999000000 -> in MoneyRange.
            BOOST_REQUIRE_EQUAL(calcRewards.size(), 2UL);
            BOOST_CHECK_EQUAL(calcRewards[0].nValue, 0);
            BOOST_CHECK_EQUAL(calcRewards[1].nValue, smallCommB * corruptLiq);

            // Also exercise UpdatePoolRewards on the legacy path with DF25 active.
            std::map<uint32_t, CAmount> transferred;
            const auto [distributed, loanDistributed] = mnview.UpdatePoolRewards(
                [&](const CScript &, DCT_ID) { return CTokenAmount{idCorruptPool, corruptLiq}; },
                [&](const CScript &, const CScript &, CTokenAmount amount) {
                    BOOST_CHECK(MoneyRange(amount.nValue));
                    transferred[amount.nTokenId.v] += amount.nValue;
                    return Res::Ok();
                },
                static_cast<int>(postHeight));
            BOOST_CHECK_EQUAL(distributed, 0);
            BOOST_CHECK_EQUAL(loanDistributed, 0);
            BOOST_CHECK_EQUAL(transferred[idD.v], smallCommB * corruptLiq);
            BOOST_CHECK_EQUAL(transferred[idC.v], 0);
        }

        // 3. UpdatePoolRewards on the modern path (height >= DF4BayfrontGardensHeight) with
        //    liquidity > totalLiquidity must not trigger the legacy assert(liqWeight < PRECISION).
        {
            constexpr uint32_t modernHeight = 205;
            constexpr CAmount modernLiq = 200;
            CAmount modernDistributed = 0;
            mnview.UpdatePoolRewards(
                [&](const CScript &, DCT_ID pid) {
                    return CTokenAmount{pid, modernLiq};
                },
                [&](const CScript &, const CScript &, CTokenAmount amount) {
                    modernDistributed += amount.nValue;
                    return Res::Ok();
                },
                static_cast<int>(modernHeight));
            // idCorruptPool has totalLiquidity = 1, poolReward = 28800000000000 ->
            // liquidityReward(28800000000000, 200, 1) = 5760000000000000.
            BOOST_CHECK_EQUAL(modernDistributed, 5760000000000000LL);
        }
    } catch (...) {
        restore();
        throw;
    }

    restore();
}

BOOST_AUTO_TEST_SUITE_END()
