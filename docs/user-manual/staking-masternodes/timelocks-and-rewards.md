# Timelocks and rewards

A masternode can be created with a **timelock**: a period during which the
masternode cannot be resigned. In exchange the masternode gets more staking
power and therefore a higher chance to produce blocks. This page explains the
timelock tiers, how the staking weight is calculated, and how the block
subsidy is split.

Part of the [Staking & Masternodes](index.md).

## The timelock tiers

| Timelock | On-chain value | Minimum lifetime | Competing subnodes | Staking power |
| --- | --- | --- | --- | --- |
| none | `0` (`ZEROYEAR`) | until resignation | 2 | baseline |
| 5 years | `260` (`FIVEYEAR`) | 260 weeks ≈ 5 years | 3 | +50 % |
| 10 years | `520` (`TENYEAR`) | 520 weeks = 10 years | 4 | +100 % |

The values are defined in `CMasternode::TimeLock` (`src/dfi/masternodes.h`).
They are expressed in **weeks** (52 weeks per year), and the same unit is used
when the expiry is evaluated (`GetTimelock()` in
`src/dfi/masternodes.cpp`).

The number of competing subnodes is derived from the timelock by
`GetTimelockLoops()` in `src/dfi/masternodes.cpp`:

| Timelock | Loops |
| --- | --- |
| `TENYEAR` (520) | 4 |
| `FIVEYEAR` (260) | 3 |
| `ZEROYEAR` (0) | 2 |

The `createmasternode` RPC describes the same tiers as “1.5x or 2.0 the
staking power” for the five- and ten-year timelock
(`src/dfi/rpc_masternodes.cpp`).

### Before EunosPaya

Timelocks were introduced with the EunosPaya upgrade. Before
`consensus.DF10EunosPayaHeight` (mainnet block 1,072,000, 5 August 2021):

* a creation transaction with a timelock is rejected (*"Timelock must be set
  to either 0, 5 or 10 years"* / *"collateral timelock cannot be set below
  EunosPaya"*), and
* `GetTimelockLoops()` returns `1`, so every masternode had a single staking
  identity.

### How the expiry works

The expiry is evaluated against the block time of the masternode's creation
block: `creationTime + timelock × 7 × 24 × 60 × 60` seconds. The current block
time is approximated by the average time of the last `resignDelay` blocks
(`GetTimelock()` in `src/dfi/masternodes.cpp`).

Once the timelock has expired, `GetTimelock()` returns `0`: the masternode
behaves like a masternode without a timelock. It can be resigned again, and
it drops back to two competing subnodes.

!!! note "Expiry does not unlock the collateral"

    An expired timelock only removes the *block on resignation*. The
    collateral stays locked until the masternode is actually resigned and the
    resignation delay has passed. Always resign a masternode you no longer
    want to operate.

## Staking weight: the coin-day weight

The probability of producing the next block is not equal for all masternodes.
It depends on the *coin-day weight* of the subnode that is competing
(`CalcCoinDayWeight()` in `src/pos_kernel.cpp`):

```text
weight = (min(now − lastBlockTimeOfSubnode, 14 days) + 6 hours) / 6 hours
```

* `consensus.pos.nStakeMaxAge` is 14 days — the weight saturates at that age.
* The 6-hour period (`6 × 60 × 60` seconds) is the granularity of the weight.
* The result is an integer between **1 and 57** (`14 days / 6 hours + 1`), and
  it is multiplied into the proof-of-stake target when the kernel is checked
  (`CheckKernelHash()` in `src/pos_kernel.cpp`).

The last block time of a subnode is reset when that subnode produces a block.
So a masternode that has just produced a block has to wait before it can
produce the next one, while a masternode that has been idle for a long time
has the maximum weight. The current weight of each subnode is visible in the
`targetMultiplier` field of `getblocktemplate` and in the masternode RPC
output (`src/rpc/mining.cpp`, `src/dfi/rpc_masternodes.cpp`).

The number of subnodes that compete at the same time is the loop count of the
timelock, so a ten-year timelock competes with four weighted identities
instead of two.

## The block subsidy

The block subsidy is the amount of DFI that is created with every block. On
mainnet:

* Before the Eunos upgrade (`consensus.DF8EunosHeight`, block 894,000,
  3 June 2021) the subsidy was `consensus.baseBlockSubsidy` = 200 DFI.
* Since Eunos it is `consensus.newBaseBlockSubsidy` = 405.04 DFI, reduced by
  `consensus.emissionReductionAmount` = 1.658 % every
  `consensus.emissionReductionPeriod` = 32,690 blocks (labelled “two weeks” in
  the chain parameters).

The reduction is applied per elapsed reduction period
(`GetBlockSubsidy()` in `src/validation.cpp`).

## How the subsidy is split

The subsidy of a block is divided between the block producer and the
community accounts. The shares are defined in `Consensus::CoinbaseDistribution`
(`src/consensus/params.h`) and set per network in `src/chainparams.cpp`:

| Recipient | Share | Where it goes |
| --- | --- | --- |
| Masternode (block producer) | 33.33 % (`dist.masternode`) | Coinbase output of the block, to the reward address or the owner address |
| Liquidity mining (incentive funding) | 25.45 % (`dist.liquidity`) | Community account `IncentiveFunding` |
| Loans | 24.68 % (`dist.loan`) | Community account `Loan` |
| Options | 9.88 % (`dist.options`) | Community account `Options` |
| Community development funds | 4.91 % (`dist.community`) | Community account `CommunityDevelopmentFunds` |
| Unallocated | 1.73 % (`dist.unallocated`) | Community account `Burnt` |
| Anchor reward | 0.02 % (`dist.anchor`) | Community account `AnchorReward`, paid out by an anchor reward transaction |

With the initial subsidy of 405.04 DFI, the masternode share is roughly
**135 DFI per block** plus the transaction fees collected in that block
(`src/miner.cpp`). The masternode share is calculated with
`CalculateCoinbaseReward(blockReward, consensus.dist.masternode)`.

The community shares are credited to the community accounts when the block is
connected (`src/validation.cpp`). The `Unallocated` pot is the *burnt* pot:
`GetCommunityAccountName()` in `src/dfi/incentivefunding.h` maps
`CommunityAccountType::Unallocated` to `"Burnt"`.

Two historical details are worth knowing when reading older block explorers:

* Until the Grand Central upgrade (`consensus.DF20GrandCentralHeight`, mainnet
  block 2,479,000, 8 December 2022) the community development share was paid
  to a foundation address in the coinbase instead of a community account.
* Since the FortCanning upgrade (`consensus.DF11FortCanningHeight`, mainnet
  block 1,367,000, 15 November 2021) the coinbase output contains the
  masternode share **plus** the transaction fees of the block.

## Where the reward is paid

When a masternode wins a block, the coinbase output is paid to the first
valid destination of this list (`src/miner.cpp`):

1. the **reward address** of the masternode, if one is set,
2. otherwise the `-rewardaddress` option of the mining node, if it is set,
3. otherwise the **owner address** of the masternode.

The operator address is not part of this list: the operator earns nothing
unless the owner sets a reward address or the node is started with
`-rewardaddress`.

The reward is a coinbase output and can only be spent after
`COINBASE_MATURITY` = 100 blocks (≈ 50 minutes at the target block time of
30 seconds).

## Related pages

* [Masternode basics](masternode-basics.md) — roles, collateral and the
  lifecycle states.
* [Staking and minting](staking-and-minting.md) — the block production loop.
* [Governance](../governance/index.md) — the other job of a masternode:
  voting on proposals.
