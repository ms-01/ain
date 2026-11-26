# Networking and privacy

The peer-to-peer network, the DNS seeds and the ways to hide the origin of a
node behind Tor or I2P.

Part of the [Node Software Manual](../index.md) ·
[Configuration](index.md).

## Contents

* [The peer-to-peer network](#the-peer-to-peer-network)
* [Choosing your connections](#choosing-your-connections)
* [DNS seeds](#dns-seeds)
* [Tor](#tor)
* [I2P](#i2p)
* [Exposing the RPC interfaces](#exposing-the-rpc-interfaces)

## The peer-to-peer network

DeFiChain nodes talk to each other over TCP. Each network has its own P2P
port:

| Network | P2P port |
| --- | --- |
| mainnet | 8555 |
| testnet | 18555 |
| changi | 20555 |
| devnet | 21555 |
| regtest | 19555 |

`-port=<port>` overrides the port. `-ports=auto` lets the operating system
choose free ports for the P2P, JSON-RPC, WebSocket and ETH JSON-RPC
interfaces at once.

The most important options:

| Option | Default | Meaning |
| --- | --- | --- |
| `-listen` | on (off when `-proxy` or `-connect` is used) | Accept inbound connections. |
| `-bind=<addr>` | all interfaces | Always listen on this address. Use `[host]:port` for IPv6. |
| `-whitebind=<addr>` | – | Listen on this address and whitelist the peers that connect to it. |
| `-connect=<ip>` | – | Connect only to this peer. Implies `-listen=0` and `-dnsseed=0`. |
| `-addnode=<ip>` | – | Connect to this peer and keep the connection open. |
| `-seednode=<ip>` | – | Connect to this peer once to fetch peer addresses. |
| `-externalip=<ip>` | – | The public address of this node; it can be a `.onion` address. |
| `-discover` | on when listening and no `-externalip`/`-proxy` | Detect the own public addresses. |
| `-dnsseed` | on (off with `-connect`) | Ask the DNS seeds for peer addresses. |
| `-maxconnections=<n>` | 125 | Maximum number of peer connections. |
| `-natpmp` | off | Use PCP or NAT-PMP to map the listening port. |
| `-onlynet=<net>` | – | Only connect through the given network: `ipv4`, `ipv6`, `onion` or `i2p`. |
| `-maxuploadtarget=<n>` | 0 | Outbound traffic target in MiB per 24 h. `0` means no limit. |

`-upnp` is deprecated and has no effect since v4.2.7 — UPnP support was
removed. Use a manual port forwarding or `-natpmp` instead.

## Choosing your connections

A node needs peers to synchronise and to relay transactions. There are three
ways to get them:

* **Automatic discovery (default).** The node asks the DNS seeds for
  addresses and connects to the peers it finds. This is the right choice for
  a public full node.
* **Fixed peers.** `-addnode=<ip>` connects to a peer and keeps the
  connection open. The peer will also tell the other nodes about this node,
  so the node becomes discoverable.
* **Isolated node.** `-connect=<ip>` connects *only* to the given peer and
  tells nobody else about this node. It also switches the DNS seeding and the
  listening mode off. This is the right choice for a private node that must
  not be visible on the public network.

```bash
# Private node: talk to exactly one peer.
defid -connect=198.51.100.10:8555

# Public node with two fixed peers in addition to the DNS seeds.
defid -addnode=198.51.100.10 -addnode=198.51.100.11
```

!!! tip
    A node that only listens (`-listen=1`) but does not allow inbound
    connections on the firewall still has eight outbound connections by
    default. The remaining slots of the 125 connections are reserved for
    inbound connections.

## DNS seeds

The seeds are hard-coded in `src/chainparams.cpp`:

| Network | Seeds |
| --- | --- |
| mainnet | `seed.defichain.live`, `seed.mydeficha.in` |
| testnet | `testnet-seed.mydefichain.com`, `seeder-testnet3.defichain-status.com` |
| changi | `35.187.53.161` |
| changi (`-changi-bootstrap`) | `35.187.53.161`, `changi-seed.mydefichain.com` (P2P port `18555`) |
| devnet | `35.187.53.161`, `34.89.47.54` |
| devnet (`-devnet-bootstrap`) | `35.187.53.161`, `34.89.47.54`, `testnet-seed.mydefichain.com` (P2P port `18555`) |
| regtest | none |

A DNS seed answers a query for `A` or `AAAA` records with the addresses of
running nodes. The node only asks the seeds when it does not have enough
peers.

Because a DNS seed can hand out arbitrary addresses, the operators of the
seeds are expected to follow a few rules. The expectations are the same as
for the upstream Bitcoin DNS seeds:

1. The operator follows good host security practices, keeps control over the
   infrastructure and does not sell or transfer control of the seed.
2. The results consist exclusively of well-selected, functioning nodes of the
   public network. The results may be randomised, but no group of hosts may be
   singled out without an urgent technical necessity.
3. The results are never served with a DNS TTL below one minute.
4. Query logs are kept only as long as necessary for the operation or the
   health of the network and are not disclosed to third parties.
5. Data collected by crawling nodes — not by DNS queries — may be published,
   as long as it does not bias the connectivity of the network.
6. A reachable contact address is published for inquiries.

## Tor

The node can use Tor for its outbound connections and can be reachable
through a Tor hidden service.

### Connecting through a Tor proxy

Most Tor installations run a SOCKS5 proxy on port 9050; the Tor Browser
Bundle uses port 9150.

| Option | Meaning |
| --- | --- |
| `-proxy=<ip:port>` | Use this SOCKS5 proxy for all outgoing connections. |
| `-onion=<ip:port>` | Use this SOCKS5 proxy for `.onion` addresses. Defaults to `-proxy`. |
| `-noonion` | Disable access to hidden services. |
| `-proxyrandomize` | Randomise the credentials for every proxy connection (on by default). This enables Tor stream isolation. |
| `-connect`, `-addnode`, `-seednode` | Accept `.onion` addresses when Tor is used. |

```bash
defid -proxy=127.0.0.1:9050
```

Because `-proxy` implies `-listen=0`, the node does not accept inbound
connections and does not detect its own public address.

### Being reachable through Tor

With `-listen=1` the node creates a hidden service automatically through the
Tor control socket:

| Option | Default | Meaning |
| --- | --- | --- |
| `-listenonion` | on | Create the Tor hidden service automatically. |
| `-torcontrol=<ip:port>` | `127.0.0.1:9051` | The Tor control port. |
| `-torpassword=<pass>` | empty | The clear-text password of `HashedControlPassword`. |

The control socket has to be enabled in the Tor configuration
(`ControlPort 9051`) and the user running `defid` must be allowed to
authenticate to it — either through the cookie authentication file or with
`-torpassword`. On Debian-based systems the `debian-tor` group usually has
the required permissions.

Alternatively, a permanent hidden service can be created in `torrc`:

```text
HiddenServiceDir /var/lib/tor/defichain-service/
HiddenServicePort 8555 127.0.0.1:8555
```

The `.onion` address is then written to
`/var/lib/tor/defichain-service/hostname` and can be announced with
`-externalip=<address>.onion`.

```bash
defid -proxy=127.0.0.1:9050 -listen -externalip=<your address>.onion
```

The node caches the private key of the automatically created hidden service
in `onion_v3_private_key` in the data directory, so the address stays the same
across restarts.

!!! tip
    Add only the P2P port to the hidden service. If you also run a web
    service on the same machine, give it its own hidden service — otherwise
    the two services are trivially linkable.

## I2P

The node can connect to I2P peers through a [SAM v3.1](https://i2p.net/en/docs/api/samv3)
bridge. The bridge translates the I2P tunnels into a TCP stream that carries
the ordinary peer-to-peer protocol.

### Running a SAM bridge

The node does not implement the I2P protocol itself; it needs a bridge.
Both `i2pd` and the Java I2P router provide one:

```bash
i2pd --sam.enabled=true --sam.bind=127.0.0.1:7656
```

### Connecting to I2P peers

| Option | Default | Meaning |
| --- | --- | --- |
| `-i2psam=<host:port>` | port 7656 | The SAM bridge to use. |
| `-i2p=<host:port>` | – | Alias of `-i2psam`. |
| `-i2pacceptincoming` | off | Accept inbound I2P connections through the bridge. |

```bash
defid -i2psam=127.0.0.1:7656 -connect=<destination>.b32.i2p
```

The bridge is **not** a SOCKS5 proxy: it is not registered with `-proxy` and
it is not used for `.onion` addresses. The option only selects the bridge for
the I2P transport.

An invalid value is a startup error, an unreachable bridge is not: the
session is created lazily on the first I2P connection attempt and re-created
after a failure, so the node stays alive and keeps retrying.

I2P destinations are portless. A port in the configuration string is accepted
but ignored — the node always uses port 0 for I2P peers.

### Inbound I2P connections

`-i2pacceptincoming` is off by default, because an always-on inbound I2P
listener changes the attack surface of the node. It requires `-i2psam`, and
`-listen=0` implies `-i2pacceptincoming=0`.

Inbound I2P connections go through the regular peer handling: they count
against `-maxconnections` and are handled like inbound TCP connections.

!!! warning
    An inbound I2P peer **cannot be banned**. The ban list is based on the
    legacy address representation, which has no representation for the I2P
    destinations, so an I2P address is rejected by the ban list. Use the
    connection limits to limit the inbound I2P peers.

### The private key and the trust model

The node needs an I2P destination to be reachable. The bridge generates the
destination (the private key) on the first start and stores it in
`<datadir>/i2p_private_key` with the permissions `0600`. The file is never
logged, not even the `SESSION CREATE` request that contains the key. A missing
or corrupt file is regenerated, so the file can be deleted without breaking
the node — the node then simply gets a new destination.

The SAM bridge sees the whole traffic: it can read and modify it and it holds
the private key of the node. It therefore has to be trusted to the same
degree as a SOCKS5 proxy. Do not expose the SAM port to the network.

The I2P addresses of the peers are neither stored in `peers.dat` nor
advertised to the other peers: the I2P peers have to be configured locally.

### Logging

The I2P transport has its own log category:

```bash
defid -debug=i2p
```

It reports the creation and the destruction of the SAM sessions, the own I2P
address and the errors of the connection attempts. The private key and the
destination of the session are never logged.

## Exposing the RPC interfaces

The JSON-RPC, ETH JSON-RPC, gRPC and WebSocket interfaces are **not** exposed
by default. They only start with `-server=1`, and they bind to the loopback
interface unless a bind address is configured.

To make the JSON-RPC interface reachable from another machine:

```bash
defid -server=1 -rpcbind=0.0.0.0 -rpcallowip=198.51.100.0/24
```

`-rpcbind` is ignored without `-rpcallowip`, and `-rpcallowip` without
`-rpcbind` has no effect — the node logs a warning in both cases.

The ETH JSON-RPC interface uses its own bind option, but the same
allow list:

```bash
defid -server=1 -ethrpcbind=0.0.0.0 -rpcallowip=198.51.100.0/24
```

!!! warning
    The RPC password is transmitted unencrypted. Never expose the RPC
    interface to the public internet; restrict it with `-rpcallowip` and put
    it behind a VPN or an SSH tunnel. Use `-rpcauth` instead of
    `-rpcpassword`, so that the hash — and not the password — is what ends up
    in the configuration file or in the process list.
