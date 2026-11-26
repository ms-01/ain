# Developer & Architecture Manual

The **Developer & Architecture Manual** is the reference for people who work on
the DeFiChain node itself: core contributors, reviewers of pull requests and
engineers who need to understand how the two execution layers of the node fit
together.

It is part of the canonical documentation suite defined in
[ADR-0002](adr/ADR-0002-documentation-structure.md).

## Who this manual is for

| Audience | Start with |
| --- | --- |
| New contributor | [Codebase map](codebase-map/index.md), then [Architecture overview](architecture/overview.md). |
| DVM (native layer) developer | [Codebase map](codebase-map/index.md) → `src/dfi/`, then [Consensus](consensus/index.md). |
| EVM / Rust developer | [C++ ↔ Rust bridge](architecture/cxx-bridge.md), then [Codebase map](codebase-map/index.md) → `lib/ain-*`. |
| Consensus reviewer | [Consensus](consensus/index.md) and [Subsystems](subsystems/index.md). |
| Build / CI maintainer | [Building & Testing](building-testing/index.md). |

The manual is **explanation and reference**: it describes how the node is put
together and where things live. Task-oriented how-to material for *running* a
node belongs to the [Node Software Manual](../node-manual/index.md), and
protocol rules for *users* belong to the
[User & Protocol Manual](../user-manual/index.md).

## Sections

| Section | What it covers |
| --- | --- |
| [Architecture](architecture/index.md) | The dual-layer execution model (C++20/DVM and Rust/MetaChain EVM), the block production and validation flow, and the `cxx` bridge between the two languages. |
| [Codebase Map](codebase-map/index.md) | Where to find what: repository directories, the `src/dfi/` modules, the Cargo workspace crates and the test trees. |
| [Consensus](consensus/index.md) | Consensus rules, the hardfork timeline and activation-height gating such as `DF25Height`. |
| [Subsystems](subsystems/index.md) | Mempool, P2P and IBD, PoS validation, state storage and the RPC layer. |
| [Building & Testing](building-testing/index.md) | Build system, C++/Rust/Python test suites, fuzzing and linting. |
| [Architecture Decision Records](adr/index.md) | The MADR workflow and the ADRs that record the structural decisions of the node. |

## How the manual is written

* **Every statement is verifiable.** Protocol and implementation claims name
  the file that implements them, for example `src/dfi/consensus/xvm.h` or
  `lib/ain-rs-exports/src/lib.rs`. If a claim and the code disagree, the code
  wins — please open an issue.
* **Two layers, one node.** The native layer (DVM) is written in C++20, the
  MetaChain EVM is written in Rust. Anything that crosses the boundary is
  described in [C++ ↔ Rust bridge](architecture/cxx-bridge.md).
* **Heights are named, not numbered.** Activation points are referred to by
  their consensus parameter, for example `DF25Height`, and their concrete
  values per network live in `src/chainparams.cpp`.
* **Diagrams are Mermaid.** All diagrams in this manual are Mermaid blocks so
  they stay reviewable as text.

## Reading paths

### "I want to make my first change"

1. [Codebase map](codebase-map/index.md) — learn where the code lives.
2. [Architecture overview](architecture/overview.md) — learn how a block is
   produced and validated.
3. [Building & Testing](building-testing/index.md) — build the node and run
   the test suites.

### "I want to change a DVM consensus rule"

1. [Codebase map](codebase-map/index.md) → `src/dfi/consensus/`.
2. [Consensus](consensus/index.md) — the activation-height gating pattern and
   the required mempool coupling.
3. [Subsystems](subsystems/index.md) — mempool acceptance and re-evaluation.

### "I want to touch the EVM"

1. [C++ ↔ Rust bridge](architecture/cxx-bridge.md) — how the two languages
   call each other and what the conventions are.
2. [Codebase map](codebase-map/index.md) → `lib/ain-evm/`, `lib/ain-grpc/`,
   `lib/ain-ocean/`.
3. [Subsystems](subsystems/index.md) — the RPC and state storage layers.

## Conventions

* Code references such as `src/dfi/customtx.cpp` are paths in this
  repository and are written in backticks.
* Consensus parameters are written in `CamelCase` with the `Height` suffix,
  e.g. `DF23Height`, and live in `src/consensus/params.h`.
* Transaction type identifiers such as `'U'` are the one-byte type codes of
  custom transactions defined in `src/dfi/customtx.h`.
* This manual is written in English and follows the Diátaxis authoring rules
  described in
  [ADR-0002](adr/ADR-0002-documentation-structure.md).

## Related manuals

* [Node Software Manual](../node-manual/index.md) — installation,
  configuration, operations, RPC/CLI reference and troubleshooting.
* [User & Protocol Manual](../user-manual/index.md) — how the protocol works
  for token holders, liquidity providers and masternode operators.
