# RPC and REST interfaces

The node exposes several HTTP interfaces: the JSON-RPC interface, the
MetaChain ETH JSON-RPC interface, the ETH WebSocket interface and the
optional REST interface. This page describes how to reach them and how to
keep them safe.

Part of the [Node Software Manual](../index.md).

## The JSON-RPC interface

The JSON-RPC interface is the main automation interface of the node. It is
served by the HTTP server of `defid` and is used by `defi-cli`, by
`defi-tx`, by the block explorers and by your own scripts.

### Enabling it

The interface is available when the node runs as a server:

```bash
./defid -server=1 -rpcuser=operator -rpcpassword=<secret>
```

In the configuration file (`defi.conf`):

```text
server=1
rpcuser=operator
rpcpassword=<secret>
```

### Binding and ports

By default the HTTP server listens on `127.0.0.1` and `::1` only. The ports
are defined in `src/chainparamsbase.cpp`:

| Network | JSON-RPC | ETH JSON-RPC | ETH WebSocket | gRPC (reserved port) |
| --- | --- | --- | --- | --- |
| mainnet | 8554 | 8551 | 8553 | 8550 |
| testnet | 18554 | 18551 | 18553 | 18550 |
| changi | 20554 | 20551 | 20553 | 20550 |
| devnet | 21554 | 21551 | 21553 | 21550 |
| regtest | 19554 | 19551 | 19553 | 19550 |

The changi and devnet defaults only change with the non-default bootstrap
switches: `-changi-bootstrap` and `-devnet-bootstrap` move both networks into
the testnet port range (`18554`/`18551`/`18553`/`18550`).

To reach the node from another machine you have to name *both* the allowed
source addresses and the bind address:

```text
rpcallowip=10.0.0.0/24
rpcbind=10.0.0.5
```

* `-rpcallowip` accepts a single IP, a network with a netmask or a CIDR
  network. It can be given several times.
* `-rpcbind` accepts `<addr>[:port]`; the port overrides `-rpcport`. For IPv6
  use the `[host]:port` notation.
* If `-rpcallowip` is given without `-rpcbind`, the node logs a warning and
  keeps the loopback binding. If `-rpcbind` is given without `-rpcallowip`,
  the node logs a warning and ignores the bind address.

The same pair of options controls the ETH JSON-RPC and the WebSocket
listener: `-ethrpcbind` with `-ethrpcport`, `-wsbind` with `-wsport`. Both
are ignored unless `-rpcallowip` is set as well.

!!! warning
    The RPC interface has **no transport encryption**. The credentials are
    sent as HTTP basic authentication in clear text. Never expose the RPC
    port to the public internet — use a VPN, an SSH tunnel or a TLS
    terminating reverse proxy.

### Authentication

There are three ways to authenticate, in this order of preference:

1. **The cookie file.** When no `-rpcuser`/`-rpcpassword` is configured, the
   node generates a random credential at every start and writes it to the
   file `.cookie` in the data directory (`<datadir>/.cookie`,
   `<datadir>/testnet3/.cookie`, …). The file is readable by the user that
   runs the node only. `defi-cli` reads it automatically. Use `-rpccookiefile`
   to store it elsewhere.

   ```bash
   cat /var/lib/defi/.cookie
   # __cookie__:e1f2…
   ```

2. **`-rpcauth`.** A salted hash avoids storing the password in clear text.
   The value has the form `<USERNAME>:<SALT>$<HASH>`, where the hash is the
   HMAC-SHA-256 of the password with the salt as the key. It can be given
   several times, for one user per line.

   ```text
   rpcauth=operator:7e0e5f3a$2f3a…
   ```

   The repository ships the helper script that generates such a value:

   ```bash
   python3 contrib/rpcauth/rpcauth.py <username> [password]
   ```

   It prints the complete `rpcauth=` line for the given user. See
   `contrib/rpcauth/README.md` for the details.

3. **`-rpcuser` and `-rpcpassword`.** The fallback for static credentials.
   Choose a long, unique passphrase.

The `Authorization` header is checked with a timing-resistant comparison
(`TimingResistantEqual` in `src/httprpc.cpp`).

### Batch requests

Several calls can be sent in one HTTP request:

```bash
curl --user operator:<secret> --data-binary '[
    {"jsonrpc":"1.0","id":"blockchain","method":"getblockchaininfo","params":[]},
    {"jsonrpc":"1.0","id":"network","method":"getnetworkinfo","params":[]}
]' -H 'content-type: text/plain;' http://127.0.0.1:8554/
```

The answer is an array with one result per request, in the order of the
requests. Batch requests are not atomic: each call is executed separately.

### Other options

