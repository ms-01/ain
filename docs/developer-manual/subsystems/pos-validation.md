# PoS validation and masternodes

DeFiChain is a Proof-of-Stake chain: the blocks are produced by masternodes
that lock a collateral and are selected through a stake kernel. This page
describes how a masternode is registered, how its state evolves, how the
kernel and the signatures of a block are validated, and how the difficulty
is retargeted.

Part of the [Subsystems section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The PoS rules live in `src/pos.{h,cpp}` and `src/pos_kernel.{h,cpp}`,
    the masternode state machine in `src/dfi/masternodes.{h,cpp}`, the DfTx
    checks in `src/dfi/mn_checks.{h,cpp}` and `src/dfi/consensus/masternodes.cpp`
    and the anchor system in `src/dfi/anchors.cpp`.

## The masternode

A masternode is an identity in the chain state that owns a collateral, an
operator key and an owner authorisation key. The class is `CMasternode`
(`src/dfi/masternodes.h`).

| Property | Meaning |
| --- | --- |
| `ownerAuthAddress` | The address that authorised the creation and that controls the node. |
| `operatorAuthAddress` | The address that actually mints the blocks (the "operator"). |
| `creationHeight` | The height at which the node was created. |
| `version` | `CMasternode::VERSION0` since Fort Canning, `PRE_FORT_CANNING` before. |
| `state` | Derived state, see below. |

### States

`CMasternode::GetState()` (`src/dfi/masternodes.cpp`) derives the state of a
node from the chain state and the current height:

| State | Meaning |
| --- | --- |
| `PRE_ENABLED` | The node is registered but the activation delay has not passed yet. |
| `ENABLED` | The node is active and may mint blocks. |
| `PRE_RESIGNED` | The node has resigned; the resignation delay has not passed yet. |
| `RESIGNED` | The node is no longer active. |
| `TRANSFERRING` | The ownership of the node is being transferred to a new owner. |

`CMasternode::IsActive()` is the predicate used by the validation code: only
an active node may mint a block.

### Timelocks

`CMasternode::TimeLock` (`src/dfi/masternodes.h`) defines the collateral
lock periods:

```cpp
enum TimeLock : uint16_t { ZEROYEAR, FIVEYEAR = 260, TENYEAR = 520 };
```

The values are week counts (52 weeks per year): `FIVEYEAR` is 260 weeks and
`TENYEAR` is 520 weeks. The timelock of an existing node is read from the
chain state with `CMasternodesView::GetTimelock()`, which converts it into
an expiry time. Since Eunos Paya (`DF10EunosPayaHeight`) a new node may
only be created with one of these three timelocks.

The number of stake kernel search iterations depends on the timelock:
`GetTimelockLoops()` (`src/dfi/masternodes.cpp`) returns 2 loops for
`ZEROYEAR`, 3 for `FIVEYEAR` and 4 for `TENYEAR` (and 1 before the Eunos
Paya fork).

### Delays and fees

| Function | Meaning |
| --- | --- |
| `GetMnActivationDelay(height)` | The number of blocks between the creation and the activation of a node. |
| `GetMnResignDelay(height)` | The number of blocks between the resignation and the end of the node's activity. |
| `GetMnCollateralAmount(height)` | The collateral amount required for a node. |
| `GetMnCreationFee(height)` | The creation fee of a node. |

The delays are network parameters (`consensus.mn.activationDelay`,
`consensus.mn.newActivationDelay`, `consensus.mn.resignDelay`,
`consensus.mn.newResignDelay` in `src/consensus/params.h`) and they changed
with Eunos (`DF8EunosHeight`).

## The masternode view

`CMasternodesView` (`src/dfi/masternodes.h`) is the read/write view on the
masternode part of the custom chain state. It is based on
`CFlushableStorageKV` (see [State storage](state-storage.md)) and offers the
operations used by the validation code:

* `CreateMasternode()`, `ResignMasternode()`,
  `UpdateMasternodeOwner()`, `UpdateMasternodeOperator()` and
  `UpdateMasternodeCollateral()`,
* `GetMasternode()`, `GetMasternodeIdByOperator()`,
  `GetMasternodeIdByOwner()` and `ForEachMasternode()`,
* `SetMasternodeLastBlockTime()`, `GetMasternodeLastBlockTime()` and the
  sub-node bookkeeping (`SetSubNodesBlockTime()`,
  `GetSubNodesBlockTime()`),
* the collateral auctions of the foundation and the team
  (`CFoundationsDebtView`, `CTeamView`).

`CCustomCSView` (`src/dfi/masternodes.h`) derives from `CMasternodesView` and
adds the accounts, tokens, pool pairs, loans, vaults, oracles and governance
state; it is the view that the DVM consensus rules work on.

## The validation of a PoS block

The PoS validation is split into a header-level part (which is executed
during the header sync) and a full-block part.

### Header level

`src/pos.cpp`:

| Function | Check |
| --- | --- |
| `pos::CheckStakeModifier()` | The stake modifier of the header equals the modifier computed from the previous block and the minter key. |
| `pos::CheckHeaderSignature()` | The header signature recovers against the minter key of the coinstake output. The genesis block is exempt. |

!!! warning "Header-level checks do not prove stake ownership"

    A header-only fork can be forged by a peer that owns a single key pair.
    This is the reason for the anchored header-tip selection of
    [ADR-0001](../adr/ADR-0001-pos-chainwork-header-tip-selection.md), see
    [P2P networking and IBD](p2p-and-ibd.md).

### Block level

`pos::ContextualCheckProofOfStake()` (`src/pos.cpp`) is executed for a full
block:

1. The minter key is extracted from the header and the masternode is looked
   up with `CMasternodesView::GetMasternodeIdByOperator()`.
2. The node must exist and must be active (`CMasternode::IsActive()`).
3. The timelock of the node is read (from `DF10EunosPayaHeight` on) and the
   sub-node block times are collected with
   `CMasternodesView::GetBlockTimes()`.
4. The stake kernel is verified: the number of loops is derived from the
   timelock (`GetTimelockLoops()`) and `CheckKernelHash()`
   (`src/pos_kernel.cpp`) is called for every sub-node until one matches.
   The kernel hash is computed with `CalcKernelHash()` /
   `CalcKernelHashMulti()` from the stake modifier, the height, the
   coinstake time and the masternode ID.
5. The header signature is checked.

`pos::CheckProofOfStake()` combines the stake modifier check and the
contextual check; it is the entry point used by the block validation.

### Difficulty

`pos::GetNextWorkRequired()` and `pos::CalculateNextWorkRequired()`
(`src/pos.cpp`) implement the retargeting. The parameters are in
`Consensus::Params::PoS` (`src/consensus/params.h`): `diffLimit`,
`nTargetTimespan`, `nTargetTimespanV2`, `nTargetSpacing`, `nStakeMinAge`,
`nStakeMaxAge`, `fAllowMinDifficultyBlocks` and `fNoRetargeting`.

!!! note "The retarget window is clamped"

    `CalculateNextWorkRequired()` clamps the actual timespan to
    `[nTargetTimespan / 4, 4 * nTargetTimespan]`. A peer that spaces its
    forged headers one second apart can therefore inflate the claimed work
    by a factor of four per retarget period. This is the reason why the
    header tip is selected by the anchored height and not by the raw
    chainwork (ADR-0001).

### Signing a block

`pos::SignPosBlock()` and `pos::CheckSignedBlock()` (`src/pos.cpp`) sign and
verify a block with the operator key; they are used by the masternode
tooling and by the tests.

## The DfTx side of the masternode rules

The registration of a masternode is a DfTx (custom transaction), so it is
also subject to the generic custom transaction rules.

| File | Responsibility |
| --- | --- |
| `src/dfi/mn_checks.cpp` | The checks of the masternode messages (`CCreateMasterNodeMessage`, `CResignMasterNodeMessage`, …), the collateral checks and the XVM/EVM coinbase payload (`XVM`, `EVM` in `src/dfi/mn_checks.h`). |
| `src/dfi/consensus/masternodes.cpp` | `CMasternodesConsensus`, the consensus visitor of the masternode messages: the creation fee and the collateral amount (`CheckMasternodeCreationTx()`), the owner authorisation from Eunos on, and the timelock rules from Eunos Paya on. |
| `src/dfi/customtx.{h,cpp}` | The serialisation and the type codes of the custom transactions. |

The generic custom transaction handling is in `src/dfi/customtx.cpp` and the
per-domain consensus visitors are in `src/dfi/consensus/`.

## Anchors

The anchor system connects the DeFiChain to the Bitcoin chain: an anchor is a
Bitcoin block that is voted on by the masternodes.

| File | Responsibility |
| --- | --- |
| `src/dfi/anchors.cpp` | `CAnchor`, `CAnchorAuthMessage` and `CAnchorAuthIndex`: the creation of an anchor, the validation of the authorisation signatures (`CheckAuthSigs()`), the quorum (`GetMinAnchorQuorum()`) and the anchor subsidy (`GetAnchorSubsidy()`). |
| `src/spv/` | The SPV proof handling of the Bitcoin side. |
| `src/dfi/anchors.h` | The anchor data structures. |

The anchor data is exchanged over the P2P protocol with the `getauths`,
`anchorauth` and `anchorconf` messages (`src/protocol.cpp`).

## Masternode version gate

Since `nMNVersionGateHeight` the blocks must signal the balance overflow fix
in `block.nVersion` with the version bit `VERSION_BIT_BALANCE_OVERFLOW`
(`1 << 24`). The check is implemented in `src/validation.cpp`
(`ComputeBlockVersion()` and `ContextualCheckBlockHeader()`), and the peers
that do not signal it are disconnected by
`PeerLogicValidation::CheckForMasternodeVersionGateStall()`
(`src/net_processing.cpp`). See the
[hardfork timeline](../consensus/hardforks.md).

## Operator interfaces

| Interface | Location |
| --- | --- |
| `protx` / `mnctl` | The masternode tooling (external tools that talk to the node over the RPC). |
| `src/dfi/rpc_masternodes.cpp` | `createmasternode`, `resignmasternode`, `updatemasternode`, `listmasternodes`, `getmasternode`, `getmasternodeblocks`, `getactivemasternodecount`, `listanchors`. |
| `src/dfi/mn_rpc.cpp` | The masternode RPC helpers shared with the other modules (`src/dfi/mn_rpc.h`). |
| `src/test/pos_tests.cpp` | The unit tests of the PoS rules. |
| `src/test/mn_timelock_tests.cpp`, `src/test/mn_blocktime_tests.cpp` | The unit tests of the timelocks and of the block time bookkeeping. |

## Related pages

* [P2P networking and IBD](p2p-and-ibd.md) — how the blocks are
  downloaded and validated.
* [Mempool and transaction acceptance](mempool.md) — the transactions that
  a masternode includes in its block.
* [State storage](state-storage.md) — where the masternode state is
  persisted.
* [Hardfork timeline](../consensus/hardforks.md) — the activation heights of
  the masternode rule changes.
* [Staking and masternodes](../../user-manual/staking-masternodes/index.md) —
  the user-facing description of the same rules.
