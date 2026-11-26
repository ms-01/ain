# Installation

This section gets the DeFiChain node software onto a machine and describes what
the node does with the disk afterwards.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| [Binaries](binaries.md) | Where to get the release, how to verify it, and how to install and run it on Linux, macOS and Windows. |
| [Docker](docker.md) | Running the node in a container: image, volumes, ports and remote authentication. |
| [Data directory](data-directory.md) | What the node writes to disk, where the EVM state lives and what is safe to delete. |

## System requirements

DeFiChain is a proof-of-stake chain: a node validates blocks and participates
in the peer-to-peer network, it does not mine. The requirements are therefore
those of a validating node, not of a mining rig.

### Hardware

| Resource | Minimum | Recommended | Notes |
| --- | --- | --- | --- |
| CPU | 2 cores | 4 cores | Script verification is capped at 4 additional threads (`-par`), see [Performance and IBD](../configuration/performance-ibd.md). |
| RAM | 2 GB | 8 GB | The defaults alone reserve 512 MiB for the UTXO cache (`-dbcache`) and 300 MB for the mempool (`-maxmempool`). A wallet, an EVM workload or a masternode needs considerably more. |
| Disk | 50 GB | 100 GB and more, on SSD | Block files are 128 MiB each and the EVM state grows continuously. A pruned node needs far less, an archival node with `-txindex` considerably more. |
| Network | 1 Mbit/s | 100 Mbit/s | The initial block download is the only phase that saturates the link. |

The chain parameters carry an *assumed* blockchain size of 240 MB for mainnet
and 30 MB for the test networks (`m_assumed_blockchain_size` in
`src/chainparams.cpp`). This value is used for internal estimates only — it is
**not** the disk requirement of a node. Plan for the real, growing size of the
block files and of the `<datadir>/evm` directory.

!!! tip
    A pruned node (`-prune=550` or higher) keeps the chain state and the most
    recent blocks only. It is the right choice for a node that serves
    ordinary wallet and RPC traffic. It is the wrong choice if you need to
    serve historical blocks to other nodes or to answer `getblock` for old
    heights.

### Operating systems

| Platform | Supported | Notes |
| --- | --- | --- |
| Linux | x86_64 and aarch64 | The primary platform; the systemd unit in `contrib/init/` is the reference setup. |
| macOS | x86_64 and aarch64 | The release binaries are not notarized, so Gatekeeper can block the first start; remove the quarantine attribute once (`xattr -d com.apple.quarantine defid`). |
| Windows | x86_64 | The daemon runs as a normal Windows program; the service installation is described in [Binaries](binaries.md). |

DeFiChain is a proof-of-stake chain with a 30-second block time, so a node
needs to be online and with a correct clock. Run an NTP client on the host.

## Choosing a release

Releases are published on the
[GitHub releases page](https://github.com/DeFiCh/ain/releases). Every release
contains one archive per build target:

| Target | Archive | Format |
| --- | --- | --- |
| `x86_64-pc-linux-gnu` | `defichain-<version>-x86_64-pc-linux-gnu.tar.gz` | tarball |
| `aarch64-linux-gnu` | `defichain-<version>-aarch64-linux-gnu.tar.gz` | tarball |
| `x86_64-w64-mingw32` | `defichain-<version>-x86_64-w64-mingw32.zip` | zip archive |
| `x86_64-apple-darwin` | `defichain-<version>-x86_64-apple-darwin.tar.gz` | tarball |
| `aarch64-apple-darwin` | `defichain-<version>-aarch64-apple-darwin.tar.gz` | tarball |

Each archive is accompanied by a `.SHA256` checksum file.

## Verifying a download

!!! warning
    Verify the checksum before you execute anything from the archive. A
    modified `defid` binary controls your wallet and your keys.

```bash
# 1. Compute the digest of the archive you downloaded.
sha256sum defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz

# 2. Show the digest that the release published.
cat defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz.SHA256
```

Both commands print a digest in the form
`<hex digest>  <file name>`. The digests must be identical. On macOS use
`shasum -a 256 defichain-4.2.7-x86_64-apple-darwin.tar.gz` instead of
`sha256sum`.

!!! warning
    Do not use `sha256sum --check` (or `shasum -a 256 -c`) on the published
    `.SHA256` files. The release build writes them with the absolute path of
    the build machine (`make.sh` calls `sha256sum "${pkg_path}"` on the
    canonicalised build path, e.g.
    `/__w/ain/ain/build/defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz`). The
    check therefore looks for a file that does not exist on your machine and
    aborts with "No such file or directory". Compare the two digests
    manually instead.

After unpacking, check the version of the binary:

```bash
./defichain-4.2.7/bin/defid -version | head -1
```

The output contains the client version (`4.2.7`), the build target and the
commit hash the binary was built from.

## Upgrade checklist

An upgrade replaces the binaries while the data directory stays in place. Work
through the list in order.

1. **Read the release notes.** They list the consensus-relevant changes of the
   release. A release that contains an activation height is only safe once
   every node you connect to runs the same version.
2. **Check the masternode version gate.** Masternodes have to signal a
   specific version bit from the version gate height onwards. On mainnet that
   height is `6564035` (`nMNVersionGateHeight` in `src/consensus/params.h`); a
   masternode that stays on an old version stops minting. See
   [Consensus](../../developer-manual/consensus/index.md).
3. **Stop the node** and wait until it has shut down cleanly:

   ```bash
   ./defichain-4.2.7/bin/defi-cli stop
   ```

   A clean shutdown is important: it flushes the chain state, the wallet and
   the mempool to disk. See [Data directory](data-directory.md) for what to
   back up.
4. **Back up the data directory** (at least the wallet files and `defi.conf`).
5. **Install the new binaries** next to the old ones and switch the symlink or
   the service unit to the new directory. Do not delete the old directory
   until the new version has run successfully — see the recovery notes in
   [Troubleshooting](../troubleshooting/index.md).
6. **Start the node** and verify that it catches up:

   ```bash
   ./defichain-4.2.7/bin/defi-cli getblockchaininfo | grep -E 'blocks|headers'
   ./defichain-4.2.7/bin/defi-cli getnetworkinfo | grep connections
   ```

7. **Check the log** for errors. `debug.log` in the data directory records
   every startup; the `-debug` option selects additional categories.

!!! note
    Downgrading is not supported. A node that has run a newer version writes a
    data format that older versions cannot read. If you have to go back,
    restore the backup from step 4 as well.

## Next steps

* [Binaries](binaries.md) — the actual installation procedure.
* [Docker](docker.md) — the containerised variant.
* [Configuration](../configuration/index.md) — the `defi.conf` file and the
  command-line options.
