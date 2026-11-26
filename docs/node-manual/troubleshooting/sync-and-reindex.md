# Sync and reindex

Diagnose a chain that stopped advancing and rebuild a damaged database.

Part of the [Node Software Manual](../index.md).

## When to use this guide

Use it when one of the following is true:

* the tip height does not grow any more,
* the node exits during startup with a database error,
* the node starts but the chain state does not match the block index,
* the node reports that it cannot replay the blocks.

## First: is it a stall or a broken database?

Both look the same from the outside — the tip does not move. They are told
apart with three commands:

```bash
defi-cli getblockchaininfo | grep -E '"blocks"|"headers"|"initialblockdownload"'
defi-cli getpeerinfo | grep -c synced_headers
grep -E "Error|Failed|Corrupted" <datadir>/debug.log | tail -20
```

| Observation | Diagnosis |
| --- | --- |
| `headers` grows, `blocks` stays behind, `initialblockdownload` is `true` | The node is downloading but does not connect the blocks. See [Sync stalls](#sync-stalls). |
| `headers` and `blocks` are equal and neither grows | The node has no usable peers. See [Peer problems](#peer-problems). |
| The node exits at startup with `Error loading block database`, `Corrupted block database detected` or `Inconsistent chainstate detected between DVM block database and EVM block database` | The database is damaged. See [Rebuilding the database](#rebuilding-the-database). |
| The log contains `Unable to replay blocks` | The chainstate does not match the block index. See [Rebuilding the database](#rebuilding-the-database). |

## Sync stalls

### The node is still in initial block download

`initialblockdownload` is `true` while the tip is younger than the maximum
tip age, which is 10 hours by default and can be changed with `-maxtipage`.
During the initial sync the node behaves like a download client:

* it connects to peers,
* it downloads and verifies the blocks,
* it does not serve the transactions of the blocks it already has to peers
  that are further behind.

This phase is expected on a new node and after a long downtime. The
[Performance and IBD](../configuration/performance-ibd.md) page covers the
options that make it faster (`-dbcache`, `-maxmempool`, `-blocksonly`).

### The download stops

The log shows the last activity, and `getpeerinfo` shows whether the peers
are still connected:

```bash
defi-cli getpeerinfo | grep -E '"addr"|"synced_blocks"|"inbound"'
```

Common causes:

| Cause | What the log shows | Fix |
| --- | --- | --- |
| No peers | `getpeerinfo` is empty, `getnetworkinfo` reports 0 connections | Check the network ports and the firewall, see [Peer problems](#peer-problems). |
| The peers are ahead but do not serve the blocks | `synced_headers` is far ahead of `synced_blocks` | The download is bandwidth-bound. `-maxuploadtarget` limits the *outbound* traffic and does not speed up a download. |
| The node is behind a restrictive NAT or proxy | No inbound connections | Open the P2P port, or run the node with `-proxy`/`-onion`. |
| The disk is full | `Error` lines about writing to the database | Free space or move the data directory, see the [data directory](../installation/data-directory.md) page. |
| The node is too old for the network | Peers disconnect after the version handshake | Upgrade to a current release. |

### Peer problems

```bash
defi-cli getnetworkinfo | grep -E '"connections"|"connections_in"|"subversion"'
```

If the node has no connections at all, check in this order:

1. `-port` matches the port of the network you selected. Mainnet uses 8555,
   testnet 18555, changi 20555, devnet 21555 and regtest 19555.
2. The port is open in the firewall and forwarded, if the node is behind a
   NAT.
3. `-connect`, `-onlynet` and `-whitelist` in the configuration file do not
   restrict the node to an unreachable peer.
4. `-dnsseed=0` is not set on a node that is expected to find peers through
   the DNS seeds.

The [Networking and privacy](../configuration/networking-privacy.md) page
describes the ports and the anonymity options.

## Rebuilding the database

Two options rebuild the state of the chain. They differ in what they use as
their source of truth.

| Option | Source of truth | Rebuilds | Use it when |
| --- | --- | --- | --- |
| `-reindex-chainstate` | The block index on disk | The chain state (the UTXO set and the account databases) | The blocks are fine but the state is inconsistent. |
| `-reindex` | The block files `blk*.dat` on disk | The block index **and** the chain state | The block index is damaged, the block files are damaged, or the node runs in pruning mode. |

The help text of the node states the same distinction: *"Rebuild chain state
and block index from the blk*.dat files on disk"* for `-reindex`, and
*"Rebuild chain state from the currently indexed blocks. When in pruning mode
or if blocks on disk might be corrupted, use full -reindex instead."* for
`-reindex-chainstate`.

### Choosing the right one

```text
Database error at startup?
├── "Corrupted block database detected", "Error loading block database",
│   "Inconsistent chainstate detected between DVM block database and EVM
│   block database", "Unable to get DVM block index from block hash"
│       └── full -reindex
└── "Unable to replay blocks", "Account database is unsuitable",
    "Error upgrading chainstate database"
        └── -reindex-chainstate
```

Additional rules:

* **Pruning**: if the node ever ran with `-prune`, the old block files were
  deleted. A full `-reindex` cannot restore them, so use
  `-reindex-chainstate` — or, if the state itself is damaged, resynchronise
  the node from scratch. A pruned data directory that is started *without*
  `-prune` is refused with the message *"You need to rebuild the database
  using -reindex to go back to unpruned mode. This will redownload the entire
  blockchain"*.
* **`-fastprune`**: this test-only option changes the on-disk layout of the
  block files. A data directory created with `-fastprune` must not be reused
  without it and the other way round. It is only allowed on regtest.
* **Block files damaged**: if `blk*.dat` is the damaged part, only a full
  `-reindex` helps — but it can only rebuild what is still on disk. If the
  block files are gone or truncated, resynchronise instead.

### Running a rebuild

Stop the node first, then start it with the option. The rebuild happens
during startup, before the node serves any request:

```bash
# Stop the service.
sudo systemctl stop defid

# Rebuild the chain state from the existing block index.
sudo systemctl start defid   # with -reindex-chainstate in /etc/defi/defi.conf
```

The log shows the progress:

| Log line | Meaning |
| --- | --- |
| `Rewinding blocks...` | The node prepares the block index for a rebuild. |
| `Reindexing block file blk00000.dat...` | One block file is read again. The files are processed in order. |
| `Reindexing finished` | The block index is complete again. |
| `Error: ...` while loading | The rebuild did not succeed; the message names the database that failed. |

The rebuild is CPU- and disk-bound. It reads the block files from disk and
writes the block index and the chain state again, so it needs the same free
disk space as the original data directory and takes roughly as long as the
initial sync of the same height.

!!! warning
    Stop the node before the rebuild and do not interrupt it. An
    interrupted rebuild leaves the databases in a partially rebuilt state;
    the next start detects this and offers the rebuild again.

### What a rebuild resets

Both options reset more than the plain chain state. The node wipes the
custom state databases (`enhancedcs`, `history`, `burn`, `vault`), the
anchor and SPV databases, and the EVM folder, and it replays the blocks
from the beginning. The wallet files are **not** touched — a rebuild does
not lose the keys — but the wallet has to rescan the chain to learn its
balances again.

The wallet keeps its keys. If the balances do not match the chain afterwards,
the wallet rescans the chain — either automatically, because its last
processed block is no longer on the chain, or on demand:

```bash
defi-cli rescanblockchain 700000
```

The [Backups and signers](../operations/backups-signers.md) page describes
what to back up before a rebuild.

## Resynchronising from scratch

When the database cannot be repaired — for example after a disk failure that
destroyed the block files — the node is rebuilt from the network:

```bash
sudo systemctl stop defid
sudo mv /var/lib/defi /var/lib/defi.broken
sudo mkdir -p /var/lib/defi
sudo chown defi:defi /var/lib/defi
sudo systemctl start defid
```

Keep the old directory until the new node has synced and the balances match
what you expect. A full resynchronisation of mainnet needs the disk space
and the time of a complete node.

## Next steps

* [Masternode stalls](masternode-stalls.md) — a chain that is stuck because
  the masternodes cannot agree, not because of a database problem.
* [Performance and IBD](../configuration/performance-ibd.md) — how to make
  the initial sync faster.
* [Data directory](../installation/data-directory.md) — the layout of the
  files that the rebuild reads and writes.
