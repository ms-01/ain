# Accounts and gas

This page explains the account model of the EVM layer, the address formats you
will meet, and exactly how gas and fees work.

## The EVM account model

An EVM account has the four classic Ethereum fields:

| Field | Meaning |
| --- | --- |
| `nonce` | Number of transactions sent from the account; part of the replay protection. |
| `balance` | The amount of DFI the account holds on the EVM layer. |
| `code` | The deployed contract bytecode, empty for a plain account. |
| `storage` | The key/value store of a contract. |

The balance is denominated in **wei** with 18 decimals, the same convention as
Ethereum. The conversion to the DVM unit is fixed:

```text
1 satoshi = 1e-8 DFI = 10 gwei = 1e10 wei
```

(`WEI_TO_SATS` in `lib/ain-evm/src/weiamount.rs`.) So an EVM account that shows
`1000000000000000000` wei holds exactly `0.1 DFI`.

A DFI balance only reaches the EVM layer if it was bridged there with a
[TransferDomain](transfer-domain.md) transaction, or if it was earned there as
a priority fee. DFI that stays in the DVM account store is invisible to the
EVM.

## Address formats

The EVM uses the **ERC-55** form of an address: `0x` followed by 40 hex
characters, where the letter case encodes a checksum
([EIP-55](https://eips.ethereum.org/EIPS/eip-55)). The node derives it from the
public key: it takes the Ethereum address identifier of the key and renders it
with the checksum (`WitnessV16EthHash` in `src/script/standard.h`,
`DestinationEncoder::operator()` in `src/key_io.cpp`).

The important consequence is that **one key has one identity on both layers**:

| Form | Example shape | Layer |
| --- | --- | --- |
| `erc55` | `0x482e975Ee029d6d268CC1dCce529748a06A46AAc` | EVM account, and the mapped DVM account of the same key |
| `bech32` | `df1…` (mainnet), `tf1…` (test networks) | DVM account (witness v0 key hash) |
| `legacy` | Base58Check, starts with `8` on mainnet | DVM account (public key hash) |

The node maps between them when it checks the authorisation of a transaction:
`HasAuth()` with `AuthStrategy::Mapped` accepts a legacy public key hash
output or a bech32 witness output as authorisation for the `erc55` address of
the same key (`src/dfi/consensus/txvisitor.cpp`, flags
`AuthFlags::PKHashInSource` and `AuthFlags::Bech32InSource`).

See [Wallets and addresses](../getting-started/wallets-and-addresses.md) for
the full picture.

## Gas

Gas is the unit that measures computation in the EVM. Every transaction pays
for two things: an **intrinsic** cost that depends only on the shape of the
transaction, and the **execution** cost of the code it runs.

### Intrinsic gas

The node checks the gas limit of a transaction against the intrinsic cost
before it enters the mempool (`check_tx_intrinsic_gas()` in
`lib/ain-evm/src/gas.rs`, using the `shanghai` fork configuration):

| Component | Gas |
| --- | --- |
| Plain call transaction | `21,000` |
| Contract creation | `53,000` |
| Calldata, per zero byte | `4` |
| Calldata, per non-zero byte | `16` |
| Access list, per address | `2,400` |
| Access list, per storage key | `1,900` |
| Init code, per 32-byte word (EIP-3860) | `2` |

A transaction whose gas limit is below the intrinsic cost is rejected with
`gas limit is below the minimum gas per tx`.

### Execution gas

The execution cost follows the Ethereum rules of the `shanghai` fork. The most
frequently encountered numbers are:

| Operation | Gas |
| --- | --- |
| `SSTORE`, slot set from zero | `20,000` |
| `SSTORE`, slot reset | `2,900` |
| `SLOAD`, warm slot | `100` |
| `SLOAD`, cold slot | `2,100` |
| `CALL`, base cost | `2,600` for a cold target account (`100` when the account is already warm) |

Refunds follow EIP-3529: clearing a storage slot refunds `4,800` gas, and at
most one fifth of the gas used by the transaction can be refunded.

### Block gas limit

The gas that may be used by one EVM block is a governance parameter:

```text
v0/evm/block/gas_limit          default 30,000,000
v0/evm/block/gas_target_factor  default 2
```

The **gas target** of a block is `gas_limit / gas_target_factor`, i.e. 15
million gas with the defaults. A transaction may not use more gas than the
block limit (`lib/ain-evm/src/core.rs`).

## Fees

Fees follow the EIP-1559 model.

```text
effective gas price = base fee + priority fee (tip)
fee paid           = gas used x effective gas price
```

* The **base fee** is computed from the parent block: it moves by at most
  1/8 of the parent base fee per block towards the gas target, it is never
  below the initial base fee of `10 gwei` and never above the maximum money
  amount (`calculate_base_fee()` in `lib/ain-evm/src/block.rs`).
* The **priority fee** is what the transaction author offers above the base
  fee. It is paid to the **beneficiary of the EVM block**, which is the
  masternode that produced the DVM block.
* The **base fee is burnt** — it is removed from the supply.

Both parts are tracked in the live attributes:

```text
v0/live/economy/evm/block/fee_burnt     total burnt base fees
v0/live/economy/evm/block/fee_priority  total priority fees paid to masternodes
```

(minimum and maximum values per block are tracked next to them.)

### Transaction types and gas prices

The EVM accepts the three standard Ethereum transaction envelopes
(`CEVMTxType` in `src/dfi/evm.h`):

| Type | Envelope | Gas price field |
| --- | --- | --- |
| Legacy | `0x00` | `gasPrice` |
| EIP-2930 (typed, access list) | `0x01` | `gasPrice` |
| EIP-1559 (typed) | `0x02` | `maxFeePerGas` / `maxPriorityFeePerGas` |

For legacy and EIP-2930 transactions the tip is `gasPrice - baseFee`; for
EIP-1559 transactions it is `maxPriorityFeePerGas`. Use `eth_gasPrice` and
`eth_maxPriorityFeePerGas` to ask the node for a suggestion — the suggested tip
is the 60th percentile of the tips of the last 20 blocks
(`suggest_priority_fee()` in `lib/ain-evm/src/block.rs`, configurable with
`-evmtxpriorityfeepercentile`).

### Replacing a transaction

To replace a pending transaction in the mempool, the new transaction has to
increase the tip fee by at least the configured percentage:

```text
v0/evm/block/rbf_increment_fee_pct   default 10
```

From the DF25 activation height on, the stored value is applied as an exact
fraction of 100 % scaled by `1e8`; below that height the value is truncated to
whole percentage points (`calculate_min_rbf_tip_gas_fee()` in
`lib/ain-evm/src/fee.rs`).

### Estimating the gas limit

`eth_estimateGas` runs the transaction against the current state and binary
searches for the smallest gas limit that still succeeds. The search stops when
the remaining range is below the configured error ratio, 15 % by default
(`-evmestimategaserrorratio`).

## Finality

The EVM does not have its own finality mechanism: an EVM block is final once
the DVM block that contains it is final. The `safe` and `finalized` block tags
of the ETH RPC point to the block that is `finality_count` (default `100`)
EVM blocks behind the tip (`lib/ain-grpc/src/rpc/common.rs`).

## Practical checklist

1. Bridge DFI to the EVM first — the EVM cannot spend DFI it does not hold.
2. Ask `eth_estimateGas` for the gas limit instead of guessing; the intrinsic
   cost alone is `21,000` gas for a plain transfer.
3. Pay a tip: transactions that only offer the base fee can wait.
4. Watch `v0/live/economy/evm/block/fee_burnt` and `fee_priority` to see what
   the EVM layer costs.

## Related pages

* [MetaChain overview](metachain-overview.md) — how the EVM block is produced.
* [TransferDomain](transfer-domain.md) — getting DFI onto the EVM layer.
* [DST20 bridging](dst20-bridging.md) — tokens on the EVM.
