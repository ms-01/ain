# Masternode stalls

Diagnose and recover from a chain that cannot advance because the
masternodes do not agree, and from a node that lost all its peers.

Part of the [Node Software Manual](../index.md).

## The masternode version gate

From a certain height on, every block must signal that the producer runs
software with the balance overflow fix. The signal is a version bit in the
block header, and the height at which it becomes mandatory is a consensus
parameter, `nMNVersionGateHeight`.

| Network | Version gate height |
| --- | --- |
| mainnet | 6564035 |
| testnet | disabled (`INT_MAX`) |
| changi | disabled (`INT_MAX`) |
| devnet | disabled (`INT_MAX`) |
| regtest | 10000000 |

A block below the gate height is accepted with or without the version bit.
From the gate height on, a block without the bit is rejected:

```text
rejected outdated nVersion=0x... block at height ... (missing overflow fix signal)
```

The node reports the gate in `getblockchaininfo`:

```bash
defi-cli getblockchaininfo | grep masternode_version_gate
```

* `masternode_version_gate_height` — the height at which the gate applies.
* `masternode_version_gate_active` — `true` when the next block is already
  subject to the gate.

## Recognising a version-gated stall

A version-gated stall has a characteristic signature: the tip is stuck
directly below the gate height and the node does not accept the headers that
would move it past the gate.

```bash
defi-cli getblockchaininfo | grep -E '"blocks"|masternode_version_gate'
```

The log contains an operator diagnostic that is printed at most every ten
minutes. It only appears once the chain has made no progress for longer
than the maximum tip age (`DEFAULT_MAX_TIP_AGE`, 36000 seconds by default,
overridable with `-maxtipage`):

```text
Masternode version gate stall detected: no chain progress for 43200 seconds
(tip height 6564034, next block height 6564035, masternode version gate
height 6564035, required version bit 0x01000000). Producing block 6564035
requires a quorum of masternodes running gate-capable software, this node
cannot advance the chain on its own. Version-gate rejected headers so far: 42
```

With the `net` category enabled, the rejected headers of the individual peers
are logged as well:

```bash
grep "masternode version gate" <datadir>/debug.log
```

```text
peer=12: header rejected by the masternode version gate, peer software is too old: bad-version(0x20000000)
```

The counter at the end of the diagnostic is the number of headers that were
rejected for this reason since the node was started.

!!! note
    A peer whose header is rejected for this reason is **not** punished and
    not banned. It is a version skew and not misbehaviour; banning it would
    isolate the node from the whole network.

## Recovery

The chain can only advance when a quorum of the masternodes produces blocks
with the required version bit. The recovery is therefore an upgrade of the
masternodes, not a change on the single node that observes the stall.

1. **Find out which height is affected.**

   ```bash
   defi-cli getblockchaininfo | grep masternode_version_gate
   ```

   If the gate is still in the future, the stall has another cause — see
   [Sync and reindex](sync-and-reindex.md).

2. **Check the masternodes.** `defi-cli listmasternodes` lists the registered
   masternodes with their state; `defi-cli getmasternodeblocks` shows the
   blocks that the masternodes have produced. A masternode that does not run
   the gate-capable software cannot produce a valid block above the gate.

3. **Upgrade the masternodes.** Every masternode that produces blocks above
   the gate height has to run a release that sets the version bit. The
   [Masternodes](../operations/masternodes.md) page describes the operator
   setup and the health checks of a masternode.

4. **Verify the local node.** The version bit is part of the block header, so
   it is visible without any special tooling:

   ```bash
   defi-cli getblockhash <height>
   defi-cli getblockheader <hash> | grep version
   ```

5. **Watch the log.** As soon as a quorum of upgraded masternodes produces
   blocks, the tip moves again and the stall diagnostic stops.

### Automatic reset of a legacy chain

If the active chain contains a block at or above the balance overflow height
that does not carry the version bit, the node rewinds the chain by itself.
The option `-autoreset` is enabled by default:

```text
Auto-Reset: Legacy chain detected at height 6564035 without overflow fix
signal (block ...). Automatically invalidating...
Auto-Reset: Successfully rewound active chain to height 6564034
```

The mempool of the legacy chain is discarded in this case, because its
transactions would be orphaned by the rewind. The rewound chain is then
rebuilt from the network with the corrected software.

Use `-noautoreset` to disable the behaviour if you need to inspect the
legacy chain first.

## Peer isolation

Peer isolation is the opposite problem: the node has no usable peers and
therefore cannot learn about new blocks, even though the network is healthy.

### Symptoms

```bash
defi-cli getnetworkinfo | grep -E '"connections"|"connections_in"'
defi-cli getpeerinfo
```

* `connections` is 0 and `connections_in` is 0 — the node is not connected
  to anybody.
* All peers show a low `synced_headers` — the node only talks to peers that
  are themselves far behind.
* The log contains `Potential stale tip detected, will try using extra
  outbound peer` — the node has peers, but none of them has the chain.

### Causes and fixes

| Cause | Fix |
| --- | --- |
| The P2P port is blocked by the firewall or the NAT | Open the port of the selected network; see [Networking and privacy](../configuration/networking-privacy.md). |
| The node runs a version that the network rejects | Upgrade the node; peers disconnect after the version handshake. |
| `-connect` or `-onlynet` restricts the node | Remove or correct the restriction in the configuration file. |
| `-dnsseed=0` on a node that should find peers on its own | Enable the DNS seeds or add a `-seednode`. |
| The node is in the masternode version gate stall and therefore has no valid blocks to serve | See [Recovery](#recovery). |
| Too many incoming connections fill the connection slots | Raise `-maxconnections` (default 125) or restrict the incoming peers with `-whitebind`. |

### Recovering an isolated node

The simplest recovery is to let the node find the network again:

```bash
# 1. Remove a stale or wrong seed configuration and restart the service.
sudo systemctl restart defid

# 2. Watch the connections while they are established.
defi-cli -rpcwait getpeerinfo
```

If the node still has no peers, connect it to a known node manually. Ask the
operator of that node for its address and add it to the configuration:

```text
[main]
seednode=<address of a reachable node>
```

After the restart the log shows the handshake and the sync progress:

```text
New outbound peer connected: version: 70040, blocks=6564034, peer=12
```

## Next steps

* [Masternodes](../operations/masternodes.md) — the operator setup and the
  minting checks of a masternode.
* [Sync and reindex](sync-and-reindex.md) — stalls that are not caused by
  the masternodes.
* [Operations](../operations/index.md) — the health checks that detect a
  stalled node early.
