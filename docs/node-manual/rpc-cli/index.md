# RPC & CLI Reference

The JSON-RPC interface of the node and the `defi-cli` commands that call it.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| [RPC command categories](categories/index.md) | The index of every JSON-RPC command, grouped by category. |
| [EVM JSON-RPC](evm-json-rpc.md) | The MetaChain ETH JSON-RPC endpoint: configuration, namespaces and methods. |

The command reference itself is **generated** from the binary, so it always
describes the node that produced it. See
[Regenerating the reference](#regenerating-the-reference).

## Using `defi-cli`

`defi-cli` sends JSON-RPC calls to a running `defid`. It is the fastest way
to inspect a node, to build transactions and to automate operations.

```bash
# List every command the node exposes.
defi-cli help

# Show the help of a single command.
defi-cli help getblock

# Call a command with positional arguments.
defi-cli getblockhash 1000000

# Call a command with named arguments.
defi-cli -named getblock '{"blockhash":"0000000000000000000000000000000000000000000000000000000000000000","verbosity":1}'
```

### Connecting to a node

| Option | Effect |
| --- | --- |
| `-datadir=<dir>` | Uses the data directory of the node, including its network subdirectory and the cookie file. |
| `-rpcconnect=<ip>` | Connects to another host. The default is `127.0.0.1`. |
| `-rpcport=<port>` | Uses another RPC port. The default depends on the network: `8554` (mainnet), `18554` (testnet), `20554` (changi), `21554` (devnet), `19554` (regtest). |
| `-rpcuser=<user>`, `-rpcpassword=<pw>` | Use static credentials instead of the cookie file. |
| `-rpccookiefile=<loc>` | Reads the credentials from another cookie file. |
| `-regtest`, `-testnet`, `-changi`, `-devnet` | Select the network. Without one of them the client talks to mainnet. |
| `-rpcwallet=<name>` | Calls a command of a specific wallet. |
| `-rpcwait` | Waits for the node to start before it sends the call. |
| `-rpcclienttimeout=<n>` | Sets the timeout of the HTTP request in seconds. The default is `900`. |
| `-stdinrpcpass` | Reads the RPC password from standard input, so it does not appear in the process list. |
| `-getinfo` | Prints a summary of the remote node. The values come from several requests and are not consistent with each other. |

The network selection and the port have to match the node. A `defi-cli` that
talks to the wrong network reports a connection error or an unexpected
result:

```bash
# A regtest node on the default regtest port.
defi-cli -regtest getblockcount

# The same node, addressed explicitly.
defi-cli -regtest -rpcconnect=127.0.0.1 -rpcport=19554 getblockcount
```

### Authentication

`defi-cli` reads the cookie file of the data directory automatically, so a
node that was started without `-rpcuser` and `-rpcpassword` is reachable
without any further option:

```bash
# Uses <datadir>/.cookie (or <datadir>/regtest/.cookie and so on).
defi-cli -regtest getnetworkinfo
```

When the node uses static credentials, pass them to the client:

```bash
defi-cli -regtest -rpcuser=operator -rpcpassword=<secret> getnetworkinfo
```

To keep the password out of the shell history and out of the process list,
read it from standard input:

```bash
defi-cli -regtest -rpcuser=operator -stdinrpcpass getnetworkinfo
```

The complete list of authentication methods — cookie file, `-rpcauth` and
static credentials — is described in
[Operations → RPC and REST interfaces](../operations/rpc-rest-interfaces.md).

### Batch requests

`defi-cli` sends one call per invocation. Several calls can be sent in a
single HTTP request with `curl`; the answers are returned in the order of the
requests:

```bash
curl --user operator:<secret> --data-binary '[
    {"jsonrpc":"1.0","id":"chain","method":"getblockchaininfo","params":[]},
    {"jsonrpc":"1.0","id":"network","method":"getnetworkinfo","params":[]}
]' -H 'content-type: text/plain;' http://127.0.0.1:8554/
```

Batch requests are not atomic: every call is executed on its own.

## Command categories

The node groups its commands into categories. Every category has its own
page with the help text of every command it contains.

| Category | Commands | Page |
| --- | --- | --- |
| Accounts | 25 | [accounts.md](categories/accounts.md) |
| Blockchain | 36 | [blockchain.md](categories/blockchain.md) |
| Control | 6 | [control.md](categories/control.md) |
| Evm | 5 | [evm.md](categories/evm.md) |
| Generating | 1 | [generating.md](categories/generating.md) |
| Icxorderbook | 10 | [icxorderbook.md](categories/icxorderbook.md) |
| Loan | 17 | [loan.md](categories/loan.md) |
| Masternodes | 9 | [masternodes.md](categories/masternodes.md) |
| Mempool | 1 | [mempool.md](categories/mempool.md) |
| Mining | 7 | [mining.md](categories/mining.md) |
| Network | 19 | [network.md](categories/network.md) |
| Oracles | 13 | [oracles.md](categories/oracles.md) |
| Poolpair | 11 | [poolpair.md](categories/poolpair.md) |
| Proposals | 7 | [proposals.md](categories/proposals.md) |
| Rawtransactions | 17 | [rawtransactions.md](categories/rawtransactions.md) |
| Signer | 1 | [signer.md](categories/signer.md) |
| Spv | 32 | [spv.md](categories/spv.md) |
| Stats | 2 | [stats.md](categories/stats.md) |
| Tokens | 8 | [tokens.md](categories/tokens.md) |
| Util | 7 | [util.md](categories/util.md) |
| Vault | 15 | [vault.md](categories/vault.md) |
| Wallet | 64 | [wallet.md](categories/wallet.md) |
| Zmq | 1 | [zmq.md](categories/zmq.md) |

The [generated category index](categories/index.md) contains the same list
with the command counts of the binary that produced it.

!!! note
    The list above covers the commands that the node publishes. Commands in
    the internal `hidden` category are not part of the public interface and
    are therefore not documented.

## Regenerating the reference

The pages below `categories/` are generated by
`contrib/devtools/gen-rpc-docs.py`. The script starts a temporary regtest
node, reads the help text of every command from it and renders the Markdown
pages:

```bash
# Build the node first, the script uses build/src/defid and build/src/defi-cli.
./make.sh build

# Regenerate the pages.
python3 contrib/devtools/gen-rpc-docs.py
```

The script writes to `docs/node-manual/rpc-cli/categories/` and removes pages
of categories that no longer exist. It never leaves a node running: the
temporary node is stopped before the script returns.

To verify that the committed pages still match the binary, use the check
mode. It exits with `1` and prints a diff when the documentation drifted:

```bash
python3 contrib/devtools/gen-rpc-docs.py --check
```

The check mode is meant for continuous integration: it fails the build when a
change adds, removes or rewords an RPC without updating the documentation.

When the generator adds a new category, add the new page to the navigation in
`mkdocs.yml` as well. The canonical-layout check
(`test/lint/check-docs-structure.py`) reports pages that are not wired into
the navigation, so an unwired page fails the documentation build.

If a node is already running, the script can use it instead of starting one
of its own:

```bash
python3 contrib/devtools/gen-rpc-docs.py --rpcport=19554 --rpcuser=operator --rpcpassword=<secret>
```

!!! warning
    Never edit a page below `categories/` by hand. The next run of the
    generator overwrites it, and the check mode reports the difference as
    drift. Add prose to this page instead.

## Next steps

* [EVM JSON-RPC](evm-json-rpc.md) — the Ethereum compatible endpoint of the
  MetaChain.
* [Operations → RPC and REST interfaces](../operations/rpc-rest-interfaces.md)
  — how to reach the node from another machine and how to keep it safe.
* [Troubleshooting](../troubleshooting/index.md) — what to do when a call
  fails or does not return.
