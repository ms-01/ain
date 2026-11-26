# Data directory

Everything the node writes to disk lives in one directory. This page explains
where that directory is, what is in it and what may be deleted.

Part of the [Node Software Manual](../index.md) ·
[Installation](index.md).

## Contents

* [Location](#location)
* [Network subdirectory](#network-subdirectory)
* [Files and directories](#files-and-directories)
* [The EVM state](#the-evm-state)
* [What can be deleted](#what-can-be-deleted)
* [Backup](#backup)

## Location

The data directory is chosen with `-datadir`. If the option is not given, the
node uses the platform default:

| Platform | Default data directory |
| --- | --- |
| Linux | `$HOME/.defi` |
| macOS | `$HOME/Library/Application Support/DeFi` |
| Windows | `%APPDATA%\DeFi Blockchain` |

The node creates the platform default directory — and the `wallets/`
subdirectory — on the first start. An explicitly given `-datadir` must
already exist: the node does not create it and refuses to start otherwise.

## Network subdirectory

Unless the node runs on mainnet, the data of a network lives in a
network-specific subdirectory of the data directory:

| Network | Subdirectory | Full path on Linux |
| --- | --- | --- |
| mainnet | *(none)* | `~/.defi` |
| testnet | `testnet3/` | `~/.defi/testnet3` |
| changi | `changi/` | `~/.defi/changi` |
| devnet | `devnet/` | `~/.defi/devnet` |
| regtest | `regtest/` | `~/.defi/regtest` |

This is why a single machine can run a mainnet node and a regtest node at the
same time without them interfering. The `-blocksdir` option moves the block
files to a different location; the block index then lives in
`<blocksdir>/index` instead of `<datadir>/blocks/index`.

## Files and directories

The table lists the entries of the network-specific data directory.

| Entry | Description |
| --- | --- |
| `blocks/` | The block files. `blk000??.dat` holds the blocks themselves, `rev000??.dat` the undo data. Each block file is up to 128 MiB (`MAX_BLOCKFILE_SIZE`). |
| `blocks/index/` | The block index (LevelDB). |
| `chainstate/` | The chain state database (LevelDB): the UTXO set and the account balances. |
| `indexes/txindex/` | The optional transaction index, only present with `-txindex`. |
| `indexes/blockfilter/` | The optional compact block filter index, only present with `-blockfilterindex`. |
| `indexes/txospenderindex/` | The optional spend index of the account model, only present with `-txospenderindex`. |
| `wallets/` | The wallet directories (`wallet.dat` plus the Berkeley DB environment) and `db.log`. |
| `evm/` | The MetaChain EVM state, see below. |
| `debug.log` | The log of the node. |
| `defi.conf` | The configuration file, if it is located in the data directory. |
| `defid.pid` | The process ID of the running daemon. |
| `.cookie` | The RPC authentication cookie. It is created when the RPC server starts without `-rpcpassword` and deleted when the daemon exits. |
| `peers.dat` | The address manager database with the known peers. |
| `banlist.json` | The ban list in a human-readable format. The legacy binary `banlist.dat` is read once for migration and is no longer written. |
| `fee_estimates.dat` | The statistics used to estimate fees and priorities. |
| `mempool.dat` | The persisted mempool. It is only written when the node is shut down with `-persistmempool=1` (the default is off). |
| `onion_v3_private_key` | The cached private key of the Tor hidden service. |
| `i2p_private_key` | The private key of the I2P destination, created by the SAM bridge on first use. |

!!! warning
    The cookie file grants full RPC access to the node, including the
    ability to move funds. Its permissions are `0600`; never copy it into a
    backup that is shared with other people.

## The EVM state

The MetaChain EVM layer keeps its own state next to the DVM state:

```text
<datadir>/<network>/evm/
├── .vsdb/            # VSDB database directory
└── …                 # block store and trie data
```

The directory is created by the node on the first start and is written by the
Rust EVM implementation (`lib/ain-evm/`). It is **not** part of the DVM chain
state: the DVM block database and the EVM block database are separate stores,
and the node checks both for consistency at startup. If they disagree, the
node refuses to continue and asks for a reindex — see
[Troubleshooting](../troubleshooting/index.md).

Because the EVM state is not covered by `-reindex-chainstate`, a full
`-reindex` is the safe option when the EVM state is suspect.

## What can be deleted

| Entry | Deletable? |
| --- | --- |
| `debug.log` | Yes — the node recreates it. Rotate it if it grows too large. |
| `.cookie` | Yes — it is recreated on the next start of the daemon. |
| `peers.dat` | Yes — the node rediscovers its peers, but the initial synchronisation becomes slower. |
| `fee_estimates.dat` | Yes — the node relearns the fee estimates. |
| `mempool.dat` | Yes — the unconfirmed transactions are lost. |
| `blocks/` | Only with `-reindex` and a resynchronisation. Never delete block files while the node runs. |
| `chainstate/` | Only with a full resynchronisation. |
| `evm/` | Only together with a full resynchronisation. |
| `wallets/` | **No** — this directory holds the private keys of your wallets. |
| `banlist.json` | Yes — the bans are lost. |

## Backup

A backup of a running node consists of two parts:

1. **The wallet directories** under `wallets/`. These hold the private keys
   and have to be copied while the node is stopped, or with a wallet-aware
   backup procedure.
2. **The configuration** (`defi.conf`), which contains the RPC credentials
   and the network selection.

The block files and the chain state do not have to be backed up: they are
re-downloaded from the network. Backing them up is only useful to avoid a
long resynchronisation.

!!! note
    Copying a wallet directory while the node is running can produce an
    inconsistent copy. Stop the node first, or use the wallet backup
    facilities described in
    [Operations](../operations/index.md).
