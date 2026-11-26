# AMM and pools

A **liquidity pool** holds two tokens and lets anybody trade against it. The
price is not set by an order book but by the ratio of the two reserves, and
the curve that connects them is the *constant product* curve
`reserveA × reserveB = k`. This page explains the anatomy of a pool, the
exact maths of adding and removing liquidity, the LP token, and the rewards
that a liquidity provider earns.

Part of the [DEX & Liquidity](index.md).

## Anatomy of a pool

| Field | Meaning | Source |
| --- | --- | --- |
| `idTokenA`, `idTokenB` | the two tokens of the pool | `CPoolPair` in `src/dfi/poolpairs.h` |
| `reserveA`, `reserveB` | the amounts of the two tokens the pool holds | `CPoolPair` in `src/dfi/poolpairs.h` |
| `totalLiquidity` | the sum of all LP tokens in circulation for this pool | `CPoolPair` in `src/dfi/poolpairs.h` |
| `commission` | the pool commission, in units of 10⁻⁸ (10⁸ = 100 %) | `CPoolPairMessageBase` in `src/dfi/poolpairs.h` |
| `status` | `true` = trading allowed, `false` = restricted | `CPoolPairMessageBase` in `src/dfi/poolpairs.h` |
| `ownerAddress` | the address that funds the pool's custom rewards | `CPoolPair` in `src/dfi/poolpairs.h` |
| `rewards` | the custom rewards paid out to the LPs of this pool | `CPoolPair` in `src/dfi/poolpairs.h` |
| `blockCommissionA`, `blockCommissionB` | the commission collected in the current block, waiting to be paid out | `CPoolPair` in `src/dfi/poolpairs.h` |

A pool also has an **LP token**: an ordinary token with the flags `DAT`,
`LPS`, `Tradeable` and `Finalized` (`src/dfi/consensus/poolpairs.cpp`). Its
default symbol is `<tokenA>-<tokenB>` and it may be up to 16 characters long
(`CToken::MAX_TOKEN_POOLPAIR_LENGTH` in `src/dfi/tokens.h`); before
`DF11FortCanningHeight` the shorter 8-character limit applied. Holding LP
tokens *is* holding the pool: the LP token can be transferred like any other
token, and its holder owns the corresponding share of the reserves and of the
rewards.

## Creating a pool

Pool creation is a governance decision, not a user action:

* The creator needs **foundation or governance authorisation**
  (`HasGovOrFoundationAuth()` in `src/dfi/consensus/poolpairs.cpp`).
* Both tokens must already exist.
* The commission must be between 0 and `COIN` (0 … 100 %); a negative value
  or one above 100 % is rejected with *"wrong commission"*.
* From `DF16FortCanningCrunchHeight` on, the pool symbol may not contain a
  `/`.

The RPC helper is `createpoolpair`:

```text
createpoolpair '{"tokenA":"DFI","tokenB":"BTC","commission":0.003,
                 "status":true,"ownerAddress":"<address>"}'
```

The `commission` value is given in the decimal unit of a token, so `0.003`
means 0.3 % — internally `300000` units of 10⁻⁸.

A pool can be changed later with `updatepoolpair` (DfTx type `'u'`): the
status can be switched off and on, the commission and the owner can be
changed and custom rewards can be added or removed. Since
`DF23Height` the symbol and the name of the pool token may also be changed.

`updatepoolpair` requires **foundation or governance authorisation** as well
(`HasGovOrFoundationAuth()` in `src/dfi/consensus/poolpairs.cpp`). The
`ownerAddress` of a pool has no right to change the pool — it can only fund
the pool's custom rewards.

## Adding liquidity

Adding liquidity is the `AddPoolLiquidity` DfTx (type `'l'`), built by the
`addpoolliquidity` RPC. The transaction carries **both** tokens, and the
consensus code (`src/dfi/consensus/poolpairs.cpp`) requires exactly two
balances in it.

!!! note "All amounts are satoshi units"

    The node works with `CAmount` values, i.e. the smallest unit of a token:
    1 DFI is `10⁸` satoshi and 1 BTC is `10⁸` satoshi as well. All formulas on
    this page use these satoshi amounts.

### The first deposit

For an empty pool the minted liquidity is the geometric mean of the two
amounts:

```text
liquidity = √(amountA × amountB)
```

