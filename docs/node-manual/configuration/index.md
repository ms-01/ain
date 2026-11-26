# Configuration

How the node is configured: the `defi.conf` file, the command-line options
and the networks they select.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| This page | The configuration file format, the precedence of the options and the network selection. |
| [Networking and privacy](networking-privacy.md) | P2P ports, DNS seeds, proxies, Tor hidden services and I2P. |
| [Performance and IBD](performance-ibd.md) | Pruning, memory and traffic tuning, the initial block download and the header-tip policy. |

## The configuration file

`defid` and `defi-cli` read the same configuration file. It is not created
automatically; create it with a text editor:

| Platform | Default path |
| --- | --- |
| Linux | `$HOME/.defi/defi.conf` |
| macOS | `$HOME/Library/Application Support/DeFi/defi.conf` |
| Windows | `%APPDATA%\DeFi Blockchain\defi.conf` |

`-datadir` moves the data directory, `-conf` overrides the name and the
location of the configuration file. A relative `-conf` path is resolved
against the data directory.

```bash
defid -conf=/etc/defi/defi.conf -datadir=/var/lib/defi
```

### Format

The file is a plain text file with one `option=value` entry per line.
Leading and trailing whitespace is removed.

* Options are written **without** a leading `-`.
* A value is mandatory. A negated option is written as `noconnect=1`, not as
  `noconnect`.
* Blank lines are allowed and ignored.
* A `#` starts a comment that extends to the end of the line. Comments can be
  on their own line or after an `option=value` entry.

```ini
# The node serves the JSON-RPC interface only to the local network.
rpcallowip=192.168.0.0/16
rpcbind=192.168.0.10

# Keep the UTXO cache small on a small VPS.
dbcache=256
```

The one exception is `rpcpassword`: a `#` inside an `rpcpassword` value is
rejected, because it is ambiguous whether the rest of the line is a comment.

### Network-specific sections

Options can be grouped in a section per network. The section names are the
chain names `main`, `test`, `changi`, `devnet` and `regtest`:

```ini
[main]
rpcport=8554
maxconnections=125

[test]
rpcport=18554
maxconnections=50

[regtest]
rpcport=19554
```

The same option can also be prefixed with the network name, which is
equivalent to a section:

```ini
regtest.rpcport=19554
```

Network-specific options take precedence over the same option in the default
section. If an option appears several times with the same precedence, the
**first** occurrence in the file wins.

```ini
# regtest.rpcport ends up as 3000
regtest=1
rpcport=2000
regtest.rpcport=3000

[regtest]
rpcport=4000
```

### Including other files

`includeconf=<file>` includes another configuration file. It can be used
inside a section to make the inclusion network-specific. Recursive includes
are rejected.

```ini
includeconf=/etc/defi/rpc.conf
```

## Precedence

1. **Command line** — the highest precedence.
2. **Network-specific configuration file entry** — a section or a prefixed
   option.
3. **Default configuration file entry**.
4. **Built-in default** of the option.

The exceptions are `-?`, `-help`, `-help-debug`, `-version` and `-conf`
themselves, which cannot be set in the configuration file.

On the command line, the **last** occurrence of an option wins
(`-dbcache=256 -dbcache=512` uses 512). In the configuration file, the
**first** occurrence wins.

## Networks

The network is selected with a command-line option. It determines the default
ports, the DNS seeds and the subdirectory of the data directory:

| Network | Option | P2P | JSON-RPC | gRPC | ETH JSON-RPC | ETH WebSocket |
| --- | --- | --- | --- | --- | --- | --- |
| mainnet | *(default)* | 8555 | 8554 | 8550 | 8551 | 8553 |
| testnet | `-testnet` | 18555 | 18554 | 18550 | 18551 | 18553 |
| changi | `-changi` | 20555 | 20554 | 20550 | 20551 | 20553 |
| devnet | `-devnet` | 21555 | 21554 | 21550 | 21551 | 21553 |
| regtest | `-regtest` | 19555 | 19554 | 19550 | 19551 | 19553 |

