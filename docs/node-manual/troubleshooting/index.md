# Troubleshooting

Diagnose a node that misbehaves: find the log, read it, and decide which
recovery procedure applies.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| [Sync and reindex](sync-and-reindex.md) | Why the chain stops, how to tell an IBD stall from a broken database, and when `-reindex` or `-reindex-chainstate` is the right tool. |
| [Masternode stalls](masternode-stalls.md) | Why a chain that is one version behind cannot advance, how the masternode version gate behaves, and how to recover. |
| [Local testing and isolation](local-testing-isolation.md) | Reproduce an operational problem in a regtest or container environment without touching a production node. |

## Where the log is

The node writes its log to `debug.log` in the network-specific part of the
data directory:

| Network | Log file |
| --- | --- |
| mainnet | `<datadir>/debug.log` |
| testnet | `<datadir>/testnet3/debug.log` |
| changi | `<datadir>/changi/debug.log` |
| devnet | `<datadir>/devnet/debug.log` |
| regtest | `<datadir>/regtest/debug.log` |

`<datadir>` is the value of `-datadir`; the default is `~/.defi` on Linux and
macOS. The [data directory](../installation/data-directory.md) page lists the
complete directory layout.

Four options control the log:

| Option | Effect |
| --- | --- |
| `-debuglogfile=<file>` | Writes the log to another file. A relative path is resolved inside the network-specific data directory, so `-debuglogfile=node.log` ends up next to `debug.log`. |
| `-nodebuglogfile` | Disables the log file completely. Use it only when `-printtoconsole` is active. |
| `-printtoconsole` | Mirrors the log to the standard output. It is on by default when the daemon is started in the foreground and off when it runs with `-daemon`. |
| `-shrinkdebugfile` | Trims an oversized log at startup: if the file is larger than 11 MB, only the last 10 MB are kept. It is enabled by default as long as no `-debug` category is active. |

A container image writes the log to stdout, so `docker logs -f <container>`
shows the same content. See [Docker](../installation/docker.md).

## Log levels and categories

Without `-debug` the node logs errors, warnings and the regular progress
messages. With `-debug` it additionally logs the internal detail of the
selected categories:

```text
-debug=<category>          # enable one category
-debug=net -debug=mempool  # enable several categories
-debug=1                    # enable all categories
-debug=1 -debugexclude=net # enable everything except one category
```

An unknown category is reported at startup as
`Unsupported logging category <name>=<value>.` and is ignored. The complete
set of categories is:

| Category | Content |
| --- | --- |
| `net` | Peer connections, handshakes, header and block relay. |
| `tor`, `i2p`, `proxy` | Anonymity network and proxy handling. |
| `mempool`, `mempoolrej` | Transaction admission, eviction and rejection reasons. |
| `rpc`, `rpccache`, `http` | JSON-RPC and REST request handling. |
| `bench` | Benchmark timings of expensive operations. |
| `zmq` | Notification publishing. |
| `db`, `coindb`, `leveldb` | Database access and batch statistics. |
| `reindex` | Progress of a reindex or a chainstate rebuild. |
| `prune` | Block pruning. |
| `addrman` | Address manager and peer selection. |
| `connect` | Outbound connection attempts. |
| `selectcoins` | Coin selection for wallet transactions. |
| `cmpctblock` | Compact block negotiation. |
| `rand`, `estimatefee` | Randomness and fee estimation. |
| `staking` | Masternode staking and reward handling. |
| `anchoring`, `spv` | Anchor and SPV related operations. |
| `oracle`, `loan`, `accountchange`, `futureswap`, `tokensplit`, `swapresult`, `icxbug`, `accountconsolidate` | Protocol subsystems. |
| `ocean` | The Ocean indexer. |
| `customtxbench` | Timings of custom transaction handling. |

The category list can also be queried at runtime:

```bash
defi-cli logging            # all categories with their active state
defi-cli logging '["net"]'  # switch the net category on
defi-cli logging '["net"]' "exclude"
```

Three formatting options help when the log is read by several people at
once: `-logthreadnames` prefixes every line with the name of the originating
thread, `-logtimemicros` adds microsecond precision to the timestamp and
`-logips` includes IP addresses. Timestamps are on by default; thread names
and IP addresses are off by default, because both make the log noisier and
the IP addresses are personal data.

## Triage checklist

Work through the list in order and stop at the first step that explains the
symptom.

1. **Is the process alive?** Check the service status (`systemctl status
   defid`) or the exit code. A node that exits during startup writes the
   reason to the log before it stops.
2. **What is the tip?** `defi-cli getblockchaininfo` reports `blocks`,
   `headers` and `initialblockdownload`. If `blocks` stops growing while
   `headers` grows, the node is downloading but not connecting the blocks —
   see [Sync and reindex](sync-and-reindex.md).
3. **Are peers healthy?** `defi-cli getpeerinfo` shows the number of peers,
   their `synced_headers` and `synced_blocks` values and whether they are
   inbound. `defi-cli getnetworkinfo` shows the total number of connections.
4. **Is the node out of disk space?** `df -h` on the data directory. A full
   disk produces database write errors and a node that cannot advance.
5. **Is the version current?** `defi-cli getnetworkinfo` reports the
   `subversion` of the node; compare it with the release notes. A node that
   is too old is rejected by its peers and stops syncing.
6. **What does the log say?** Search the last few hundred lines for `Error`,
   `Failed`, `Corrupted` and `Warning`. The first error in the log is
   usually the cause; everything after it is a consequence.

```bash
# The last error lines of the log, newest last.
grep -nE "Error|Failed|Corrupted|Warning" <datadir>/debug.log | tail -20
```

## RPCs that answer most questions

| RPC | Question it answers |
| --- | --- |
| `getblockchaininfo` | Tip height, header height, IBD flag, verification progress, pruning state and the masternode version gate height. |
| `getnetworkinfo` | Connections, version, subversion, network and local address. |
| `getpeerinfo` | Per-peer state: header and block sync, inbound/outbound, ban score. |
| `getnettotals` | Traffic counters and the current bandwidth limit. |
| `getmempoolinfo` | Number and size of the mempool, the minimum fee. |
| `getchaintips` | The active chain and the forks that branch off it. |
| `getrawmempool` | The transaction identifiers in the mempool. |
| `getmininginfo` | Whether the node is mining and the masternode state. |
| `logging` | The log categories and their active state. |

The [RPC and CLI reference](../rpc-cli/index.md) explains how to call them.

## Symptom to procedure

| Symptom | Go to |
| --- | --- |
| The tip does not move, `headers` grows | [Sync and reindex](sync-and-reindex.md) |
| The node exits at startup with a database error | [Sync and reindex](sync-and-reindex.md) |
| The tip is stuck directly above a version-gated height | [Masternode stalls](masternode-stalls.md) |
| The node has no peers at all | [Masternode stalls](masternode-stalls.md) |
| The problem only appears under load | [Local testing and isolation](local-testing-isolation.md) |

## Next steps

* [Operations](../operations/index.md) — running the node as a service and
  the health checks that should alert you before a stall becomes visible.
* [Configuration](../configuration/index.md) — the options that influence
  syncing and disk usage.
* [Masternodes](../operations/masternodes.md) — the masternode lifecycle that
  the version gate depends on.
