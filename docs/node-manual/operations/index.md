# Operations

Running the node in production: as a service, under supervision, watched by
health checks and a monitoring system.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| [Masternodes](masternodes.md) | Owner and operator roles, collateral, registration, timelocks, activation and minting checks. |
| [RPC and REST interfaces](rpc-rest-interfaces.md) | JSON-RPC authentication and binding, batch requests, the ETH JSON-RPC listener and the REST endpoints. |
| [ZMQ notifications](zmq-notifications.md) | Publishing blocks and transactions to ZeroMQ subscribers. |
| [Backups and signers](backups-signers.md) | Backing up wallets and the data directory, restoring them and using an external signer. |

## Running the node as a service

A node that is operated for months has to survive reboots, power cuts and
package upgrades. Use an init system for that; the repository ships a
reference unit for systemd and scripts for OpenRC and System V / CentOS.

### systemd

The reference unit is `contrib/init/defid.service`. It runs the daemon as the
unprivileged user `defi` and keeps the configuration, the state and the runtime
files in separate directories:

| Path | Content | Created by |
| --- | --- | --- |
| `/etc/defi/defi.conf` | The configuration file. | `ConfigurationDirectory=defi` |
| `/var/lib/defi` | The data directory: blocks, chain state, wallets, EVM state. | `StateDirectory=defi` |
| `/run/defid` | The PID file of the daemon. | `RuntimeDirectory=defid` |

Install it on a Debian or Ubuntu system:

```bash
# 1. Create the system user and the directories of the unit.
sudo useradd --system --home /var/lib/defi --shell /usr/sbin/nologin defi
sudo install -d -o defi -g defi -m 0710 /etc/defi /var/lib/defi /run/defid

# 2. Install the unit and the default configuration.
sudo install -m 0644 contrib/init/defid.service /etc/systemd/system/defid.service
sudo install -m 0640 -o defi -g defi /dev/null /etc/defi/defi.conf

# 3. Enable and start the service.
sudo systemctl daemon-reload
sudo systemctl enable --now defid.service
```

The unit runs `defid` with `-daemon`, `-pid=/run/defid/defid.pid`,
`-conf=/etc/defi/defi.conf` and `-datadir=/var/lib/defi`. Options that are
given on the command line override the ones in the configuration file, so put
everything you want to change into `/etc/defi/defi.conf` and keep the unit
untouched. If you have to change the unit itself, use a drop-in instead of
editing the installed file — it is overwritten on package upgrades:

```bash
sudo systemctl edit defid.service
```

The unit also applies a set of hardening options (`PrivateTmp`,
`ProtectSystem=full`, `ProtectHome`, `NoNewPrivileges`, `PrivateDevices`,
`MemoryDenyWriteExecute`). If you bind a data directory or a socket from
outside the service directories, relax the corresponding option with a
drop-in.

Check the service and its log:

```bash
sudo systemctl status defid.service
sudo journalctl -u defid.service -n 200 --no-pager
```

### OpenRC

`contrib/init/defid.openrc` and `contrib/init/defid.openrcconf` provide the
equivalent setup for Alpine Linux and other OpenRC systems. Install the service
script into `/etc/init.d/defid`, copy the configuration into
`/etc/conf.d/defid` and start the service with `rc-service defid start`.

### System V / CentOS

`contrib/init/defid.init` is the System V init script for CentOS and similar
distributions. It sources the function library `/etc/init.d/functions` and
reads its configuration from `/etc/sysconfig/defid`. Install the script into
`/etc/init.d/defid`, provide the configuration in `/etc/sysconfig/defid` and
start the service with `service defid start`.

### Docker

The container variant, including the volume layout and the way the RPC port is
published, is described in
[Installation → Docker](../installation/docker.md). A container needs a health
check and a restart policy for the same reason a bare-metal node needs systemd:

```yaml
services:
  defid:
    image: defi/defichain:latest
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "defi-cli", "getnodestatusinfo"]
      interval: 60s
      timeout: 10s
      retries: 3
```

### Stopping and restarting

Stop the node with a clean shutdown so that the chain state, the wallets and
the mempool are flushed to disk:

```bash
# Via the RPC interface.
./defi-cli stop

# Via the init system.
sudo systemctl stop defid.service
```

A clean shutdown matters for the backups in
[Backups and signers](backups-signers.md): an unclean stop can leave the
wallet file and the chain state out of sync.

## Health checks

The node answers three questions that every monitoring system should ask:
*is the process alive*, *is the chain state consistent* and *is the node
connected to the network*.

```bash
# 1. Is the process alive and answering at all?
./defi-cli uptime

# 2. Is the node synchronised with the chain tip and connected to peers?
./defi-cli getnodestatusinfo
```

`getnodestatusinfo` returns the three flags that the node itself uses for its
health evaluation:

```json
{
  "health_status": true,
  "sync_to_tip": true,
  "active_peer_nodes": true
}
```

* `sync_to_tip` is true when the height of the active chain equals the height
  of the best known header.
* `active_peer_nodes` is true when the node is connected to at least the
  minimum number of peers that sent data recently
  (`DEFAULT_MINIMUM_ACTIVE_NODE_PEERS` and
  `DEFAULT_ACTIVE_PEER_CONNECTION_TIMEOUT` in `src/rpc/net.cpp`).
