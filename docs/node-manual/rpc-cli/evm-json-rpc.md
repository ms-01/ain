# EVM JSON-RPC

The MetaChain runs an Ethereum compatible JSON-RPC endpoint next to the
DeFiChain JSON-RPC interface. It serves the `eth_*`, `net_*`, `web3_*` and
`debug_*` namespaces for wallets, explorers and smart contract tooling.

Part of the [Node Software Manual](../index.md).

## Contents

| Document | What you learn |
| --- | --- |
| [RPC & CLI Reference](index.md) | The commands of the DeFiChain JSON-RPC interface. |
| [Operations → RPC and REST interfaces](../operations/rpc-rest-interfaces.md) | How to reach the interfaces of the node and how to keep them safe. |

## The endpoint

The endpoint is served by the Rust part of the node and listens on its own
port, next to the DeFiChain JSON-RPC port:

| Network | JSON-RPC | ETH JSON-RPC | ETH WebSocket |
| --- | --- | --- | --- |
| mainnet | 8554 | 8551 | 8553 |
| testnet | 18554 | 18551 | 18553 |
| changi | 20554 | 20551 | 20553 |
| devnet | 21554 | 21551 | 21553 |
| regtest | 19554 | 19551 | 19553 |

The ports come from `src/chainparamsbase.cpp`. The changi and devnet defaults
change with the bootstrap switches `-changi-bootstrap` and
`-devnet-bootstrap`, which move both networks into the testnet port range.

### Configuration

| Option | Effect |
| --- | --- |
| `-ethrpcport=<port>` | The port of the ETH JSON-RPC listener. `-ethrpcport=-1` disables the listener completely. |
| `-ethrpcbind=<addr>[:port]` | The address the listener binds to. The port overrides `-ethrpcport`. The option can be given several times. It is ignored unless `-rpcallowip` is set as well. |
| `-ethmaxconnections=<n>` | The maximum number of concurrent connections. The default is `100`. |
| `-ethmaxresponsesize=<mb>` | The maximum size of a response in megabytes. The default is `25`. |
| `-ethtracingmaxmemoryusage=<bytes>` | The memory limit of the raw tracing. The default is `20000000` bytes. |
| `-ethdebug` | Enables the `debug_*` methods that are off by default. |
| `-ethdebugtrace` | Enables the `debug_trace*` methods. The default is `1`. |
| `-ethsubscription` | Enables the subscription notifications. The default is `1`. |
| `-wsport=<port>` | The port of the ETH WebSocket listener. `-wsport=-1` disables it. |
| `-wsbind=<addr>[:port]` | The address the WebSocket listener binds to. It is ignored unless `-rpcallowip` is set. |
| `-rpcallowcors=<host>` | Allows CORS requests from the given origin, for example `http://127.0.0.1:5173`. |

The listener binds to `127.0.0.1` only. To reach it from another machine,
name the allowed source addresses **and** the bind address:

```text
rpcallowip=10.0.0.0/24
ethrpcbind=10.0.0.5
```

Without `-rpcallowip` the node logs a warning and keeps the loopback
binding. With `-rpcallowip` but without `-ethrpcbind` the node logs a
warning and ignores the bind address.

!!! warning
    The ETH JSON-RPC endpoint has **no authentication**. Every method is
    available to everyone who can reach the port, including the methods that
    submit transactions. Keep the listener on the loopback interface, reach
    it through a VPN or an SSH tunnel, and put a reverse proxy with
    authentication in front of it before you expose it to a network you do
    not control.

### Chain IDs

The EVM chain ID is part of the consensus parameters
(`consensus.evmChainId` in `src/chainparams.cpp`) and is reported by
`eth_chainId`:

| Network | Chain ID | Hexadecimal |
| --- | --- | --- |
| mainnet | 1130 | `0x46a` |
| testnet | 1131 | `0x46b` |
| devnet | 1132 | `0x46c` |
| changi | 1133 | `0x46d` |
| regtest | 1133 | `0x46d` |

Use the chain ID of the network you talk to when you sign a transaction or
when you configure a wallet, otherwise the transaction is replayable on
another network.

## Request format

The endpoint speaks **JSON-RPC 2.0**. The `jsonrpc` field is mandatory; a
request without it, or with `"1.0"`, is rejected with
`Invalid request` (`-32600`).

```bash
# The current block number of the EVM.
curl -s -X POST -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}' \
    http://127.0.0.1:8551
```

The answer is the standard JSON-RPC object:

```json
{"jsonrpc":"2.0","id":1,"result":"0x1b4"}
```

An unknown method is reported with the error code `-32601`:

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

Batch requests are supported. The answers are returned in the order of the
requests:

```bash
curl -s -X POST -H 'content-type: application/json' \
    --data '[{"jsonrpc":"2.0","id":1,"method":"eth_chainId","params":[]},
            {"jsonrpc":"2.0","id":2,"method":"net_version","params":[]}]' \
    http://127.0.0.1:8551
```

```json
[{"jsonrpc":"2.0","id":1,"result":"0x46a"},
 {"jsonrpc":"2.0","id":2,"result":"1130"}]
```

!!! note
    The answers of the ETH JSON-RPC endpoint are not covered by the
    consistency guarantees of the DeFiChain JSON-RPC interface. The endpoint
    reads the EVM state of the MetaChain, which is committed in DVM blocks
    with a delay.

## Namespaces

### `eth`

