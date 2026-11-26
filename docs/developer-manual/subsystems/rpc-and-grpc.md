# RPC and gRPC layer

The node exposes several interfaces: the Bitcoin-style JSON-RPC server of
the C++ layer, the Ethereum JSON-RPC and the WebSocket subscriptions of the
Rust layer, the Ocean REST API, the REST interface and the ZMQ
notifications. This page describes how the commands are written, registered
and served.

Part of the [Subsystems section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The C++ server is in `src/rpc/`, the DeFiChain commands in
    `src/dfi/rpc_*.cpp`, the HTTP transport in `src/httprpc.cpp` and
    `src/httpserver.cpp`, and the Rust services in `lib/ain-grpc/` and
    `lib/ain-ocean/`.

## The C++ JSON-RPC server

### The command table

Every RPC command is described by a `CRPCCommand` and is registered in a
category table. `CRPCTable` (`src/rpc/server.h`) holds the commands in a map
and `CRPCTable::appendCommand()` adds a command to it.

```cpp
// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "mempool",            "submitpackage",           &submitpackage,           {"package"} },
};
// clang-format on
```

The category is the first column; it is used by the `help` command
(`CRPCTable::help()`, `src/rpc/server.cpp`) to group the commands and by the
generated reference in the
[RPC and CLI reference](../../node-manual/rpc-cli/index.md).

### `RPCHelpMan`

Each command has a help text that is generated from an `RPCHelpMan` object
(`src/rpc/util.h`):

```cpp
RPCHelpMan{
    "getrawmempool",
    "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n",
    {
        {"verbose", RPCArg::Type::BOOL, /* default */ "false", "True for a json object, false for array of transaction ids"},
    },
    RPCResult{"for verbose = false",
        "[                     (json array of string)\n"
        "  \"transactionid\"     (string) The transaction id\n"
        "]\n"
    },
    RPCExamples{
        HelpExampleCli("getrawmempool", "true")
            + HelpExampleRpc("getrawmempool", "true")
    },
}.Check(request);
```

* `RPCHelpMan::Check()` validates the number of arguments and throws the
  help text when the caller asked for help (`fHelp`) or passed a wrong
  number of arguments.
* `RPCArg` describes an argument (type, default value, description),
  `RPCResults` the result and `RPCExamples` the example calls.

Because the help text and the argument names are declared twice (once in the
`RPCHelpMan` and once in the command table), a lint script keeps them in
sync: `test/lint/check-rpc-mappings.py` compares the argument names of the
command tables with the conversions of `src/rpc/client.cpp`.

### The registration

`src/rpc/register.h` declares one registration function per module and
`RegisterAllCoreRPCCommands()` calls all of them:

| Module | Registration function | File |
| --- | --- | --- |
| Blockchain | `RegisterBlockchainRPCCommands()` | `src/rpc/blockchain.cpp` |
| Mempool | `RegisterMempoolRPCCommands()` | `src/rpc/mempool.cpp` |
| Network | `RegisterNetRPCCommands()` | `src/rpc/net.cpp` |
| Statistics | `RegisterStatsRPCCommands()` | `src/rpc/stats.cpp` |
| Miscellaneous | `RegisterMiscRPCCommands()` | `src/rpc/misc.cpp` |
| External signer | `RegisterSignerRPCCommands()` | `src/rpc/external_signer.cpp` |
| Mining | `RegisterMiningRPCCommands()` | `src/rpc/mining.cpp` |
| Raw transactions | `RegisterRawTransactionRPCCommands()` | `src/rpc/rawtransaction.cpp` |
| Masternodes | `RegisterMasternodesRPCCommands()` | `src/dfi/rpc_masternodes.cpp` |
| Accounts | `RegisterAccountsRPCCommands()` | `src/dfi/rpc_accounts.cpp` |
| Tokens | `RegisterTokensRPCCommands()` | `src/dfi/rpc_tokens.cpp` |
| Pool pairs | `RegisterPoolpairRPCCommands()` | `src/dfi/rpc_poolpair.cpp` |
| Proposals | `RegisterProposalRPCCommands()` | `src/dfi/rpc_proposals.cpp` |
| Masternode blockchain | `RegisterMNBlockchainRPCCommands()` | `src/dfi/mn_rpc.cpp` |
| SPV (anchoring) | `RegisterSpvRPCCommands()` | `src/spv/spv_rpc.cpp` |
| Oracles | `RegisterOraclesRPCCommands()` | `src/dfi/rpc_oracles.cpp` |
| ICX order book | `RegisterICXOrderbookRPCCommands()` | `src/dfi/rpc_icxorderbook.cpp` |
| Loans | `RegisterLoanRPCCommands()` | `src/dfi/rpc_loan.cpp` |
| Vaults | `RegisterVaultRPCCommands()` | `src/dfi/rpc_vault.cpp` |
| EVM | `RegisterEVMRPCCommands()` | `src/dfi/rpc_evm.cpp` |

The client side conversions (the mapping of the JSON arguments to the C++
types of the `defi-cli` client) are in `src/rpc/client.cpp`.

### The transport

`src/httprpc.cpp` registers the HTTP handlers:

* `HTTPReq_JSONRPC` handles the POST requests on `/` (and on `/wallet/` for
  the wallet commands),
* `src/httpserver.cpp` implements the HTTP server, the authentication
  (cookie or user/password) and the request queue.

The relevant options are `-rpcport`, `-rpcbind`, `-rpcallowip`, `-rpcuser`,
`-rpcpassword`, `-rpccookiefile` and `-rpcthreads`.

Two more interfaces of the C++ layer:

* the REST interface (`src/rest.cpp`) serves the read-only REST endpoints,
* the ZMQ notifications (`src/zmq/`) publish the block, transaction and
  account events.

## The Rust services

The Rust layer of the node exposes the Ethereum-compatible interfaces. The
services are started by `src/init.cpp` after the RPC warm-up, through the
`cxx` bridge (`src/ffi/ffiexports.cpp` and `lib/ain-rs-exports/src/core.rs`).

| Service | Rust entry point | C++ caller | Purpose |
| --- | --- | --- | --- |
| Ethereum JSON-RPC | `ain_grpc::init_network_json_rpc_service()` | `ain_rs_init_network_json_rpc_service()` | The `eth`, `net`, `web3` and `debug` namespaces (`lib/ain-grpc/src/rpc/`). |
| WebSocket subscriptions | `ain_grpc::init_network_subscriptions_service()` | `ain_rs_init_network_subscriptions_service()` | The `eth_subscribe` notifications (`lib/ain-grpc/src/subscription/`). |
| Ocean REST | `ain_grpc::init_network_rest_ocean()` | `ain_rs_init_network_rest_ocean()` | The indexer API of `lib/ain-ocean/`. |
| gRPC | `ain_grpc::init_network_grpc_service()` | `ain_rs_init_network_grpc_service()` | The protobuf service of `lib/proto/types/`. |

!!! note "The gRPC service is currently disabled"

    `init_network_grpc_service()` in `lib/ain-grpc/src/lib.rs` is a stub: the
    server start is commented out because there is nothing to serve yet. The
    protobuf definitions (`block.proto`, `eth.proto`, `tx.proto`) are still
    part of the repository. The Ethereum compatibility is provided by the
    JSON-RPC service, not by the gRPC service.

The addresses are computed by `SetupRPCPorts()` (`src/init.cpp`):

| Option | Default | Service |
| --- | --- | --- |
| `-rpcport`, `-rpcbind` | `BaseParams().RPCPort()` | The C++ JSON-RPC server. |
| `-ethrpcport`, `-ethrpcbind` | `BaseParams().ETHRPCPort()` | The Ethereum JSON-RPC server. `-ethrpcport=-1` disables it. |
| `-wsport`, `-wsbind` | `BaseParams().WSPort()` | The WebSocket subscriptions. `-wsport=-1` disables them. |
| `-oceanarchiveserver` | disabled by default | The Ocean REST API. |

## The EVM commands of the C++ layer

The `evm` category of the C++ RPC server (`src/dfi/rpc_evm.cpp`) contains the
commands that inspect the state of the Rust layer through the bridge:

| Command | Purpose |
| --- | --- |
| `evmtx` | Build an EVM transaction. |
| `vmmap`, `logvmmaps` | Inspect the EVM state and the state changes. |
| `dumpevmdb` | Dump the entries of the EVM database. |
| `logevmaccountstates` | Log the account states of the EVM. |

The counterpart is the Ethereum JSON-RPC of the Rust layer, which is
documented in the
[EVM JSON-RPC](../../node-manual/rpc-cli/evm-json-rpc.md) page.

## Writing a new RPC command

1. Add the implementation to the module of the domain (for a DVM command:
   `src/dfi/rpc_<domain>.cpp`).
2. Describe the command with an `RPCHelpMan` object and add the entry with
   its category and its argument names to the `commands` table of the module.
3. If the command is a new module, declare the registration function in
   `src/rpc/register.h` and call it from `RegisterAllCoreRPCCommands()`.
4. Add the client side conversion to `src/rpc/client.cpp` if the command
   should be available in `defi-cli`.
5. Run the lint: `test/lint/check-rpc-mappings.py` verifies the consistency of
   the argument names, `test/lint/lint-rpc-help.sh` the help texts.

## Related pages

* [Mempool and transaction acceptance](mempool.md) — the `sendrawtransaction`
  path.
* [State storage](state-storage.md) — the state that the commands read.
* [C++ ↔ Rust bridge](../architecture/cxx-bridge.md) — how the C++ commands
  reach the Rust services.
* [RPC and CLI reference](../../node-manual/rpc-cli/index.md) — the
  generated command reference.
* [RPC and REST interfaces](../../node-manual/operations/rpc-rest-interfaces.md) —
  the operator-facing configuration of the interfaces.
