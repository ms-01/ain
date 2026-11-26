# Masternodes

Registering a masternode, running it as an operator node and checking that it
actually mints.

Part of the [Node Software Manual](../index.md).

## The two roles

A masternode consists of two independent roles:

| Role | What it holds | What it does |
| --- | --- | --- |
| **Owner** | The collateral — the locked tokens — in the owner address. | Owns the stake. The collateral is locked and cannot be spent while the masternode is active. |
| **Operator** | The operator key. | Signs the proof-of-stake blocks. The operator key may be held on a different machine than the owner. |

One node can play both roles. Splitting them is the normal setup: the owner
key stays on a cold machine, the operator key lives on the machine that is
online 24/7.

DeFiChain is a proof-of-stake chain. A masternode does not "mine" with a
hash power — it competes for the right to produce a block with a
proof-of-stake kernel, and it receives the staking reward and the anchor
payments for the transactions it confirms.

## Collateral and fees

The amounts are defined in the chain parameters
(`consensus.mn.collateralAmount`, `consensus.mn.collateralAmountDakota` and
`consensus.mn.creationFee` in `src/chainparams.cpp`):

| Item | Mainnet, testnet, changi and devnet | Regtest |
| --- | --- | --- |
| Collateral (since the Dakota hard fork) | 20,000 DFI | 2 DFI |
| Collateral (before Dakota) | 1,000,000 DFI | 10 DFI |
| Creation fee (burned) | 10 DFI | 1 DFI |

The collateral is a plain token balance in the owner address. It is locked by
the masternode creation transaction and released when the masternode is
resigned — after the resign delay (see below) — or, for a masternode with a
timelock, after the timelock period.

!!! note
    The 10 DFI creation fee is burned, not paid to anybody. It is paid with
    the same transaction that creates the masternode.

## Choosing the addresses

Both addresses must be addresses of the account (UTXO) model:

* The **owner address** may be a P2PKH or a P2WPKH address. It holds the
  collateral.
* The **operator address** may be a P2PKH or a P2WPKH address as well, and it
  must be unique — one operator address can only ever be the operator of one
  masternode.

Create the addresses in a dedicated wallet so that the keys are easy to back
up and to move:

```bash
# 1. Create a wallet for the masternode.
./defi-cli createwallet "mn-owner"

# 2. Create the owner address (holds the collateral).
./defi-cli -rpcwallet=mn-owner getnewaddress "mn-owner" legacy

# 3. Create the operator address (signs the blocks).
./defi-cli -rpcwallet=mn-owner getnewaddress "mn-operator" legacy
```

Keep the seed of that wallet offline once the addresses are created. The
operator key is the only key that has to be online; the owner key should stay
cold.

## Registering the masternode

### 1. Fund the owner address

The owner address needs at least the collateral plus the creation fee:

```bash
./defi-cli -rpcwallet=mn-owner getbalances
./defi-cli sendtoaddress <owner-address> 20010
```

### 2. Create the masternode

```bash
./defi-cli -rpcwallet=mn-owner createmasternode <owner-address> <operator-address>
```

The command creates and broadcasts the masternode creation transaction and
returns the transaction id. The operator address is optional and defaults to
the owner address — only use that if the same node plays both roles.

The node refuses the command while the initial block download is still
running (`Cannot create Masternode while still in Initial Block Download`), so
wait until the node is synchronised.

The transaction spends the collateral and the fee in one output each. The
optional third argument selects the UTXOs to spend:

```bash
./defi-cli -rpcwallet=mn-owner createmasternode <owner-address> <operator-address> \
    '[{"txid":"<txid>","vout":1}]'
```

### 3. Wait for the activation delay

A newly created masternode is not active immediately. The activation delay is
`consensus.mn.newActivationDelay` in `src/chainparams.cpp`:

| Network | Activation delay | At 30 seconds per block |
| --- | --- | --- |
| mainnet, testnet, changi, devnet | 1008 blocks | about 8 hours 24 minutes |
| regtest | 20 blocks | about 10 minutes |

During that window the masternode is in the state `PRE_ENABLED`. After the
delay it is `ENABLED` and starts competing for blocks.

## Timelocks

A masternode can be created with a timelock. The collateral then cannot be
spent for the whole period, but the masternode receives a higher staking
power:

| Timelock | Argument | Staking power | Collateral locked |
| --- | --- | --- | --- |
| none | *(omitted)* | 1.0x | until the masternode is resigned |
| five years | `FIVEYEARTIMELOCK` | 1.5x | 260 weeks (5 years) |
| ten years | `TENYEARTIMELOCK` | 2.0x | 520 weeks (10 years) |

```bash
./defi-cli -rpcwallet=mn-owner createmasternode <owner-address> <operator-address> '[]' FIVEYEARTIMELOCK
```

The fourth argument carries the timelock; the third argument is the (empty)
array of UTXOs to spend. The values are the enum
`CMasternode::TimeLock` (`ZEROYEAR`, `FIVEYEAR = 260`, `TENYEAR = 520`) in
`src/dfi/masternodes.h`; the node maps them to the strings
`FIVEYEARTIMELOCK` and `TENYEARTIMELOCK`.

!!! warning
    A timelocked masternode cannot be resigned before the period is over. The
    collateral stays locked even if the operator machine is lost. Use a
    timelock only if you are certain that you will not want the funds back
    earlier.

The timelock of an existing masternode is visible in `listmasternodes` as
`"timelock": "5 years"` and the staking power as `targetMultipliers`.

## Configuring the operator node

The node that mints has to know which operator address it is responsible for.
Set `-masternode_operator` in `defi.conf`:

