# Local testing and isolation

Reproduce an operational problem in a throw-away environment instead of on a
production node.

Part of the [Node Software Manual](../index.md).

## When to use this guide

Use it when you want to reproduce one of the following before changing a
production node:

* a sync stall or a peer problem,
* a version-gated chain stall,
* a database problem that requires a rebuild,
* a masternode setup problem,
* a configuration change that is not yet proven.

The rule is simple: **the reproduction never touches the data directory of a
real network.** Every node gets its own data directory, and every node runs
on regtest (or mocknet) unless a real network is genuinely required.

## The regtest network

Regtest is a private network for tests. It is selected with `-regtest` and
differs from the other networks in a few important ways:

| Property | Regtest |
| --- | --- |
| Block difficulty | Minimum difficulty is allowed and the difficulty never retargets, so blocks can be produced instantly. |
| P2P port | 19555 |
| JSON-RPC port | 19554 |
| Data directory | `<datadir>/regtest` |
| EVM chain ID | 1133 |
| Masternode version gate height | 10000000 |
| Balance overflow height | 1 |
| Masternode activation delay | 10 blocks (`consensus.mn.activationDelay`) |
| New masternode activation delay | 20 blocks (`consensus.mn.newActivationDelay`) |

Regtest has no connection to any public network, so a regtest node can never
sync from mainnet and cannot interfere with it.

## A single isolated node

Create a data directory that is used by nothing else and start the node
without any outbound connection:

```bash
# A dedicated directory for this experiment.
mkdir -p /tmp/defi-lab/regtest

# Start the node: no peers, no DNS seeds, no incoming connections.
./defid -regtest \
  -datadir=/tmp/defi-lab/regtest \
  -server=1 \
  -listen=0 \
  -noconnect \
  -dnsseed=0 \
  -printtoconsole=0 \
  -debug=net -debug=mempool
```

* `-listen=0` — the node does not accept incoming connections.
* `-noconnect` — the node does not open outbound connections either, so it
  cannot reach the public network.
* `-dnsseed=0` — no DNS seed is queried for peers.
* `-datadir` — a directory that is used by this experiment only. Never point
  it at the data directory of a mainnet node.

The log of the node is `<datadir>/regtest/debug.log`. Talk to the node with
the client:

```bash
./defi-cli -regtest -datadir=/tmp/defi-lab/regtest -rpcwait getblockchaininfo
```

### Producing blocks

Regtest mines instantly, so the chain can be advanced on demand. A node
starts without a wallet, so create one first:

```bash
CLI="./defi-cli -regtest -datadir=/tmp/defi-lab/regtest"

# A wallet for the experiment.
$CLI createwallet lab

# A fresh address to receive the coinbase.
ADDR=$($CLI -rpcwallet=lab getnewaddress)

# Ten blocks, instantly.
$CLI generatetoaddress 10 "$ADDR"
```

### Cleaning up

```bash
# Stop the node, then remove the directory.
./defi-cli -regtest -datadir=/tmp/defi-lab/regtest stop
rm -rf /tmp/defi-lab
```

A regtest data directory is disposable. When in doubt, delete it and start
again — with one exception: a data directory that was created with
`-fastprune` uses a different block file layout and must not be reused
without that option, and the other way round.

## Several nodes at once

For problems that involve more than one node — peer synchronisation, a
reorganisation, a masternode quorum — run a small cluster. Each node needs
its own data directory and its own ports:

```bash
for i in 0 1 2; do
  mkdir -p /tmp/defi-lab/node$i
  ./defid -regtest \
    -datadir=/tmp/defi-lab/node$i \
    -server=1 \
    -listen=1 \
    -bind=127.0.0.1 \
    -port=$((19555 + i)) \
    -rpcport=$((19554 + i)) \
    -rpcallowip=127.0.0.1 \
    -printtoconsole=0 &
done
```

Connect the nodes explicitly, so that no node finds a peer on the public
network:

```bash
./defi-cli -regtest -datadir=/tmp/defi-lab/node0 -rpcport=19554 \
  addnode "127.0.0.1:19556" onetry
```

!!! note
    `-rpcallowip` is only needed for clients that are **not** on the local
    machine. Without it the RPC server is still started and reachable from
    the loopback interface: the node always allows the local IPv4 subnet and
    `::1`, and it falls back to the loopback bind. So the option is not
    required for `defi-cli` on the same host — and it should never be
    widened beyond the loopback interface on a shared machine.

## The functional test framework

The repository contains a test framework that starts a cluster of regtest
nodes, waits for them to be ready and tears them down afterwards. Use it
when the reproduction should be repeatable and reviewable.