* `health_status` is the conjunction of both.

For a detailed picture use the individual RPCs:

```bash
# Chain state and synchronisation progress.
./defi-cli getblockchaininfo
./defi-cli getchaintips

# Peer connections and the local network view.
./defi-cli getnetworkinfo
./defi-cli getpeerinfo

# Mempool, wallet and the process itself.
./defi-cli getmempoolinfo
./defi-cli getwalletinfo
./defi-cli uptime
```

A minimal shell health check for a cron job or a load balancer:

```bash
#!/usr/bin/env bash
set -euo pipefail

CLI="/usr/bin/defi-cli -datadir=/var/lib/defi"

status="$(${CLI} getnodestatusinfo)"
if ! grep -q '"health_status": true' <<<"${status}"; then
    echo "defid is not healthy: ${status}" >&2
    exit 1
fi

echo "defid is healthy"
```

## Monitoring metrics

The node exposes everything you need as JSON-RPC. Export the values below
with the exporter of your choice (Prometheus, OpenTelemetry, a plain cron
script) and alert on them.

| Metric | Source | What to watch |
| --- | --- | --- |
| Chain height | `getblockchaininfo` → `blocks` | The height must grow roughly every 30 seconds. A flat height for more than a few minutes means the node is stuck or disconnected. |
| Header height | `getblockchaininfo` → `headers` | `headers` must not be smaller than `blocks`. A growing gap means the initial block download is still running or stalled. |
| Verification progress | `getblockchaininfo` → `verificationprogress` | Below `1.0` while `blocks` is behind `headers` means the node is still catching up. |
| Peer connections | `getnetworkinfo` → `connections`, `connections_in`, `connections_out` | Zero inbound connections on a public node means you are not reachable. |
| Node health | `getnodestatusinfo` → `health_status` | The single flag to use for an availability alert. |
| Mempool size | `getmempoolinfo` → `size`, `bytes` | A mempool that stays close to `maxmempool` for a long time means the node cannot relay transactions. |
| Wallet balance | `getwalletinfo` → `balance` | For a masternode operator, an unexpected drop of the balance is worth an alert. |
| Peer traffic | `getpeerinfo` → `bytesrecv`, `bytessent` | Flat counters mean the peer is stale even though the connection count looks healthy. |
| Local services | `getnetworkinfo` → `localservices` | The bitmask of the enabled services; a change after a restart is worth a review. |
| Process uptime | `uptime` | A short uptime in combination with a growing `blocks` value points at a restart loop. |

!!! tip
    Keep the health check cheap. `getnodestatusinfo` reads two in-memory
    counters; `getblockchaininfo` and `getpeerinfo` are heavier and are better
    suited to a scrape interval of one minute than to a five-second probe.

## Logs

The node writes a log file `debug.log` into the data directory
(`<datadir>/debug.log`, `<datadir>/testnet3/debug.log` and so on). The
location and the behaviour are controlled by these options:

| Option | Effect |
| --- | --- |
| `-debuglogfile=<file>` | Writes the log to another file. A relative path is resolved inside the network-specific data directory. |
| `-nodebuglogfile` | Disables the log file completely. The log then only goes to the console. |
| `-printtoconsole` | Also prints to the console. The default is `1` when the node is not started with `-daemon`. |
| `-logtimestamps` | Prefixes every line with a timestamp. Enabled by default. |
| `-logips` | Includes IP addresses in the log. Disabled by default. |
| `-debug=<category>` | Enables the debug output of one or more categories, for example `-debug=net`. |

With systemd the log is additionally available in the journal, because
`-printtoconsole` is on for a foreground start:

```bash
sudo journalctl -u defid.service -f
```

The log file grows without a limit. Rotate it with `logrotate` and a copy
truncate (`copytruncate`), because the node keeps the file open and does not
react to a rename:

```text
/var/lib/defi/debug.log {
    daily
    rotate 14
    compress
    missingok
    notifempty
    copytruncate
}
```

## Upgrades in production

The upgrade procedure itself is described in
[Installation → Upgrade checklist](../installation/index.md). For a node that
runs unattended, add these steps:

1. Check the release notes for an activation height. A release with a new
   activation height is only safe when every node in your cluster runs the
   same version — see
   [Consensus](../../developer-manual/consensus/index.md).
2. Take a backup of the wallets and the configuration *before* the upgrade
   (see [Backups and signers](backups-signers.md)).
3. Stop the service, replace the binaries, start the service again.
4. Verify the version and the synchronisation:

   ```bash
   ./defi-cli getnetworkinfo | grep -E 'subversion|connections'
   ./defi-cli getblockchaininfo | grep -E 'blocks|headers'
   ```

5. For a masternode, check that the node still mints after the restart — see
   [Masternodes](masternodes.md).

## Next steps

* [Masternodes](masternodes.md) — the operator role, collateral and minting.
* [RPC and REST interfaces](rpc-rest-interfaces.md) — how to reach the node
  from another machine.
* [Backups and signers](backups-signers.md) — protecting the keys.
