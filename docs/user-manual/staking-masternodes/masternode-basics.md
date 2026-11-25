# Masternode basics

A **masternode** is the only entity that can produce a block on DeFiChain and
the only entity that can earn block rewards. This page explains the roles
involved, what a masternode costs, and the states a masternode moves through.

Part of the [Staking & Masternodes](index.md).

## Roles: owner, operator, reward address

A masternode is identified by three addresses. They are stored in the
`CMasternode` structure (`src/dfi/masternodes.h`).

| Role | Field | What it does |
| --- | --- | --- |
| **Owner** | `ownerAuthAddress` | Owns the masternode and the locked collateral. The owner authorises the creation, the resignation and the updates of the masternode. The owner address is also the address that holds the collateral output. |
| **Operator** | `operatorAuthAddress` | Runs the node that participates in the block lottery. Only the operator can sign blocks for this masternode. Defaults to the owner address if no separate operator is given. |
| **Reward address** | `rewardAddress` | Optional. The address that receives the block reward. If it is not set, the reward goes to the owner address. |

Three properties follow from this separation:

* The **operator cannot move the collateral**. The collateral output is locked
  in the owner's transaction and only the owner can authorise its release.
* The **operator does not automatically receive a share of the reward**. If you
  pay somebody to operate your masternode, you have to set a reward address
  explicitly if you want to share the income.
* Owner and operator must be **unique**. A masternode whose owner or operator
  address already exists is rejected (`src/dfi/consensus/masternodes.cpp`).

All three addresses must be P2PKH or P2WPKH (legacy or native SegWit). Since
the Metachain activation (`consensus.DF22MetachainHeight`) the reward address
may additionally be a P2SH address.

## Collateral and fees

| Item | Amount | When it is paid |
| --- | --- | --- |
| Collateral | 20,000 DFI (mainnet) | Locked in the creation transaction, released on resignation |
| Creation fee | 10 DFI | Burned in the creation transaction |

* The collateral amount is height dependent:
  `GetMnCollateralAmount()` in `src/dfi/masternodes.cpp` returns
  `consensus.mn.collateralAmountDakota` (20,000 DFI on mainnet) from the
  Dakota activation height (`consensus.DF6DakotaHeight`, mainnet block
  678,000, 1 March 2021) onwards and `consensus.mn.collateralAmount`
  (1,000,000 DFI) before it.
* The creation fee is `consensus.mn.creationFee` (10 DFI) and is paid in
  addition to the collateral. It is not returned when the masternode is
  resigned.
* The creation transaction has exactly two outputs: the creation fee and the
  collateral (`CheckMasternodeCreationTx()` in
  `src/dfi/consensus/masternodes.cpp`).

The collateral is **not** a regular balance: it is a dedicated transaction
output (output 1 of the creation transaction) that the masternode code locks
until the masternode is resigned. A transaction that tries to spend it while
the masternode exists is rejected with *"tried to spend locked collateral"*
(`CCustomCSView::CanSpend()` in `src/dfi/masternodes.cpp`, enforced in
`src/consensus/tx_verify.cpp`). Until the masternode reaches the state
`RESIGNED`, the collateral can therefore not be used in a liquidity pool, as
vault collateral or as loan collateral.

## Creating a masternode

Creation is the `CreateMasternode` custom transaction (type code `C`, see
[Custom transactions](../dvm/custom-transactions.md)). The RPC wrapper
`createmasternode` builds it for you:

```text
defi-cli createmasternode "<ownerAddress>" "<operatorAddress>" '[]' "<timelock>"
```

* `ownerAddress` — the address that keeps the collateral. It must be owned by
  the wallet that submits the transaction.
* `operatorAddress` — optional, defaults to the owner address.
* `inputs` — optional list of UTXOs to spend.
* `timelock` — optional, see [Timelocks and rewards](timelocks-and-rewards.md).

The masternode identifier is the transaction id of the creation transaction
(`mnview.CreateMasternode(tx.GetHash(), …)`), so the masternode exists as soon
as the creation transaction is confirmed.

### The activation delay

A freshly created masternode is in the state `PRE_ENABLED` and may not produce
blocks yet. It becomes `ENABLED` after the activation delay, which is
`consensus.mn.newActivationDelay` (1,008 blocks ≈ 8.4 hours on mainnet) from
the creation height. The delay exists so that the masternode is known to the
whole network before it starts competing for blocks.