The values come from `src/chainparamsbase.cpp` and `src/chainparams.cpp`.
`-changi-bootstrap` and `-devnet-bootstrap` select the bootstrap variant of
those networks: they move the P2P, JSON-RPC, gRPC and ETH ports into the
testnet port range (`18555`/`18554`/`18550`/`18551`/`18553`) and change the
post-AMK message start, while the data directory keeps its own subdirectory.
Both variants additionally use a different DNS seed: `-changi-bootstrap` adds
`changi-seed.mydefichain.com`, `-devnet-bootstrap` adds
`testnet-seed.mydefichain.com`.

Only one network can be active. Combining `-regtest` and `-testnet` is a
startup error.

## Commonly used options

| Option | Default | Meaning |
| --- | --- | --- |
| `-datadir=<dir>` | platform default | Directory of all node data. |
| `-conf=<file>` | `defi.conf` | Name of the configuration file. |
| `-daemon` | off | Run in the background. |
| `-printtoconsole` | on when not a daemon | Send the log to the terminal. |
| `-server` | **off** | Accept JSON-RPC commands. The HTTP server, the REST interface and the health endpoints only start with `-server=1`. |
| `-rpcuser=<user>` / `-rpcpassword=<pw>` | – | Credentials for the JSON-RPC interface. |
| `-rpccookiefile=<loc>` | data directory | Location of the authentication cookie. |
| `-rpcallowip=<ip>` | unset | Addresses allowed to use the RPC interface. Without it, the server only listens on the loopback interface. |
| `-rpcbind=<addr>[:port]` | `127.0.0.1`, `::1` | Address the JSON-RPC interface listens on. It is ignored unless `-rpcallowip` is set as well. |
| `-ethrpcbind=<addr>[:port]` | `127.0.0.1` | Address the ETH JSON-RPC interface listens on. |
| `-ethrpcport=<port>` | per network | Port of the ETH JSON-RPC interface; `-1` disables it. |
| `-wsport=<port>` | per network | Port of the ETH WebSocket interface; `-1` disables it. |
| `-rest` | off | Enable the REST interface. |
| `-txindex` | off | Index every transaction. |
| `-dbcache=<n>` | 512 | UTXO cache size in MiB. |
| `-maxmempool=<n>` | 300 | Mempool size in MB. |
| `-maxconnections=<n>` | 125 | Maximum number of peer connections. |
| `-prune=<n>` | 0 | Pruning: `0` off, `1` manual, `>=550` automatic target in MiB. |
| `-blocksonly` | off | Do not accept or relay transactions. |
| `-listen` | on without `-proxy` | Accept inbound P2P connections. |
| `-maxuploadtarget=<n>` | 0 | Outbound traffic target in MiB per 24 h; `0` means no limit. |

The complete list is printed by the node itself:

```bash
defid -help          # all options
defid -help-debug    # additional debugging and test options
defid -version       # version, build target and commit
```

!!! note
    The JSON-RPC server is **not** started by default. A node that should
    answer `defi-cli` or REST requests has to be started with `-server=1`
    (or with `server=1` in the configuration file).

### Parameter interactions

Some options change the meaning of others. The node applies these rules at
startup and logs every change in `debug.log`:

| Option | Effect |
| --- | --- |
| `-bind`, `-whitebind` | Implies `-listen=1`. |
| `-connect` | Implies `-listen=0` and `-dnsseed=0`: the node only talks to the given peers. |
| `-proxy` | Implies `-listen=0`, `-discover=0` and `-natpmp=0`. |
| `-listen=0` | Implies `-discover=0`, `-listenonion=0` and `-natpmp=0`. |
| `-externalip` | Implies `-discover=0`: no other address is detected. |
| `-blocksonly` | Implies `-whitelistrelay=0`. |
| `-rpcallowip` without `-rpcbind` | The bind address is ignored and a warning is logged. |
| `-rpcbind` without `-rpcallowip` | The bind address is ignored and a warning is logged. |

## Next steps

* [Networking and privacy](networking-privacy.md) — the P2P network, proxies,
  Tor and I2P.
* [Performance and IBD](performance-ibd.md) — disk, memory, bandwidth and the
  initial block download.
