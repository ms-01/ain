# Codebase map

"Where do I find that?" — a map of the repository: the top-level layout, the
C++ tree, the DeFiChain-specific modules, the Rust workspace and the test
trees.

Part of the [Developer & Architecture Manual](../index.md).

## Top level

| Path | Content |
| --- | --- |
| `src/` | The C++ node (Bitcoin Core derived, with the DeFiChain extensions in `src/dfi/`). |
| `lib/` | The Rust Cargo workspace: the EVM, the Ocean indexer and the `cxx` bridge crates. |
| `test/` | Functional tests (Python), lint scripts and test helpers. |
| `doc/` | Legacy documentation that is still referenced by the build (`Makefile.am` `DIST_DOCS`, `doc/Doxyfile.in`). |
| `docs/` | The canonical documentation suite published with MkDocs (this manual lives here). |
| `depends/` | The dependency build system (`depends/`) used by `make.sh`. |
| `build-aux/`, `configure.ac`, `Makefile.am`, `autogen.sh` | The autotools build. |
| `make.sh` | The convenience build script (`./make.sh build`). |
| `ci/` | CI helper scripts (lint, fuzz, sync tests, ethlibs). |
| `.github/workflows/` | The GitHub Actions workflows (build, tests, lint, fuzz, docs). |
| `rust-toolchain.toml` | The pinned Rust toolchain for the workspace. |
| `mkdocs.yml` | The MkDocs configuration of the documentation site. |

## The C++ tree (`src/`)

The C++ part of the node follows the Bitcoin Core layout, so upstream
knowledge transfers directly.

| Directory | Content |
| --- | --- |
| `src/consensus/` | Consensus parameters (`params.h`) and the consensus rules of the Bitcoin-derived layer. |
| `src/dfi/` | Everything DeFiChain-specific: accounts, tokens, DEX, loans, vaults, oracles, masternodes, governance, the EVM bridge and the DVM RPCs. |
| `src/rpc/` | The Bitcoin-style JSON-RPC server and its command modules. |
| `src/net_processing.cpp`, `src/net.*`, `src/addrman.*` | Peer handling, address manager, message processing, IBD. |
| `src/txmempool.*`, `src/mempool_accept.*` | Mempool and transaction acceptance. |
| `src/validation.*` | Block and transaction validation, `ConnectBlock`, chain state. |
| `src/miner.*` | Block assembly, including the EVM block template. |
| `src/pos.*`, `src/pos_kernel.*` | Proof of stake kernels and PoS validation. |
| `src/consensus/`, `src/chainparams.cpp` | Network parameters and the activation heights. |
| `src/node/` | Node-level helpers: coin database, mempool persistence, transaction and PSBT helpers. |
| `src/policy/` | Fee policy, RBF, TRUC and package policies. |
| `src/wallet/` | The wallet (if built with wallet support). |
| `src/script/` | The script interpreter. |
| `src/crypto/`, `src/secp256k1/`, `src/crc32c/` | Cryptography primitives. |
| `src/index/` | The block filter, transaction and spent-output indexes. |
| `src/leveldb/` | The bundled LevelDB. |
| `src/ffi/` | The C++ side of the `cxx` bridge (`ffiexports.*`, `ffiocean.*`, `ffihelpers.h`, `cxx.h`). |
| `src/zmq/`, `src/rest.cpp`, `src/httpserver.*` | ZMQ notifications, REST interface and HTTP server. |
| `src/util/`, `src/common/`, `src/support/` | Shared helpers (logging, time, threading, configuration). |
| `src/test/` | The C++ unit tests (Boost.Test) and the fuzz targets. |
| `src/bench/` | The benchmarks. |
| `src/test/fuzz/` | Fuzz targets driven by `libFuzzer` and the `test/fuzz/test_runner.py` driver. |

### The DeFiChain modules (`src/dfi/`)

| Module | Files | Responsibility |
| --- | --- | --- |
| Accounts | `accounts.cpp`, `accountshistory.cpp`, `balances.h`, `coinselect.h` | Balances, submbalances, account history, coin selection. |
| Tokens | `tokens.cpp`, `undos.cpp` | Token creation, minting, destruction, DAT handling, undo data. |
| Custom transactions | `customtx.cpp` | The DfTx types, their serialisation and the type codes. |
| DEX | `poolpairs.cpp`, `icxorder.cpp` | Liquidity pools, swaps and ICX orders. |
| Loans & vaults | `loan.cpp`, `vault.cpp`, `vaulthistory.cpp` | Loans, dTokens, vaults, collateral and auctions. |
| Oracles | `oracles.cpp` | Price oracles and the price aggregation. |
| Masternodes | `masternodes.cpp`, `mn_checks.cpp` | Masternode registration, timelocks, PoS checks and the XVM coinbase payload. |
| Governance | `gv.cpp`, `govvariables/`, `proposals.cpp` | Governance variables, attributes and on-chain proposals. |
| Consensus rules | `consensus/` | The consensus checks per domain: `tokens.cpp`, `accounts.cpp`, `poolpairs.cpp`, `loans.cpp`, `vaults.cpp`, `oracles.cpp`, `masternodes.cpp`, `proposals.cpp`, `governance.cpp`, `smartcontracts.cpp`, `icxorders.cpp`, `tokenlock.cpp`, `xvm.cpp`, `txvisitor.cpp`. |
| EVM bridge | `evm.cpp` | The EVM block template handle and the cross-layer block/transaction edges. |
| Anchors | `anchors.cpp` | Anchor (chainwork) handling. |
| Snapshots | `snapshotmanager.cpp` | The account/vault snapshots used for token and loan migrations. |
| History | `historywriter.cpp`, `auctionhistory.cpp` | Block and auction history indexes. |
| RPC | `rpc_*.cpp` | The DeFiChain RPC commands: accounts, tokens, poolpair, ICX orderbook, loans, vaults, oracles, masternodes, proposals, custom transactions and the EVM RPC. |
| Errors | `errors.h`, `res.h` | The DeFiChain error codes (`CustomTxErrCodes`) and the `Res`/`ResVal` result types. |
| Threading | `threadpool.cpp` | The background thread pool used by the DVM subsystems. |