## The lifecycle states

`CMasternode::GetState()` in `src/dfi/masternodes.cpp` derives the state of a
masternode from its creation height, its resign height and the pending
collateral entries:

| State | Meaning | Active? |
| --- | --- | --- |
| `PRE_ENABLED` | Creation confirmed, activation delay not yet over | No |
| `ENABLED` | Fully active, may produce blocks | Yes |
| `PRE_RESIGNED` | Resignation confirmed, resignation delay not yet over | No |
| `RESIGNED` | Resignation delay over, collateral released | No |
| `TRANSFERRING` | An owner change is pending, the masternode waits for the new collateral | No |
| `UNKNOWN` | Not reachable in a consistent state | No |

`CMasternode::IsActive()` is the predicate the consensus code uses when it
decides whether a masternode may sign a block. Since the EunosPaya upgrade
only the state `ENABLED` counts as active; before that upgrade a
`PRE_RESIGNED` masternode could still produce blocks
(`src/dfi/masternodes.cpp`).

## Resigning a masternode

Resignation is the `ResignMasternode` custom transaction (type code `R`):

```text
defi-cli resignmasternode "<mn_id>" '[]'
```

* The masternode moves to `PRE_RESIGNED` immediately and stops producing
  blocks.
* After the resignation delay — `consensus.mn.newResignDelay`
  (2,016 blocks ≈ 16.8 hours on mainnet) — the masternode becomes `RESIGNED`
  and the collateral output is released back to the owner address.
* A masternode with an active timelock **cannot** be resigned: the
  transaction fails with *"Trying to resign masternode before timelock
  expiration."* (`ResignMasternode()` in `src/dfi/masternodes.cpp`). The
  timelock runs out on its own, see
  [Timelocks and rewards](timelocks-and-rewards.md).

Because the collateral is only returned after the resignation delay, a
masternode that is stopped without being resigned keeps its collateral locked
indefinitely.

## Updating a masternode

The `UpdateMasternode` custom transaction (type code `m`) changes the
metadata of an existing masternode. The RPC wrapper is `updatemasternode`.
Three update types exist (`UpdateMasternodeType` in
`src/dfi/masternodes.h`):

| Update | Effect | Extra requirement |
| --- | --- | --- |
| `OwnerAddress` | Moves the masternode to a new owner address; the old collateral output is spent and a new collateral output is created at the new owner address. The masternode is `TRANSFERRING` until the new collateral is activated, then `PRE_ENABLED` again. | Feature attribute `v0/params/feature/mn-setowneraddress` must be enabled |
| `OperatorAddress` | Hands the masternode to a different operator without touching the collateral. | Feature attribute `v0/params/feature/mn-setoperatoraddress` must be enabled |
| `SetRewardAddress` / `RemRewardAddress` | Sets or removes the address that receives the block reward. | Feature attribute `v0/params/feature/mn-setrewardaddress` must be enabled |

The feature attributes are on-chain governance variables, so the community
decides whether these updates are available. An update is only accepted while
the masternode is in the state `ENABLED`.

An owner change is not instantaneous. The new collateral output becomes the
collateral of the masternode after the resignation delay
(`src/dfi/validation.cpp`); until then the masternode is `TRANSFERRING` and
does not produce blocks. Afterwards it is `PRE_ENABLED` for one activation
delay and only then `ENABLED` again.

## Subnodes

A masternode does not stake with a single identity but with up to **four
subnodes** (`SUBNODE_COUNT` in `src/dfi/masternodes.h`). Each subnode keeps
its own "last block time", which is the reference point for its staking
weight. The number of subnodes that actually compete is derived from the
timelock:

| Timelock | Competing subnodes ("loops") |
| --- | --- |
| none (0 years) | 2 |
| 5 years | 3 |
| 10 years | 4 |

`GetTimelockLoops()` in `src/dfi/masternodes.cpp` is the single source of
truth for this mapping. The details are in
[Timelocks and rewards](timelocks-and-rewards.md).

## Related pages

* [Timelocks and rewards](timelocks-and-rewards.md) — the timelock tiers, the
  staking weight and the reward split.
* [Staking and minting](staking-and-minting.md) — how a masternode produces a
  block and what an operator has to do.
* [Operations](../../node-manual/operations/index.md) — running the node
  behind a masternode.