If that value is not larger than `MINIMUM_LIQUIDITY` (1000, i.e. 10⁻⁵ of a
token) the deposit is rejected with *"liquidity too low"*. Otherwise the
minted amount is reduced by the minimum liquidity:

```text
minted = √(amountA × amountB) − 1000
totalLiquidity = 1000
```

The 1000 units stay in the pool forever. They exist so that the pool can
never be emptied completely and the division by `totalLiquidity` in the
removal formula can never hit zero — the constant is a deliberate hack, see
the comment in `CPoolPair::AddLiquidity()`.

### Later deposits

For a pool that already holds liquidity, the provider's share is computed
against the current reserves:

```text
liqA = amountA × totalLiquidity / reserveA
liqB = amountB × totalLiquidity / reserveB
minted = min(liqA, liqB)
```

The provider receives the *smaller* of the two amounts, because the pool must
not be diluted: only the excess of the other token stays with the pool as
the trader's gain.

From `DF3BayfrontMarinaHeight` on, the node additionally enforces a **ratio
slippage protection** of 3 %:

```text
(max(liqA, liqB) − minted) × 100 / minted  ≥  3  →  rejected
```

with the error *"Exceeds max ratio slippage protection of 3%"*. This protects
a liquidity provider against a pool whose price moved between the moment the
transaction was built and the moment it was mined.

### Worked example

A pool is empty. A deposit of 1,000 satoshi per side (0.00001 DFI and
0.00001 BTC) is rejected, because the geometric mean is not larger than the
minimum liquidity:

```text
√(1000 × 1000) = 1000 ≤ MINIMUM_LIQUIDITY (1000)    → rejected ("liquidity too low")
```

With 1 DFI and 1 BTC, i.e. `10⁸` satoshi per side:

```text
minted = √(10⁸ × 10⁸) − 1000 = 10⁸ − 1000 = 99,999,000 satoshi (LP tokens)
reserves: 1 DFI / 1 BTC, totalLiquidity = 1000 + 99,999,000 = 100,000,000
```

A second provider deposits 0.5 DFI and 0.5 BTC, i.e. `5 × 10⁷` satoshi per
side:

```text
liqA = 5×10⁷ × 100,000,000 / 100,000,000 = 50,000,000
liqB = 5×10⁷ × 100,000,000 / 100,000,000 = 50,000,000
minted = min(50,000,000, 50,000,000) = 50,000,000 satoshi (LP tokens)
ratio slippage = (50,000,000 − 50,000,000) × 100 / 50,000,000 = 0 %   → accepted
```

After that the pool holds 1.5 DFI / 1.5 BTC with `totalLiquidity = 150,000,000`.
The second provider removes its `50,000,000` LP tokens again and gets exactly
its share back:

```text
amountA = 50,000,000 × 150,000,000 / 150,000,000 = 50,000,000 (0.5 DFI)
amountB = 50,000,000 × 150,000,000 / 150,000,000 = 50,000,000 (0.5 BTC)
```

## Removing liquidity

Removing liquidity is the `RemovePoolLiquidity` DfTx (type `'r'`), built by
`removepoolliquidity`. The LP tokens are burned and the two tokens are
returned in the exact proportion of the pool:

```text
amountA = liqAmount × reserveA / totalLiquidity
amountB = liqAmount × reserveB / totalLiquidity
```

Two constraints are enforced by `CPoolPair::RemoveLiquidity()`:

* the amount must be greater than zero and **smaller than
  `totalLiquidity`** — removing the whole share is rejected with
  *"incorrect liquidity"*, because the minimum liquidity stays locked in the
  pool, and
* the LP tokens are burned from the provider's balance before the tokens are
  returned (`SubBalanceDelShares()` in `src/dfi/consensus/poolpairs.cpp`).

!!! note "Removing liquidity is a swap with yourself"

    Because the price follows the constant-product curve, the last provider
    who leaves a pool receives slightly less than his share of the reserves:
    the value he withdraws shrinks as his share of the pool shrinks. This is
    the usual *impermanent loss* (exit fee) effect of an AMM, and it applies
    to DeFiChain pools in exactly the same way.

## How the price is computed

For a trade that puts token *F* (from) into the pool and takes token *T*
(to) out of it, the node uses the constant-product curve. From
`DF6DakotaHeight` on, the price of the pool is