| Method | Description |
| --- | --- |
| `eth_accounts` | The accounts that the node manages. |
| `eth_blockNumber` | The number of the latest EVM block. |
| `eth_call` | Executes a call without creating a transaction and returns the output data. |
| `eth_chainId` | The chain ID of the network. |
| `eth_createAccessList` | Creates the access list of a transaction call. |
| `eth_estimateGas` | Estimates the gas that a call needs. |
| `eth_feeHistory` | The historical gas prices and the reward percentiles. |
| `eth_gasPrice` | The current gas price. |
| `eth_getBalance` | The balance of an address at a given block. |
| `eth_getBlockByHash` | A block, identified by its hash. |
| `eth_getBlockByNumber` | A block, identified by its number. |
| `eth_getBlockTransactionCountByHash` | The number of transactions of a block, identified by its hash. |
| `eth_getBlockTransactionCountByNumber` | The number of transactions of a block, identified by its number. |
| `eth_getCode` | The bytecode of a contract. |
| `eth_getFilterChanges` | The changes of a filter that was created earlier. |
| `eth_getFilterLogs` | The logs of a filter. |
| `eth_getLogs` | The logs that match a filter criteria. |
| `eth_getStorageAt` | The storage value of a contract at a given position. |
| `eth_getTransactionByBlockHashAndIndex` | A transaction, identified by block hash and index. |
| `eth_getTransactionByBlockNumberAndIndex` | A transaction, identified by block number and index. |
| `eth_getTransactionByHash` | A transaction, identified by its hash. |
| `eth_getTransactionCount` | The number of transactions sent from an address. |
| `eth_getTransactionReceipt` | The receipt of a transaction. |
| `eth_getUncleByBlockHashAndIndex` | The uncle of a block, identified by block hash and index. |
| `eth_getUncleByBlockNumberAndIndex` | The uncle of a block, identified by block number and index. |
| `eth_getUncleCountByBlockHash` | The number of uncles of a block, identified by its hash. |
| `eth_getUncleCountByBlockNumber` | The number of uncles of a block, identified by its number. |
| `eth_getWork` | The hash of the current block, the seed hash and the target. |
| `eth_hashrate` | The hash rate of the node. The node always answers `0x0`. |
| `eth_maxPriorityFeePerGas` | The suggested maximum priority fee per gas. |
| `eth_mining` | Whether the node is mining. |
| `eth_newBlockFilter` | Creates a filter for new block headers. |
| `eth_newFilter` | Creates a filter for logs of new blocks. |
| `eth_newPendingTransactionFilter` | Creates a filter for new pending transactions. |
| `eth_pendingTransactions` | The pending transactions of the node. |
| `eth_sendRawTransaction` | Submits a signed transaction and returns its hash. |
| `eth_sendTransaction` | Signs and submits a transaction and returns its hash. |
| `eth_signTransaction` | Signs a transaction and returns the raw transaction. |
| `eth_submitHashrate` | Submits the hash rate of a miner. The node always answers `false`. |
| `eth_submitWork` | Submits a proof of work solution. The node always answers `false`. |
| `eth_syncing` | The synchronisation status of the node. |
| `eth_uninstallFilter` | Removes a filter. |

The mining related methods exist for compatibility with tooling that expects
the Ethereum interface. The MetaChain is a proof of stake chain, so the node
does not mine and always answers the mining methods with their neutral
value.

### `net`

| Method | Description |
| --- | --- |
| `net_version` | The network ID of the node as a decimal string, for example `1130` on mainnet. |
| `net_peerCount` | The number of connected peers. |

### `web3`

| Method | Description |
| --- | --- |
| `web3_clientVersion` | The version of the node and the platform it was built for. |
| `web3_sha3` | The Keccak-256 hash of the given data. |

### `debug`

The `debug_*` methods are meant for development and troubleshooting. The
tracing methods are enabled by default (`-ethdebugtrace=1`), the remaining
methods need `-ethdebug=1`.

| Method | Description |
| --- | --- |
| `debug_traceTransaction` | The trace of a transaction. |
| `debug_traceCall` | The trace of a call that is not submitted. |
| `debug_traceBlockByHash` | The trace of all transactions of a block, identified by its hash. |
| `debug_traceBlockByNumber` | The trace of all transactions of a block, identified by its number. |
| `debug_feeEstimate` | The fee estimate of a call. |

## Subscriptions

The subscription methods are served by the WebSocket listener, not by the
HTTP listener:

| Method | Description |
| --- | --- |
| `eth_subscribe` | Subscribes to an event and returns the subscription ID. |
| `eth_unsubscribe` | Cancels a subscription. |

The supported event types are `newHeads`, `logs`, `newPendingTransactions`
and `syncing`.

```python
import asyncio
import json

import websockets


async def main():
    async with websockets.connect("ws://127.0.0.1:8553") as ws:
        await ws.send(
            json.dumps(
                {"jsonrpc": "2.0", "id": 1, "method": "eth_subscribe", "params": ["newHeads"]}
            )
        )
        print(await ws.recv())
        # {"jsonrpc":"2.0","id":1,"result":"0x…"}
        # Every following message is a notification of the subscription.


asyncio.run(main())
```

!!! warning
    The WebSocket listener has no authentication either. Treat it like the
    HTTP listener: loopback binding, private network or a reverse proxy with
    authentication.

## Related pages

* [RPC & CLI Reference](index.md) — the DeFiChain JSON-RPC commands.
* [Operations → RPC and REST interfaces](../operations/rpc-rest-interfaces.md)
  — authentication, binding and hardening of the interfaces.
* [User Manual → MetaChain and EVM](../../user-manual/evm/index.md) — how the
  two layers relate to each other.
