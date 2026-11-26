# Mempool and transaction acceptance

The mempool holds the transactions that have been accepted but not yet mined.
This page describes how a transaction enters the pool, how it is replaced or
evicted, and how the pool is coupled to the height-gated consensus rules of
the DVM and EVM layers.

Part of the [Subsystems section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The behaviour described here is implemented in `src/txmempool.{h,cpp}`,
    `src/mempool_accept.{h,cpp}` and `src/policy/`. The policy constants live
    in `src/policy/policy.h` and `src/validation.h`.

## Components

| Component | Location | Responsibility |
| --- | --- | --- |
| `CTxMemPool` | `src/txmempool.h`, `src/txmempool.cpp` | The pool itself: the transaction map, the ancestor/descendant bookkeeping, the account view, expiry and eviction. |
| `CTxMemPoolEntry` | `src/txmempool.h` | One entry: the transaction, the fee, the weight, the entry time and height, the ancestor/descendant counters and the EVM bookkeeping (`evmAddressAndNonce`, `evmRbfMinTipFee`). The per-sender indices are held by `CTxMemPool` itself. |
| `MemPoolAccept` | `src/mempool_accept.h`, `src/mempool_accept.cpp` | The acceptance pipeline: `PreChecks`, `ReplacementChecks`, `PolicyScriptChecks`, `ConsensusScriptChecks`, `EvmChecks`, `Finalize`. |
| `AcceptToMemoryPool()` / `AcceptToMemoryPoolWithTime()` | `src/mempool_accept.cpp` | The entry points used by the RPC layer and by the P2P layer. |
| `ProcessNewPackage()` / `AcceptToMemoryPoolPackage()` | `src/mempool_accept.cpp`, `src/policy/packages.cpp` | Atomic acceptance of transaction packages (1P1C). |
| `CBlockPolicyEstimator` | `src/policy/fees.cpp`, `src/policy/fees.h` | Estimates the fee rate needed for inclusion within *n* blocks. |
| RBF / TRUC policy | `src/policy/rbf.cpp`, `src/policy/truc_policy.cpp` | Replacement rules for UTXO transactions. |
| `CCoinsViewMemPool` | `src/txmempool.h` | A coins view that also sees the unconfirmed outputs of the pool. |
| Mempool persistence | `src/node/mempool_persist.cpp` | Dumps and reloads the pool across a restart (`-persistmempool`). |
| Orphan transactions | `src/node/txorphanage.cpp` | Holds transactions whose parents are not known yet. |

## The acceptance pipeline

`MemPoolAccept::AcceptSingleTransaction()` runs the checks in this order.
Every step may reject the transaction with a `CValidationState` reason that
is forwarded to the caller (and, for P2P submissions, mapped to a `Reject`
message).

### 1. `PreChecks()`

`MemPoolAccept::PreChecks()` (`src/mempool_accept.cpp`) performs the
context-free and context-dependent checks:

* `CheckTransaction()` — the static sanity of the transaction.
* A coinbase transaction is rejected: it is only valid inside a block.
* Standardness: `IsStandardTx()` is enforced when `fRequireStandard` is set
  (the default on mainnet). Scripts that carry an Ethereum-style payload are
  rejected with the reason `eth-scriptpubkey`.
* Minimum size (`MIN_STANDARD_TX_NONWITNESS_SIZE`) and finality
  (`CheckFinalTx()`) are checked, so the pool never fills up with
  transactions that cannot be mined in the next block.
* The transaction must not already be in the pool
  (`txn-already-in-mempool`).
* Conflicts with in-memory transactions are detected. A transaction may opt
  out of replacement by setting `nSequence` above
  `MAX_BIP125_RBF_SEQUENCE` on *all* inputs; otherwise the conflict is
  handed to `ReplacementChecks()`.
* The inputs must exist. Missing inputs are reported via `pfMissingInputs`,
  which is the signal for the P2P layer to keep the transaction as an orphan
  instead of rejecting it.
* The fee must cover the inputs (`bad-txns-inputs-below-tx-fee`), the
  transaction must not be below the minimum relay fee and must not exceed the
  absurd fee limit.
* BIP68 sequence locks are evaluated (`non-BIP68-final`) and the ancestor and
  descendant limits (`-limitancestorcount`, `-limitancestorsize`,
  `-limitdescendantcount`, `-limitdescendantsize`) are enforced.
* Transactions that spend a coinbase are tracked, so that a reorg can
  re-check `COINBASE_MATURITY`.

### 2. `ReplacementChecks()`

`MemPoolAccept::ReplacementChecks()` decides whether an incoming
transaction may replace a conflicting one that is already in the pool:

* UTXO transactions follow the BIP125 rules of `src/policy/rbf.cpp` and the
  TRUC rules of `src/policy/truc_policy.cpp` (a TRUC transaction may only be
  replaced by a transaction with the same inputs and a higher fee rate).
* `UseExactRbfFeeIncrement()` (`src/mempool_accept.cpp`) switches the
  required fee increment to the exact increment when DF25 is active
  (`Consensus::IsDF25Active()`); before that the BIP125 rule applies.
* EVM transactions have their own replacement path: they are tracked per
  sender in `evmTxsBySender` and `evmReplaceByFeeBySender`
  (`src/txmempool.h`), and the fee bump is evaluated in `EvmChecks()`.

### 3. `PolicyScriptChecks()` and `ConsensusScriptChecks()`

Both steps run the script interpreter, but with different flag sets:

* `MemPoolAccept::PolicyScriptChecks()` uses the fixed policy flag set
  `STANDARD_SCRIPT_VERIFY_FLAGS` (`src/mempool_accept.cpp`) — the standard
  relay rules, which are stricter than the consensus rules in some aspects
  (e.g. `SCRIPT_VERIFY_CLEANSTACK`).
* `MemPoolAccept::ConsensusScriptChecks()` derives the flags from the chain
  tip with `GetBlockScriptFlags(::ChainActive().Tip(), consensus)` and stores
  them in `currentBlockScriptVerifyFlags`, then calls
  `CheckInputsFromMempoolAndCache()` (`src/mempool_accept.cpp`), which uses
  the pool's own view of the unspent outputs. This second pass catches
  transactions that are standard but invalid under the rules of the current
  block (for example after a soft fork changed a flag).

The flag names are defined in `src/script/interpreter.h` (e.g.
`SCRIPT_VERIFY_P2SH`, `SCRIPT_VERIFY_CLEANSTACK`,
`SCRIPT_VERIFY_WITNESS`); there is no separate "policy" or "consensus" flag
constant — the two runs differ in the *set* of flags that is assembled.

### 4. `EvmChecks()`

`MemPoolAccept::EvmChecks()` handles the EVM transactions
(`src/mempool_accept.cpp`):

* The sender address and the nonce are checked against the account state.
* The replacement-by-fee bookkeeping of the sender is updated
  (`CTxMemPool::checkAddressNonceAndFee()` and
  `CTxMemPool::IncrementEvmRbfCount()`).
* The EVM transactions are not subject to the Bitcoin-style input check:
  `Consensus::CheckTxInputs()` returns early for them, so a DF25 rule change
  cannot turn an already accepted EVM transaction into an invalid one.

### 5. `Finalize()`

`MemPoolAccept::Finalize()` inserts the entry with
`CTxMemPool::addUnchecked()`. From that point on the pool maintains the
ancestor and descendant counters, the fee deltas of
`PrioritiseTransaction()` and, for EVM transactions, the per-sender indices.

## Packages

`ProcessNewPackage()` (`src/mempool_accept.cpp`) accepts a package of
transactions atomically:

* Only 1-parent-1-child packages are supported.
* The package is validated transaction by transaction with the same checks as
  the single transaction path; the outputs of an already validated
  transaction are available to the next one through the package coins view.
* A low-fee parent can be funded by its child (package CPFP); the combined
  package feerate is evaluated against the mempool minimum fee and the
  minimum relay fee.
* EVM transactions and DfTx (custom transactions) are rejected before any
  per-transaction validation, because they need the account view and the EVM
  nonce bookkeeping, which are not available during package validation.
* The submission is all-or-nothing: if one transaction cannot be submitted,
  the already submitted ones are removed again.

## The account view

The pool keeps its own `CCustomCSView` (`acview` in `src/txmempool.h`) so
that DVM balances of unconfirmed transactions are visible without touching
the chain state:

* `CTxMemPool::rebuildAccountsView()` rebuilds the view from the chain state
  plus the pool transactions, for the height of the *next* block
  (`accountsViewHeight`).
* `CTxMemPool::setAccountViewDirty()` and `getAccountViewDirty()` track
  whether the view is stale.
* `CTxMemPool::resetAccountsView()` drops the view, for example after a
  reorg.

## Height-gated rules: `MempoolRulesChangedBetween()`

`MempoolRulesChangedBetween()` (`src/txmempool.cpp`) is called from
`CTxMemPool::removeForBlock()`, i.e. whenever a block is connected. It
returns `true` when a rule that is relevant for the mempool changes between
the height the account view was built with and the height of the next block:

```cpp
static bool MempoolRulesChangedBetween(int fromHeight, int toHeight) {
    const auto &consensus = Params().GetConsensus();
    return HeightCrossed(consensus.nBalanceOverflowHeight, fromHeight, toHeight) ||
           HeightCrossed(consensus.DF25Height, fromHeight, toHeight);
}
```

When it returns `true`, the account view is marked dirty and rebuilt, even
if no transaction was removed from the pool. This keeps the pool consistent
with the rules of the next block. The DF25 gas cost changes are *not*
acceptance rules for EVM transactions (see the comment above the function),
so they only trigger a rebuild.

See [Upgrade gating](../consensus/upgrade-gating.md) for the general pattern
and [Hardfork timeline](../consensus/hardforks.md) for the heights.

## Expiry, size and eviction

| Setting | Meaning |
| --- | --- |
| `-mempoolexpiry=<hours>` | Maximum age of a DVM transaction in the pool. Default: `DEFAULT_MEMPOOL_DVM_EXPIRY` (336 hours). |
| `-mempoolexpiryevm=<hours>` | Maximum age of an EVM transaction in the pool. Default: `DEFAULT_MEMPOOL_EVM_EXPIRY` (3 hours). |
| `-maxmempool=<MB>` | Memory budget of the pool. Default: `DEFAULT_MAX_MEMPOOL_SIZE` (300 MB). |

The helpers `MempoolExpirySeconds()`, `MempoolSizeBytes()` and
`IsMempoolTimeExpired()` (`src/txmempool.h`) convert the operator supplied
values with saturating arithmetic, so a corrupted value cannot cause an
overflow. `IsMempoolEntryExpired()` combines both windows: an EVM
transaction is only expired once the DVM *and* the EVM window have passed.

When the pool exceeds its budget, `CTxMemPool::TrimToSize()` evicts the
transactions with the lowest fee rate. The eviction threshold is the rolling
minimum fee rate (`rollingMinimumFeeRate`, `CTxMemPool::GetMinFee()`), which
is recomputed when a block is connected (`lastRollingFeeUpdate`,
`blockSinceLastRollingFeeBump`).

## Fee policy

* The minimum relay fee is `DEFAULT_MIN_RELAY_TX_FEE` (`src/validation.h`)
  and can be changed with `-minrelaytxfee`.
* `CBlockPolicyEstimator` (`src/policy/fees.cpp`) groups transactions into
  fee buckets and tracks how long they stay in a block. The result is the
  `estimatesmartfee` RPC and the `-feefilter` announcement to the peers.
* `-incrementalrelayfee` defines the cost of relay that a replacement has to
  pay on top of the replaced transaction.

## Persistence and orphans

* With `-persistmempool` the pool is written to `<datadir>/mempool.dat` on
  shutdown (`DumpMempool()`) and loaded on startup (`LoadMempool()` in
  `src/node/mempool_persist.cpp`).
* Transactions whose parents are unknown are kept in the orphan pool
  (`src/node/txorphanage.cpp`), bounded by `-maxorphantx`
  (`DEFAULT_MAX_ORPHAN_TRANSACTIONS` = 100).

## Debugging

| Tool | Purpose |
| --- | --- |
| `src/rpc/blockchain.cpp` | `getrawmempool`, `getmempoolinfo`, `getmempoolentry`, `getmempooldescendants`, `getmempoolancestors`, `savemempool`. |
| `src/rpc/mempool.cpp` | `gettxspendingprevout` and `submitpackage`. |
| `src/test/mempool_tests.cpp` | Unit tests of the pool and of the acceptance path. |
| `src/test/mempool_persist_tests.cpp` | Unit tests of the mempool persistence. |
| `CTxMemPool::xcheck()` | Consistency check of the pool against the coins view. |

## Related pages

* [P2P networking and IBD](p2p-and-ibd.md) — how transactions reach the pool
  and how they are relayed.
* [PoS validation and masternodes](pos-validation.md) — the block producer
  side of the pool.
* [State storage](state-storage.md) — where the account state of the pool
  view comes from.
* [RPC and gRPC layer](rpc-and-grpc.md) — the `sendrawtransaction` path.
* [Codebase map](../codebase-map/index.md) — where the files above live.