```text
masternode_operator=<operator-address>
```

The option can be given several times if one node operates several
masternodes. The node then looks for the operator key in its wallets and uses
it to sign the proof-of-stake blocks.

The operator key has to be spendable in one of the wallets of that node:

```bash
# Import the operator key into the wallet of the operator node.
./defi-cli -rpcwallet=mn-operator importprivkey "<operator-private-key>" \
    "mn-operator" false

# Check that the node knows the address.
./defi-cli -rpcwallet=mn-operator getaddressinfo <operator-address> | grep -E 'ismine|iswatchonly'
```

Optionally, set the address that receives the staking rewards and the anchor
payments:

```text
rewardaddress=<reward-address>
```

Without `-rewardaddress`, the reward goes to the owner address of the
masternode.

!!! note
    Setting `-masternode_operator` switches the node to level-DB checksum
    mode; the node logs `parameter interaction: -masternode_operator ->
    setting -leveldbchecksum='true'` at startup.

Restart the node after changing the configuration:

```bash
sudo systemctl restart defid.service
```

## Checking that the masternode works

### State and metadata

```bash
# All masternodes with their full metadata.
./defi-cli listmasternodes

# Only the ones of this node.
./defi-cli listmasternodes | grep -i "<operator-address>"

# One masternode, addressed by its ID, owner or operator address.
./defi-cli getmasternode <mn-id>
```

The verbose output of `listmasternodes` contains:

| Field | Meaning |
| --- | --- |
| `ownerAuthAddress` | The address that holds the collateral. |
| `operatorAuthAddress` | The address that signs the blocks. |
| `rewardAddress` | The address that receives the rewards. |
| `creationHeight` | The height of the creation transaction. |
| `resignHeight` | The height of the resign transaction, `0` while active. |
| `collateralTx` | The transaction that locked the collateral. |
| `state` | `PRE_ENABLED`, `ENABLED`, `PRE_RESIGNED`, `RESIGNED` or `TRANSFERRING`. |
| `mintedBlocks` | The number of blocks this masternode has produced. |
| `ownerIsMine` / `operatorIsMine` | Whether the wallet of the calling node owns the key. |
| `localMasternode` | Whether this node is the operator of the masternode. |
| `timelock` | The timelock period, if one was set. |
| `targetMultipliers` | The staking power of the masternode. |

`localMasternode: true` is the quickest check that the configuration of the
operator node is correct — it means that the node found the masternode for
the operator address given in `-masternode_operator`.

### Minting checks

```bash
# How many blocks did this masternode produce?
# The first argument is an object with exactly one identifier:
# "id", "ownerAddress" or "operatorAddress".
./defi-cli getmasternodeblocks '{"operatorAddress":"<operator-address>"}'

# How many masternodes are active in the given block range?
./defi-cli getactivemasternodecount 100
```

A masternode that is `ENABLED` but never mints is a problem. The usual causes
are:

| Symptom | Cause | Fix |
| --- | --- | --- |
| `localMasternode: false` | `-masternode_operator` is missing or misspelled, or the node was not restarted. | Fix the configuration and restart the node. |
| `operatorIsMine: false` on the operator node | The operator key is not in any wallet of that node. | Import the key with `importprivkey`. |
| The state stays `PRE_ENABLED` | The activation delay has not passed yet. | Wait for the delay (1008 blocks on mainnet). |
| The state is `ENABLED` but `mintedBlocks` stays 0 | The operator key does not match the registered operator address, or the node has no reward destination. | Compare `operatorAuthAddress` with the configured address. |
| Minting stops after an upgrade | The node runs a version that does not satisfy the masternode version gate. | Update the node software; see below. |

### The version gate

Masternodes have to signal a specific version from the masternode version gate
height onwards. On mainnet that height is `6564035`
(`consensus.nMNVersionGateHeight` in `src/chainparams.cpp`, overridable with
`-mnversiongateheight`). A masternode that stays on an old version stops
minting and eventually is deactivated. When you upgrade the node software,
check that the new version signals the required bit.

## The anchors

The masternodes of a network form *anchor teams*. An anchor team confirms the
transactions of the regular chain and receives the anchor payments. The
membership changes with the active masternode set:

```bash
# The anchor teams at a given block height.
./defi-cli getanchorteams <block-height>

# The anchors of the current height.
./defi-cli listanchors
```

## Updating and resigning

### Changing the addresses

```bash
./defi-cli -rpcwallet=mn-owner updatemasternode <mn-id> \
    '{"operatorAddress":"<new-operator-address>","rewardAddress":"<new-reward-address>"}'
```

The transaction can change the owner address, the operator address and the
reward address. Changing the owner address requires a new collateral fee; the
operator and the reward address can be changed without one. The transaction
has to be signed by the current owner address.

After an operator change, update `-masternode_operator` on the new operator
node and restart it.

### Resigning

```bash
./defi-cli -rpcwallet=mn-owner resignmasternode <mn-id>
```

The masternode moves to `PRE_RESIGNED` and then to `RESIGNED`. The collateral
is unlocked after the resign delay
(`consensus.mn.newResignDelay = 2 × newActivationDelay`, i.e. 2016 blocks —
about 16 hours 48 minutes — on mainnet).

!!! warning
    A masternode with a timelock cannot be resigned before the timelock
    period is over.

## Next steps

* [Operations](index.md) — running the operator node as a service and
  monitoring it.
* [Backups and signers](backups-signers.md) — protecting the owner and
  operator keys.
* [Staking and masternodes](../../user-manual/staking-masternodes/index.md) —
  the protocol side of staking, rewards and timelocks.
