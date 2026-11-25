# State storage

The node keeps two kinds of state: the Bitcoin-derived UTXO state and the
DeFiChain DVM state on the C++ side, and the EVM/MetaChain state on the Rust
side. This page describes the storage abstractions, the databases in the data
directory, and how the two layers are kept consistent.

Part of the [Subsystems section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The C++ storage abstraction is in `src/flushablestorage.h`, the database
    wrappers in `src/dbwrapper.{h,cpp}` and `src/txdb.{h,cpp}`, the DVM
    state in `src/dfi/`, and the Rust storage backend in `lib/ain-db/`.

## Two storage engines

| Layer | Engine | Crate / module | Content |
| --- | --- | --- | --- |
| C++ | LevelDB (`src/leveldb/`) | `src/dbwrapper.h`, `src/flushablestorage.h` | The UTXO set, the block index, the DVM state (accounts, tokens, DEX, loans, vaults, oracles, governance, masternodes). |
| Rust | RocksDB | `lib/ain-db/` | The EVM state: blocks, transactions, receipts, logs and the Ocean index data. |

The two layers are separate databases with separate lifecycles. They are
coupled through the `cxx` bridge (see the
[C++ ↔ Rust bridge](../architecture/cxx-bridge.md)) and through the
*transfer domain* mechanism, which moves value between the DVM and the EVM.

## The C++ storage abstraction

`src/flushablestorage.h` defines a small key/value abstraction that is used
by all DVM views:

| Class | Role |
| --- | --- |
| `CStorageKV` | The abstract key/value store: `Read()`, `Write()`, `Erase()`, `Exists()`, `Flush()`, `NewIterator()`, `SizeEstimate()`. |
| `CStorageLevelDB` | The LevelDB implementation of `CStorageKV` (`src/flushablestorage.h`). |
| `CFlushableStorageKV` | A write-back overlay in front of a `CStorageKV`. |
| `CStorageView` | A typed wrapper around a single `CStorageKV` that provides `Read()`, `Write()`, `Erase()`, `Exists()` and iteration. |
| `CLazySerialize` | A lazily deserialised value that is read from the iterator only when it is used. |
| `CStorageIteratorWrapper` | The typed iterator over the key space of a view. |

### `CFlushableStorageKV`

`CFlushableStorageKV` is the central piece: it keeps the changes in a
`MapKV` (a `std::map` of `TBytes` to `std::optional<TBytes>`) and only
writes them to the underlying database when `Flush()` is called:

```cpp
bool Write(const TBytes& key, const TBytes& value) override {
    changed[key] = value;
    return true;
}

bool Erase(const TBytes& key) override {
    changed[key] = {};
    return true;
}
```

An empty `std::optional` marks a deleted key, so a deletion is also part of
the overlay and survives until the next `Flush()`. `Read()` and `Exists()`
consult the overlay first and fall back to the database.

Two constructors exist:

* the normal one takes a reference to the underlying `CStorageKV`,
* the snapshot constructor takes a `CStorageLevelDB` snapshot and the
  corresponding overlay; on such a snapshot `Flush()` throws, because a
  snapshot is read-only.

`CreateSnapshotData()` returns the overlay together with a LevelDB snapshot,
which is what `CSnapshotManager` uses (see below). `SizeEstimate()` reports
the dynamic memory usage of the overlay.

The DVM views are built on top of it:

* `CMasternodesView` (`src/dfi/masternodes.h`) exposes
  `GetStorage()` and casts its database to `CFlushableStorageKV`.
* `CCustomCSView` derives from `CMasternodesView` and adds the accounts,
  tokens, pool pairs, loans, vaults, oracles and governance state. It is the
  view that the DVM consensus rules (`src/dfi/consensus/`) work on.
* `CAccountHistoryStorage` (`src/dfi/accountshistory.h`) is a `CStorageView`
  over a `CStorageLevelDB`. The `CFlushableStorageKV` overlay is used by
  `CAccountsHistoryWriter` (`src/dfi/accountshistory.cpp`), which wraps the
  storage of a `CAccountHistoryStorage` for the duration of a write.

### The Bitcoin-derived state

| Data | Class | Location |
| --- | --- | --- |
| UTXO set | `CCoinsViewDB` | `src/txdb.{h,cpp}` |
| Block index | `CBlockTreeDB` | `src/txdb.{h,cpp}` |
| Generic database wrapper | `CDBWrapper`, `CDBBatch`, `CDBIterator` | `src/dbwrapper.{h,cpp}` |

`CChainState::InitCoinsDB()` (`src/validation.cpp`) opens the coins database
in the `chainstate` directory and `CBlockTreeDB` opens the block index in the
`index` directory of the blocks directory.

## The databases in the data directory

| Path | Content | Opened by |
| --- | --- | --- |
| `<datadir>/chainstate/` | The UTXO set (LevelDB). | `CChainState::InitCoinsDB()`, `src/validation.cpp` |
| `<blocksdir>/index/` | The block index (LevelDB). | `CBlockTreeDB`, `src/txdb.cpp` |
| `<datadir>/enhancedcs/` | The DVM state: accounts, tokens, DEX, loans, vaults, oracles, governance, masternodes (LevelDB). | `CStorageLevelDB` + `CCustomCSView`, `src/init.cpp` |
| `<datadir>/history/` | The account history index (optional, `-acindex`). | `CAccountHistoryStorage`, `src/init.cpp` |
| `<datadir>/burn/` | The burn history index. | `CBurnHistoryStorage`, `src/init.cpp` |
| `<datadir>/vault/` | The vault history index (optional, `-vaultindex`). | `CVaultHistoryStorage`, `src/init.cpp` |
| `<datadir>/evm/` | The EVM state (RocksDB). | `lib/ain-evm/src/storage/`, `lib/ain-grpc/src/lib.rs` |
| `<datadir>/mempool.dat` | The persisted mempool (`-persistmempool`). | `src/node/mempool_persist.cpp` |
| `<datadir>/peers.dat` | The known addresses. | `CAddrDB`, `src/addrdb.cpp` |
| `<datadir>/banlist/` | The ban list. | `BanMan`, `src/banman.cpp` |
| `<datadir>/net/anchors.dat` | The anchor peers. | `src/init.cpp`, `src/addrdb.h` |
| `<datadir>/fee_estimates.dat` | The fee estimates. | `src/init.cpp` |

The DVM database carries its own version
(`CCustomCSView::DbVersion`, `src/dfi/masternodes.h`). When the version of
an existing database does not match, the node refuses to start with
*Account database is unsuitable*; the operator has to reindex.

## Undo data and reorgs

To undo the effect of a disconnected block, the node writes the *undo data*
of the block into the custom state:

* `src/dfi/undo.h` and `src/dfi/undos.cpp` define the undo records of the
  DVM state (balances, token supplies, pool pairs, loans, vaults, …).
* The undo data is written with the block and read back when the block is
  disconnected during a reorg.

## Snapshots

`CSnapshotManager` (`src/dfi/snapshotmanager.{h,cpp}`) takes a consistent
snapshot of the DVM state, the account history and the vault history:

* `SetBlockSnapshots()` stores the snapshot of the current view,
* `GetSnapshots()` returns the snapshot collection (the view, the account
  history and the vault history),
* `ReturnSnapshot()` gives a checked-out snapshot back.

The snapshots are used by the migrations that need a stable view of the state
while it is being rewritten (token and loan migrations), and by the block
template creation.

## The Rust storage backend

`lib/ain-db/` is the storage backend of the EVM and of the Ocean indexer:

| Item | Role |
| --- | --- |
| `Rocks` | The RocksDB wrapper: `open()`, `compact()`, `flush()`, `cf_handle()`, `iterator_cf()`, `dump_table_sizes()`, `hash_db_state()`. |
| `Column`, `ColumnName`, `TypedColumn` | The traits that describe a column family and its key and value types. |
| `LedgerColumn` | A typed column with `get()`, `put()`, `delete()` and `iter()`. |
| `version.rs` | The migration mechanism of the database schema. |

The values are encoded with `bincode`.

`lib/ain-evm/src/storage/` builds the EVM storage on top of it:

| File | Content |
| --- | --- |
| `db.rs` | The column definitions: transactions, blocks, receipts, block map, latest block number, address logs, address code, deployed code hashes and the metadata. |
| `block_store.rs` | `BlockStore`, the RocksDB backed store of the blocks, transactions and receipts. |
| `cache.rs` | The in-memory cache in front of the store. |
| `migration.rs` | The schema migrations. |
| `traits.rs` | The storage traits (`BlockStorage`, `FlushableStorage`, `ReceiptStorage`, `Rollback`, `TransactionStorage`). |

`lib/ain-ocean/src/storage/` uses the same backend for the indexer data
(`ocean_store.rs`).

## The EVM folder and the reindex

The EVM database lives in `<datadir>/evm` and is wiped when the node is
reindexed: `ain_rs_wipe_evm_folder()` (`lib/ain-grpc/src/lib.rs`) removes the
folder and `src/init.cpp` calls it when `-reindex` or `-reindex-chainstate`
is used. The C++ databases are wiped in the same run.

| Option | Effect |
| --- | --- |
| `-reindex` | Rebuild the block index from the block files and wipe all the state databases. |
| `-reindex-chainstate` | Rebuild only the chain state (and the DVM and EVM databases). |
| `-dbcache=<n>` | The LevelDB cache size in MiB. The unused mempool memory is shared with it. |

## Testing

| Test | Location |
| --- | --- |
| Storage abstraction | `src/test/storage_tests.cpp` |
| The flushable storage with the EVM | `src/test/evm_tests.cpp` |
| The DVM state | `src/test/applytx_tests.cpp` |
| The Rust backend | `lib/ain-db/`, `cargo test -p ain-db` |

## Related pages

* [Mempool and transaction acceptance](mempool.md) — the account view that
  is built on top of this state.
* [PoS validation and masternodes](pos-validation.md) — the masternode state.
* [RPC and gRPC layer](rpc-and-grpc.md) — how the state is exposed.
* [C++ ↔ Rust bridge](../architecture/cxx-bridge.md) — the boundary between
  the two storage engines.
* [Data directory](../../node-manual/installation/data-directory.md) — the
  operator-facing description of the files.