```bash
# A single test.
./test/functional/test_runner.py feature_auto_reset.py

# Several tests, in parallel, with the logs of the nodes on failure.
./test/functional/test_runner.py --jobs=4 --combinedlogslen=20 feature_*.py

# Use a fixed root directory for the temporary data directories.
./test/functional/test_runner.py --tmpdirprefix=/tmp/defi-lab feature_auto_reset.py
```

The runner creates a temporary directory per test and removes it afterwards.
To keep the data directories of a run for inspection, start the test script
directly — it accepts `--tmpdir=<directory>` and `--nocleanup`:

```bash
./test/functional/feature_auto_reset.py --tmpdir=/tmp/defi-lab --nocleanup
```

The framework writes the data directories of the nodes into that directory,
one per node, and each of them contains its own `regtest/debug.log`.

A test is a Python file that derives from `DefiTestFramework`; the file
`test/functional/example_test.py` is a commented example that uses both the
RPC and the P2P interface. `test/functional/README.md` describes the naming
conventions, the style rules and the modules of the framework.

## Reproducing a version-gated stall

The version gate height is a consensus parameter, but it can be overridden on
regtest and mocknet with `-mnversiongateheight`. This makes it possible to
reproduce a version-gated stall locally:

```bash
./defid -regtest \
  -datadir=/tmp/defi-lab/gate \
  -mnversiongateheight=10 \
  -server=1 -listen=0 -noconnect -dnsseed=0
```

Produce blocks past the gate with a build that does not signal the version
bit and the node rejects them:

```text
rejected outdated nVersion=0x... block at height 10 (missing overflow fix signal)
```

The same override exists for the balance overflow height
(`-balanceoverflowheight`), which is what the automatic reset of a legacy
chain is based on. On regtest the automatic reset is enabled by default; the
functional test `test/functional/feature_auto_reset.py` exercises it.

Because the override is only evaluated on regtest and mocknet, it cannot
change the behaviour of a mainnet node.

## Mocknet

Mocknet is a test network with a masternode foundation and a configurable
block time:

```bash
./defid -regtest -mocknet -mocknet-blocktime=30 -mocknet-key=<pubkey> ...
```

* `-mocknet-blocktime=<secs>` — the target time between two blocks.
* `-mocknet-key=<pubkey>` — adds an additional foundation member.
* The tip age limit is raised to ten years, so the node does not consider
  itself to be in initial block download.

The node prints a warning banner at startup, because mocknet is not a
production network.

## Containers

Containers are useful when the problem is environmental — a disk that fills
up, a filesystem that behaves differently, a network that only allows
certain traffic.

```bash
# One container per node, one volume per node.
docker run -d --name defi-lab-0 \
  -v /tmp/defi-lab/vol0:/data \
  -p 127.0.0.1:19554:19554 \
  defi/defichain:latest \
  -regtest=1 -server=1 -listen=0 -noconnect -dnsseed=0 -printtoconsole=0

docker exec defi-lab-0 defi-cli -regtest getblockchaininfo
```

Rules for container experiments:

* **Bind the ports to the loopback interface.** `-p 127.0.0.1:19554:19554`
  publishes the RPC port on the host, but only for the local machine. Never
  publish it on all interfaces, and never add `-rpcallowip` for a network
  you do not control.
* **One volume per node.** Two nodes that share a data directory corrupt
  each other's databases.
* **The image name is `defi/defichain:latest`.** The container writes the
  log to stdout, so `docker logs -f defi-lab-0` shows the same content as
  `debug.log`.
* **Keep the containers off the public network.** Use `-listen=0`,
  `-noconnect` and `-dnsseed=0` for a node that is only used locally.

The [Docker](../installation/docker.md) page describes the image and the
volume layout in detail.

## Rules for a safe experiment

1. One data directory per node, never a shared one.
2. Always select the network explicitly with `-regtest`.
3. Never reuse a data directory between a pruned and an unpruned node, and
   never between a `-fastprune` and a normal node.
4. Stop the nodes before you delete or copy a data directory.
5. Delete the experiment directory when the experiment is over.
6. Do not copy production keys, mn private keys or wallet files into an
   experiment.

## Next steps

* [Sync and reindex](sync-and-reindex.md) — the recovery procedures that
  you want to reproduce.
* [Masternode stalls](masternode-stalls.md) — the version gate and the peer
  isolation cases.
* [Building & testing](../../developer-manual/building-testing/index.md) —
  how the functional tests are executed in CI.
