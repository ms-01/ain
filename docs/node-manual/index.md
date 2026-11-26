# Node Software Manual

The **Node Software Manual** is written for the people who run DeFiChain nodes:
full nodes, masternodes, explorers and exchange or wallet infrastructure. It
covers everything between "I have a machine" and "the node is running,
configured and observable in production".

Part of the canonical documentation suite defined in
[ADR-0002](../developer-manual/adr/ADR-0002-documentation-structure.md).

## Who this manual is for

You need this manual if you:

* install `defid` on a server and want it to survive a reboot,
* decide how much disk, RAM and bandwidth the node may consume,
* expose or protect the JSON-RPC, ETH JSON-RPC, gRPC and WebSocket interfaces,
* tune the node for a slow link or a small VPS,
* automate the node through `defi-cli`, the JSON-RPC API or ZMQ.

If you only want to *use* DeFiChain — tokens, liquidity pools, vaults, loans,
staking — read the [User & Protocol Manual](../user-manual/index.md) instead.
If you want to *change* the node software, read the
[Developer & Architecture Manual](../developer-manual/index.md).

## Sections

| Section | What you learn |
| --- | --- |
| [Installation](installation/index.md) | System requirements, release verification, binary and Docker installation, the upgrade checklist and the layout of the data directory. |
| [Configuration](configuration/index.md) | The `defi.conf` file, the precedence of command-line options, the networks and their ports, P2P networking with Tor and I2P, pruning, memory and traffic tuning and the initial block download. |
| [Operations](operations/index.md) | Running the node as a service, masternode lifecycle, backups, monitoring and the REST / JSON-RPC / ZMQ interfaces. |
| [RPC & CLI Reference](rpc-cli/index.md) | The complete `defi-cli` and JSON-RPC command reference, including the MetaChain EVM endpoints. |
| [Troubleshooting](troubleshooting/index.md) | Log analysis, resynchronisation and reindexing, sync stalls and safe recovery procedures. |

## The binaries

A DeFiChain release ships four executables:

| Binary | Purpose |
| --- | --- |
| `defid` | The node daemon. It validates the chain, serves the RPC interfaces and participates in the peer-to-peer network. |
| `defi-cli` | The command-line client. It talks to a running `defid` over JSON-RPC. |
| `defi-tx` | Creates, signs and broadcasts raw transactions without a running node. |
| `defi-wallet` | Creates and manages offline wallet files. |

`defid` is the only process that has to run continuously; `defi-cli` is a
short-lived client that connects to it.

## Quick start

```bash
# 1. Download and unpack a release (see "Installation" for the full procedure).
tar -xzf defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz

# 2. Start the node. It creates its data directory on the first run.
#    -server=1 is required if you want to talk to the node over JSON-RPC.
./defichain-4.2.7/bin/defid -server=1

# 3. Check that it is running and synchronised.
./defichain-4.2.7/bin/defi-cli getblockchaininfo
```

On the first start the node creates the data directory
(`~/.defi` on Linux, `~/Library/Application Support/DeFi` on macOS,
`%APPDATA%\DeFi Blockchain` on Windows) and downloads the block headers
first, then the blocks. The process is called the *initial block download*
(IBD) and is explained in
[Configuration → Performance and IBD](configuration/performance-ibd.md).

## Networks and default ports

The node always runs on exactly one network. The network is selected on the
command line (`-testnet`, `-changi`, `-devnet`, `-regtest`) and determines the
default ports and the subdirectory of the data directory:

| Network | Option | P2P | JSON-RPC | gRPC | ETH JSON-RPC | ETH WebSocket | Data subdirectory |
| --- | --- | --- | --- | --- | --- | --- | --- |
| mainnet | *(default)* | 8555 | 8554 | 8550 | 8551 | 8553 | *(data directory root)* |
| testnet | `-testnet` | 18555 | 18554 | 18550 | 18551 | 18553 | `testnet3/` |
| changi | `-changi` | 20555 | 20554 | 20550 | 20551 | 20553 | `changi/` |
| devnet | `-devnet` | 21555 | 21554 | 21550 | 21551 | 21553 | `devnet/` |
| regtest | `-regtest` | 19555 | 19554 | 19550 | 19551 | 19553 | `regtest/` |

The values are defined in `src/chainparamsbase.cpp` (RPC, gRPC, ETH JSON-RPC
and WebSocket ports) and `src/chainparams.cpp` (P2P port, DNS seeds). The
`-changi-bootstrap` and `-devnet-bootstrap` options select the bootstrap
variant of those networks: they move the P2P, JSON-RPC, gRPC and ETH ports
into the testnet port range (`18555`/`18554`/`18550`/`18551`/`18553`) and
change the post-AMK message start, while the data directory keeps its own
subdirectory. The bootstrap variants additionally use a different DNS seed —
`-changi-bootstrap` adds `changi-seed.mydefichain.com`, `-devnet-bootstrap`
adds `testnet-seed.mydefichain.com`.

!!! warning
    The JSON-RPC, ETH JSON-RPC, gRPC and WebSocket listeners bind to
    `127.0.0.1` by default. They are only reachable from outside the machine
    if you pass `-rpcallowip` together with an explicit bind address — see
    [Configuration → Networking and privacy](configuration/networking-privacy.md).

## How this manual is organised

The sections follow the life cycle of a node:

1. **Installation** — get the software onto the machine and check the
   download.
2. **Configuration** — decide what the node does: which network, how much
   disk and RAM it may use, who may talk to it.
3. **Operations** — run it, monitor it, back it up, keep it alive.
4. **RPC & CLI Reference** — automate it.
5. **Troubleshooting** — fix it when it misbehaves.

Every flag, port and file path in this manual is verified against the
`master` branch of this repository. If a behaviour is gated by an activation
height, the height is stated explicitly and linked to
[Consensus](../developer-manual/consensus/index.md).