```text
price = reserveF × 10⁸ / reserveT
```

(`CPoolPair::Swap()` in `src/dfi/poolpairs.cpp`). Before Dakota the formula
was inverted — the code comments this as a bug of the old version.

The **output** of a swap is computed by `CPoolPair::slopeSwap()`:

```text
swapped = reserveT − (reserveT × reserveF / (reserveF + unswapped))
```

This is the `DF4BayfrontGardensHeight` formula. Before that upgrade the node
walked the trade in chunks of `reserveF / SLOPE_SWAP_RATE` (1000) units,
which produced a slightly different (rounder) result for large trades. From
`DF14FortCanningHillHeight` on, one unit is subtracted from the result
("*floor the result*"), so the trader never receives a fraction of a unit
more than the curve allows.

A pool whose reserves fall below `SLOPE_SWAP_RATE` (1000) on either side
cannot be traded: the swap is rejected with *"Lack of liquidity."*.

## The rewards of a liquidity provider

A provider earns four different kinds of rewards, all distributed
proportionally to the liquidity share (`CalculatePoolRewards()` in
`src/dfi/poolpairs.cpp`):

| Reward | Source | Paid in |
| --- | --- | --- |
| `Coinbase` | the share of the block subsidy that governance assigned to this pool (`LP_DAILY_DFI_REWARD`, `LP_SPLITS`) | DFI |
| `LoanTokenDEXReward` | the DEX reward assigned to the pool for loan-token activity (`LOAN_SPLITS`) | DFI |
| `Commission` | the trading commission collected in the block of the swap | the token that was traded |
| `Pool` | the custom rewards configured for the pool | the configured reward token |

The **commission** deserves a special mention: it is not burned and it is
not paid to the pool owner. It is collected from the trader in the block of
the swap (`blockCommissionA` / `blockCommissionB`) and paid out to the
liquidity providers of that pool in the same block, in proportion to their
share.

The **custom pool rewards** are the one exception: they are paid *from* the
balance of the pool owner address to the liquidity providers
(`onTransfer(ownerAddress, provider, …)` in `UpdatePoolRewards()`). The pool
owner funds them, and a custom reward whose owner balance is too small is
dropped from the pool for that block.

## The numbers at a glance

| Parameter | Value | Source |
| --- | --- | --- |
| Minimum liquidity permanently locked per pool | `1000` satoshi units (10⁻⁵ of a token) | `CPoolPair::MINIMUM_LIQUIDITY` in `src/dfi/poolpairs.h` |
| Minimum reserve for trading | `1000` per side | `CPoolPair::SLOPE_SWAP_RATE` in `src/dfi/poolpairs.h` |
| Price precision | `10⁸` | `CPoolPair::PRECISION` in `src/dfi/poolpairs.h` |
| First-deposit liquidity | `√(amountA × amountB) − 1000` | `CPoolPair::AddLiquidity()` in `src/dfi/poolpairs.cpp` |
| Later-deposit liquidity | `min(liqA, liqB)` | `CPoolPair::AddLiquidity()` in `src/dfi/poolpairs.cpp` |
| Ratio slippage protection | 3 % (from `DF3BayfrontMarinaHeight`) | `CPoolPair::AddLiquidity()` in `src/dfi/poolpairs.cpp` |
| Removal formula | proportional to `reserve / totalLiquidity` | `CPoolPair::RemoveLiquidity()` in `src/dfi/poolpairs.cpp` |
| Pool token flags | `DAT`, `LPS`, `Tradeable`, `Finalized` | `src/dfi/consensus/poolpairs.cpp` |
| Pool token symbol | `<tokenA>-<tokenB>`, max. 16 characters | `CToken::MAX_TOKEN_POOLPAIR_LENGTH` in `src/dfi/tokens.h` |

## What is where

| Topic | Lives in |
| --- | --- |
| The custom transactions of this section | [DVM](../dvm/index.md) |
| How the LP token is defined and how tokens are minted | [Tokens and DAT](../dvm/tokens-and-dat.md) |
| Swap execution, price limits and the DEX fee | [Swapping and fees](swapping-and-fees.md) |
| The governance variables that set the pool rewards | [Governance](../governance/on-chain-governance.md) |
| The pool implementation | [Developer & Architecture Manual](../../developer-manual/index.md) |