| Option | Effect |
| --- | --- |
| `-rpcport=<port>` | The port of the JSON-RPC listener. |
| `-rpcthreads=<n>` | The number of threads that serve the RPC calls. The default is `DEFAULT_HTTP_THREADS`. |
| `-rpcworkqueue=<n>` | The depth of the work queue that serves the RPC calls. |
| `-server=1` | Enables the JSON-RPC interface. |
| `-deprecatedrpc=<method>` | Re-enables an RPC that is deprecated in this release. The methods are listed in the release notes. |

### Consistency guarantees

The state that the RPC interface reports is consistent with the chain state
at the moment of the call:

* The mempool state is consistent with itself and with the chain state at the
  time of the call; it only contains the transactions that the node
  considered valid at that moment.
* The wallet state is consistent with itself and with the chain state at the
  time of the call. It may lag behind the mempool: a transaction that was
  replaced in the mempool can still be reported as unconfirmed by the wallet.

### Other listeners

| Interface | Option | Purpose |
| --- | --- | --- |
| ETH JSON-RPC | `-ethrpcport`, `-ethrpcbind` | The MetaChain EVM endpoints (`eth_*`, `net_*`, `web3_*`). `-ethrpcport=-1` disables the listener. |
| ETH WebSocket | `-wsport`, `-wsbind` | The event stream of the MetaChain. `-wsport=-1` disables the listener. |
| gRPC | — | The chain parameters reserve a gRPC port (8550 on mainnet), but this build does not register a gRPC listener. `defid -help` lists the options that the binary actually supports. |

The complete list of the options is in `defid -help` and in
[Configuration](../configuration/index.md).

## The REST interface

The REST interface is enabled with `-rest=1` and served on the **same port**
as the JSON-RPC interface. It is **not authenticated** — every endpoint can be
read by anyone who can reach the port.

```text
rest=1
```

Because the interface is unauthenticated, enable it only when the RPC port is
bound to the loopback interface or protected by a reverse proxy with
authentication.

### Endpoints

| Endpoint | Returns |
| --- | --- |
| `GET /rest/tx/<txid>.<bin\|hex\|json>` | A transaction. Only the mempool is searched unless the transaction index is enabled with `-txindex=1`. |
| `GET /rest/block/<blockhash>.<bin\|hex\|json>` | A block with the full transaction details. |
| `GET /rest/block/notxdetails/<blockhash>.<bin\|hex\|json>` | A block whose JSON output contains only the transaction hashes. |
| `GET /rest/headers/<count>/<blockhash>.<bin\|hex\|json>` | Up to `<count>` block headers above the given block. |
| `GET /rest/blockhashbyheight/<height>.<bin\|hex\|json>` | The hash of the block at the given height. |
| `GET /rest/chaininfo.json` | The same object as `getblockchaininfo`. |
| `GET /rest/getutxos/<checkmempool>/<txid>-<n>/….<bin\|hex\|json>` | The UTXO set entries of the given outpoints. |
| `GET /rest/mempool/info.json` | The same object as `getmempoolinfo`. |
| `GET /rest/mempool/contents.json` | The transactions in the mempool. |

Examples:

```bash
# The chain info as JSON.
curl -s http://127.0.0.1:8554/rest/chaininfo.json | head -20

# The hash of the block at height 1,000,000.
curl -s http://127.0.0.1:8554/rest/blockhashbyheight/1000000.hex
```

The consistency guarantees of the REST interface are the same as the ones of
the JSON-RPC interface.

### Risks

!!! warning
    A browser on the same machine can read the REST endpoints of the node
    with a request to `http://127.0.0.1:8554/rest/…` — a web page can load
    a script tag that points at the node and read the response. That leaks the
    block and transaction data of your node to a third party.

    Keep `-rest=1` off on machines that run a browser, or put the REST
    endpoints behind an authenticating reverse proxy.

## Hardening checklist

- [ ] The RPC port is bound to `127.0.0.1` (the default) or to a private
      network interface.
- [ ] `-rpcallowip` lists only the addresses that really need access.
- [ ] The credentials are the cookie file or `-rpcauth`, not a shared
      password in a configuration file.
- [ ] `-rest` is disabled unless a consumer needs it.
- [ ] The RPC port is not published through the firewall of the host and not
      exposed through the port mapping of a container.
- [ ] The node runs as an unprivileged user, so that a compromised RPC
      client does not compromise the whole machine.
- [ ] The RPC interface is reached over a VPN or an SSH tunnel:

  ```bash
  ssh -N -L 8554:127.0.0.1:8554 operator@<node-host>
  ```

## Next steps

* [RPC & CLI Reference](../rpc-cli/index.md) — the commands themselves.
* [Configuration](../configuration/index.md) — the complete option list.
* [ZMQ notifications](zmq-notifications.md) — the push interface for block
  and transaction events.