## The Rust workspace (`lib/`)

`lib/Cargo.toml` defines the workspace. The members are the `ain-*` crates plus
`cli` and `parser`.

| Crate | Purpose |
| --- | --- |
| `ain-evm` | The EVM: executor, gas accounting, precompiles, block template, storage backend, genesis and filters. |
| `ain-cpp-imports` | The Rust → C++ side of the `cxx` bridge (`lib/ain-cpp-imports/src/bridge.rs`) and the `cxx-build` glue. |
| `ain-rs-exports` | The C++ → Rust side of the `cxx` bridge (`lib/ain-rs-exports/src/lib.rs`) and the `cxx-gen` header/source generator. |
| `ain-db` | The storage backend of the EVM and Ocean data (RocksDB, `bincode` encoding). |
| `ain-dftx` | The shared definition of the DfTx types and of the skipped-transaction list, so both sides agree on the format. |
| `ain-contracts` | Contract artifacts and helpers for the precompiles. |
| `ain-grpc` | The Ethereum JSON-RPC (`eth`, `net`, `web3`, `debug`), the subscriptions and the gRPC service. |
| `ain-ocean` | The Ocean indexer and its REST API (axum): blocks, transactions, tokens, prices, governance and stats. |
| `ain-macros` | Procedural macros used by the workspace. |
| `cli` (`metachain-cli`) | The command line client for the Rust services. |
| `parser` | The parser for transaction and RPC payloads. |
| `proto/` | The protobuf definitions (`block.proto`, `eth.proto`, `tx.proto`) of the gRPC service — a directory, not a crate. |

## The test trees

| Path | Content |
| --- | --- |
| `src/test/` | The C++ unit tests (Boost.Test), run with `src/test/test_defi --run_test=<Suite>`. Suite names follow the module, e.g. `applytx_tests`, `evm_tests`, `xvm_tests`, `pos_tests`, `mempool_tests`. |
| `src/test/fuzz/` | The fuzz targets (`transaction.cpp`, `deserialize.cpp`, `p2p_message_parsing.cpp`, …). |
| `test/functional/` | The Python functional tests, driven by `test/functional/test_runner.py`. They cover accounts, tokens, DEX, loans, masternodes, governance and the EVM (`feature_evm_*.py`, `feature_poolpair*.py`, `feature_poolswap*.py`, `feature_icx_orderbook*.py`, …). |
| `test/functional/contracts/` | Solidity contracts used by the functional tests. |
| `test/fuzz/` | The fuzzing driver (`test_runner.py`) that feeds the corpus to the fuzz targets. |
| `test/lint/` | The lint scripts: `lint-all.sh`, `lint-circular-dependencies.sh`, `check-docs-structure.py`, `check-rpc-mappings.py` and others. |
| `test/util/` | Python helpers for the functional tests. |
| `test/config.ini.in` | The template of the test configuration file. |
| `ci/` | CI-only helpers (extended lint, fuzzing, sync tests, ethlibs). |

## Legacy documentation (`doc/`)

The `doc/` directory holds the documentation that predates the canonical
MkDocs site. Some of its files are part of the distribution (`Makefile.am`,
`DIST_DOCS`) and the Doxygen configuration (`doc/Doxyfile.in`), so they are
kept and are gradually consolidated into this manual:

| Legacy file | Consolidated in |
| --- | --- |
| `doc/developer-notes.md` | [Building & Testing](../building-testing/index.md) |
| `doc/build-unix.md`, `doc/build-osx.md`, `doc/build-windows.md`, `doc/build-quick.md`, `doc/dependencies.md` | [Building & Testing](../building-testing/index.md) |
| `doc/fuzzing.md` | [Building & Testing](../building-testing/index.md) |
| `doc/hardforks.md` | [Consensus](../consensus/index.md) |

## Quick index of key files

| Question | File |
| --- | --- |
| What is the activation height of a hardfork? | `src/consensus/params.h`, `src/chainparams.cpp` |
| How is a DfTx defined? | `src/dfi/customtx.h` |
| Where is a consensus rule for tokens implemented? | `src/dfi/consensus/tokens.cpp` |
| How is the mempool coupled to activation heights? | `src/txmempool.cpp` (`MempoolRulesChangedBetween()`) |
| Where is the EVM block template created? | `src/dfi/evm.cpp` (`CScopedTemplate`) |
| Where is the Rust side of the bridge declared? | `lib/ain-rs-exports/src/lib.rs` |
| Where is the C++ side of the bridge declared? | `lib/ain-cpp-imports/src/bridge.rs` |
| Where is the C++ implementation of the bridge? | `src/ffi/ffiexports.cpp` |
| Where is the EVM implemented? | `lib/ain-evm/src/` |
| Where is the Ocean indexer implemented? | `lib/ain-ocean/src/` |
| Where is the Ethereum JSON-RPC implemented? | `lib/ain-grpc/src/rpc/` |
| Where are the DeFiChain RPCs registered? | `src/rpc/register.h`, `src/dfi/rpc_*.cpp` |
| Where do the activation-height CLI overrides live? | `src/chainparams.cpp` (`UpdateHeightValidation()`) |

## Related pages

* [Architecture overview](../architecture/overview.md) — how the parts above
  work together.
* [C++ ↔ Rust bridge](../architecture/cxx-bridge.md) — the FFI boundary in
  detail.
