# Performance and IBD

How to size a node: disk, memory, bandwidth and the initial block download.

Part of the [Node Software Manual](../index.md) ·
[Configuration](index.md).

## Contents

* [Disk](#disk)
* [Memory](#memory)
* [Traffic](#traffic)
* [The initial block download](#the-initial-block-download)
* [Header-tip selection](#header-tip-selection)

## Disk

### Pruning

Pruning deletes old block files and keeps only the chain state and the most
recent blocks:

| `-prune` | Behaviour |
| --- | --- |
| `0` (default) | No pruning. The node keeps every block. |
| `1` | Manual pruning: the node keeps the block files, and the `pruneblockchain` RPC deletes them. |
| `>= 550` | Automatic pruning: the node keeps the disk usage of the block files below the given target in MiB. Values below 550 MiB are rejected (`MIN_DISK_SPACE_FOR_BLOCK_FILES`). |

```bash
defid -prune=2048
```

!!! warning
    Pruning is incompatible with `-txindex` and with the wallet rescan
    (`-rescan`). Switching pruning on or off later requires a full
    resynchronisation of the blockchain: the node refuses to start and asks
    for `-reindex`.

Pruning is the right choice for a node that serves wallets and RPC calls. It
is the wrong choice for a node that has to serve historical blocks to other
nodes during their initial block download.

### The transaction index

`-txindex` maintains an index of every transaction, so that
`getrawtransaction` can answer for a transaction without its block hash. It
is the usual choice for an explorer or an exchange node that has to look up
deposits. It costs additional disk space and is not compatible with pruning.

### The EVM state

The MetaChain EVM state in `<datadir>/evm` grows with the number of EVM
transactions and contracts. It is not affected by `-prune`; the only way to
reclaim the space is to remove the directory and resynchronise the chain.

## Memory

### The UTXO cache

`-dbcache=<n>` sets the size of the UTXO database cache in MiB:

| Value | Meaning |
| --- | --- |
| `512` (default) | The default cache size. |
| `4` | The minimum. |
| `16384` | The maximum on a 64-bit system (`1024` on a 32-bit system). |

The cache is split into a custom transaction part (a quarter of the total) and
a coin part (the rest). During the initial block download the node flushes
the chain state based on the smaller custom part only; after the IBD the
threshold is doubled.

A smaller cache slows the validation down noticeably, especially during the
initial block download. A larger cache speeds the validation up and costs
RAM.

```bash
# A small VPS: 256 MiB cache.
defid -dbcache=256
```

### The mempool

`-maxmempool=<n>` limits the mempool to `n` megabytes (the default is 300).
Unused mempool memory is shared with the UTXO cache, so lowering the mempool
limit is the effective way to reduce the memory usage.

`-blocksonly` disables the mempool completely: the node stops accepting and
relaying transactions from the network and only processes the transactions
that are part of a block. Do not use it on a node that broadcasts
transactions — every transaction of yours stands out. If you use it together
with a wallet, combine it with `-walletbroadcast=0` and
`-spendzeroconfchange=0` and use a different channel to broadcast your own
transactions.

### Connections and threads

| Option | Default | Meaning |
| --- | --- | --- |
| `-maxconnections=<n>` | 125 | Maximum number of peer connections. Every connection costs memory. Only inbound connections count here — the node always opens up to 8 outbound connections. |
| `-par=<n>` | auto | Number of script verification threads. `0` detects the number of cores, a negative value leaves that many cores free. The value is capped at 4 additional threads, because the custom transactions of DeFiChain do not benefit from more. |
| `-rpcthreads=<n>` | 4 | Number of threads that serve the RPC requests. |

### Linux: memory arenas

Since glibc 2.10 the C library creates up to two heap arenas per core, which
can inflate the memory usage. Setting `MALLOC_ARENA_MAX` before starting the
node avoids this:

```bash
#!/bin/bash
export MALLOC_ARENA_MAX=1
exec defid "$@"
```

The setting trades a little CPU locality for a significantly lower memory
footprint.

## Traffic

| Option | Default | Meaning |
| --- | --- | --- |
| `-maxuploadtarget=<n>` | 0 | Keeps the outbound traffic below `n` MiB per 24 h. `0` means no limit. This is a soft limit: when it is nearly reached, the node stops serving historic blocks (blocks older than a week). Whitelisted peers are never disconnected, but their traffic counts. |
| `-listen=0` | off | The node accepts no inbound connections. Fewer peers, less traffic — but the node also stops being useful for the network. |
| `-maxconnections=<n>` | 125 | Fewer connections mean less traffic. The trustless model works best with a handful of well-connected peers. |
| `-blocksonly` | off | No transaction relay. The fee estimation stops working, block propagation becomes slower and the privacy of the node suffers if a wallet is loaded. |

## The initial block download

The initial block download (IBD) is the phase in which the node downloads the
block headers first and then the blocks themselves. It is the only phase that
saturates the link and the disk.

The node considers itself in the initial block download while

* it is importing or reindexing,
* its tip is below the minimum chain work of the network
  (`nMinimumChainWork`), or
* its tip is older than `-maxtipage` seconds (the default is 10 hours).

```bash
# Consider the node out of IBD after 30 minutes without a new block.
defid -maxtipage=1800
```

### What helps

* **A fast disk.** The bottleneck of the IBD is usually the disk, not the
  network. An SSD makes the difference between hours and days.
* **Enough memory.** The UTXO cache is the limiting factor; see
  [Memory](#memory).
* **Peers.** The node downloads from up to eight peers in parallel. A node
  that only has one peer is slow.
* **Do not use `-blocksonly` during the IBD** unless you really need it: the
  transactions of the blocks still have to be validated.

### What to expect

The progress is visible in the log and through the RPC:

```bash
defi-cli getblockchaininfo | grep -E '"blocks"|"headers"'
defi-cli getnetworkinfo | grep -E 'connections|networks'
```

If the progress stops, the [Troubleshooting](../troubleshooting/index.md)
section describes how to diagnose it.

## Header-tip selection

`-headertipmode=<anchored|chainwork>` selects how the node chooses the best
header chain while it is ahead of the validated tip:

| Mode | Behaviour |
| --- | --- |
| `anchored` (default) | Prefers the header chain that is anchored in the validated chain. A forged fork with inflated chain work cannot take over the header tip. |
| `chainwork` | Selects the header chain with the highest raw `nChainWork`. This is the legacy behaviour and it is vulnerable to forged-header tip hijacking on a proof-of-stake chain. |

```bash
defid -headertipmode=anchored
```

An invalid value is a startup error:

```text
Invalid -headertipmode value 'foo'. Must be 'anchored' or 'chainwork'.
```

The option is **not** a consensus rule: it does not change the block
validation, it only decides which header chain the node follows while it
synchronises. The reasoning and the exact comparison are documented in
[ADR-0001: PoS Chainwork Soundness and Anchored Header-Tip Selection](../../developer-manual/adr/ADR-0001-pos-chainwork-header-tip-selection.md).

!!! note
    The `anchored` mode is the default and should not be changed. The
    `chainwork` mode exists to reproduce the legacy behaviour when a
    compatibility problem has to be investigated.
