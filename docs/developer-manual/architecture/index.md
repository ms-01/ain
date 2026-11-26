# Architecture

High-level system design of the DeFiChain node and the boundaries between the
C++ core, the native DVM and the Rust MetaChain EVM.

Part of the [Developer & Architecture Manual](../index.md).

## Contents

| Document | What it covers |
| --- | --- |
| [Architecture overview](overview.md) | The two execution layers, the block production and validation flow, state storage and the RPC entry points. |
| [C++ ↔ Rust bridge](cxx-bridge.md) | The `cxx` FFI boundary: `lib/ain-cpp-imports`, `lib/ain-rs-exports`, `src/ffi/` and the block-template lifecycle. |

## The one-paragraph version

The node is a single binary (`defid`) that hosts two execution layers which
advance in lockstep:

* the **native layer (DVM)**, a Bitcoin-derived UTXO chain extended with
  account balances, tokens, custom transactions and masternode proof of stake,
  implemented in C++20 under `src/`, with the DeFiChain-specific parts in
  `src/dfi/`;
* the **MetaChain EVM**, an Ethereum-compatible execution environment written
  in Rust under `lib/ain-evm/`, which runs *inside* the DVM block production:
  every DVM block has exactly one EVM block, committed together with it.

The two layers talk to each other through a narrow `cxx` FFI surface
(`lib/ain-cpp-imports` for Rust → C++ calls, `lib/ain-rs-exports` for
C++ → Rust calls, and the C++ implementations in `src/ffi/`). A third,
read-only component — the **Ocean** indexer (`lib/ain-ocean/`) — serves a REST
API over the chain data for explorers and wallets.

## Where to start

* Never seen the node? Read the [architecture overview](overview.md) first.
* Coming from the Rust side? Read the
  [C++ ↔ Rust bridge](cxx-bridge.md) before touching any `cxx::bridge` code.
* Looking for a specific file? Use the [codebase map](../codebase-map/index.md).
