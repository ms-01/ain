# Support for signing transactions outside of DeFi Blockchain

> **This page has moved.** The canonical version is
> [Node Software Manual → Operations → Backups and external signers](../docs/node-manual/operations/backups-signers.md).
> The file stays in this directory because the build system ships it
> (`Makefile.am` → `DIST_DOCS`); it is removed once the migration is
> complete.

The DeFi Blockchain node can be launched with `-signer=<cmd>` where `<cmd>` is an external tool which can
perform signing device operations, for example a hardware wallet.

The scope of this document is the interaction with an external signing device:

- the enumeration of the connected devices (`enumeratesigners`),
- the verification of a receive address on the device (`walletdisplayaddress`).

The signing of a transaction with a device (the exchange of a PSBT with the device) is not part of this
document, it is tracked separately and not implemented yet.

## Example usage

The following example is based on the [HWI](https://github.com/bitcoin-core/HWI) tool. Version 2.0 or newer is
required. Although this tool is hosted under the Bitcoin Core GitHub organization and maintained by Bitcoin Core
developers, it should be used with caution. It is considered experimental and has far less review than the node
itself. Be particularly careful when running tools such as these on a computer with private keys on it.

Start the node:

```sh
defid -signer=../HWI/hwi.py
```

### Device setup

Follow the hardware manufacturers instructions for the initial device setup, as well as their instructions for
creating a backup. Alternatively, for some devices, you can use the `setup`, `restore` and `backup` commands
provided by [HWI](https://github.com/bitcoin-core/HWI).

### Import the public keys of the device

Get a list of the connected signing devices / services:

```sh
defi-cli enumeratesigners
```

```
{
  "signers": [
    {
      "fingerprint": "c8df832a",
      "name": "trezor_t"
    }
  ]
}
```

The master key fingerprint identifies a device.

The public keys of the device are imported into a wallet of the node with `importdescriptors`. The descriptor has
to carry the origin information of the device, otherwise the node cannot select the device for an address:

```sh
defi-cli importdescriptors '[{"desc":"wpkh([c8df832a/84h/1h/0h/0/0]02c9...)#3te6hhy7","timestamp":"now"}]'
```

### Verify an address

Display an address on the device:

```sh
defi-cli -rpcwallet=<walletname> getnewaddress
defi-cli -rpcwallet=<walletname> walletdisplayaddress <address>
```

Replace `<address>` with the result of `getnewaddress`.

The `walletdisplayaddress` RPC resolves the address to a descriptor of a single key (with the origin information
of the wallet, if the wallet has any) and hands it to the device:

```sh
defi-cli -rpcwallet=<walletname> walletdisplayaddress bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g
```

```
{
  "address": "bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g"
}
```

## The `-signer` option

- `-signer=<cmd>` is a server option of the node, it can be given on the command line or in the configuration
  file (`defi.conf`). It is never read from an RPC request, so there is no command injection surface.
- The command string is split on whitespace. Quoting and escaping are **not** supported, a path that contains a
  space cannot be used.
- The command is executed with `fork`/`exec`, never through a shell.
- The option is registered in the server arguments (`src/init.cpp`) and not in the wallet arguments, so that
  `enumeratesigners` stays usable with `-disablewallet`. This is a deviation from Bitcoin Core, which registers
  the option in the wallet.

## The build switch

The external signer support is enabled by default. A build with `--disable-external-signer` does not contain the
transport to the device (`ENABLE_EXTERNAL_SIGNER` is not defined).

The RPC methods `enumeratesigners` and `walletdisplayaddress` are registered in **every** build, so that the RPC
surface of the node does not depend on the build options. A build without the external signer support answers
with the error `External signer support is not enabled in this build`. This is a documented deviation from Bitcoin
Core, which does not register the methods at all in such a build (the client sees `Method not found`).

## The error precedence

Both RPC methods check their preconditions in the same, deterministic order:

1. **the build**: a build without the external signer support answers with
   `External signer support is not enabled in this build`;
2. **the `-signer` option**: a node that was not started with `-signer=<cmd>` answers with
   `Error: restart defid with -signer=<cmd>`;
3. **the RPC specific checks**: the availability of the wallet, the validation of the address, the resolution of
   the address to a descriptor, the selection of the device and the call of the device.

The build check always comes first, so the message does not depend on the configuration of the node.

## The error catalogue of `enumeratesigners`

| Situation | Error |
|---|---|
| the build has no external signer support | `External signer support is not enabled in this build` |
| the node was not started with `-signer` | `Error: restart defid with -signer=<cmd>` |
| the signer cannot be started | `execve failed: No such file or directory` |
| the signer exits with an error | `RunCommandParseJSON error: ...` |
| the answer is not valid JSON | `Unable to parse JSON: ...` |
| the answer is not an array | `'<cmd>' received invalid response, expected array of signers` |
| the answer contains an error | `'<cmd>' error: <error text>` |
| a signer has no fingerprint | `'<cmd>' received invalid response, missing signer fingerprint` |
| a fingerprint is not 8 hex characters | `'<cmd>' received invalid fingerprint, must be 8 hex characters` |

Signers with a duplicate master key fingerprint are skipped, the first occurrence wins.

## The error catalogue of `walletdisplayaddress`

| Situation | Error |
|---|---|
| the build has no external signer support | `External signer support is not enabled in this build` |
| the node was not started with `-signer` | `Error: restart defid with -signer=<cmd>` |
| the address is invalid | `Invalid address` |
| the address does not belong to the wallet | `Address <addr> is not owned by this wallet` |
| the address cannot be resolved to a descriptor | `Cannot resolve address <addr> to a descriptor (no public key / origin information in this wallet)` |
| no device is connected | `No external signer connected` |
| the device of the address is not connected | `No external signer with fingerprint <fpr> connected (connected: <fp1>, <fp2>)` |
| more than one device is connected and the address has no origin | `Multiple external signers connected (<fp1>, <fp2>); connect only one device` |
| the device reports an error | `Signer returned error: <error text>` |
| the device does not echo the address | `Signer did not echo address` |
| the device echoes another address | `Signer echoed unexpected address <addr>` |

The device is selected by the fingerprint of the origin information of the key of the address. If the wallet has
no genuine origin information for the key, exactly one connected device is required. A wallet that stores no
origin information must not produce a fabricated origin: `CWallet::GetKeyOrigin()` invents a fingerprint for
every key and the expansion of a descriptor without an origin information stores a fabricated origin (the
fingerprint of the key itself with an empty path) in the wallet. The wallet RPC therefore uses the accessor
`CWallet::GetKeyOriginIfPresent()`, which returns `false` for such a fabricated origin, so that a fabricated
origin can never reach the device nor select a device.

## Signer API

In order to be compatible with the node, any signer command should conform to the specification below. This
specification is subject to change.

Prerequisite knowledge:

* [Output Descriptors](descriptors.md)
* Partially Signed Bitcoin Transaction ([PSBT](psbt.md))

### Flag `--chain <name>` (required except for `enumerate`)

With `<name>` one of `main`, `test` or `regtest`. The chain names of the node are mapped to the chain names of the
devices:

| Chain of the node | `--chain` of the device |
|---|---|
| `main` | `main` |
| `test` | `test` |
| `changi` | `test` |
| `devnet` | `test` |
| `regtest` | `regtest` |

The node passes this flag to the commands that operate on a chain specific device, for example
`displayaddress`.

### Flag `--stdin` (required)

Indicates that the (sub)command is received over stdin and the result is returned in the response. `--stdin` is a
global flag, it is not specific to any subcommand.

All subcommands SHOULD support both

- being called as command line arguments; or
- being written to the _external-signer_ process directly through stdin (with `--stdin`).

Usage:

```sh
<cmd> --stdin …
```

```
[…command and arguments written to stdin…]
```

Note: remember that shell-expansion is not available on _stdin_. Consequently, commands such as `signtx`, may
write their arguments in either quoted or unquoted form.

### Flag `--fingerprint <fingerprint>` (required except for `enumerate`)

With `<fingerprint>` being the hexadecimal 8-symbol identifier for a wallet.

The node passes this flag to the commands that operate on a specific external-signer wallet, for example
`displayaddress`.

### `enumerate` (required)

Usage:

```sh
<cmd> enumerate
```

```
[
    {
        "fingerprint": "00000000",
        "model": "trezor_t"
    }
]
```

The command MUST return an array, possibly empty, of entries that contain at least a `fingerprint` field.

If present, the optional `model` field is used as the device name in the `enumeratesigners` RPC result.

### `displayaddress` (required by `walletdisplayaddress`)

Usage:

```sh
<cmd> --fingerprint <fingerprint> --chain <name> displayaddress --desc <descriptor>
```

Example, display a native SegWit receive address on the regtest chain:

```sh
<cmd> --fingerprint 00000000 --chain regtest displayaddress --desc "wpkh([00000000/84h/1h/0h/0/0]02c9...)#3te6hhy7"
```

The command MUST be able to figure out the address type from the descriptor.

The command MUST return an object containing `{"address": "[the address]"}`.

If `<descriptor>` contains a master key fingerprint, the command MUST fail if it does not match the fingerprint
known by the device.

The node validates the answer of the device: the echoed address has to be the address that was requested.

### `signtx` (not used yet)

The signing of a transaction with a device is not implemented yet, it is tracked separately. The
specification of the command is the one of Bitcoin Core:

```sh
<cmd> --stdin --fingerprint <fingerprint> --chain <name>
```

```
signtx <base64-encoded psbt>
```

The command MUST return a JSON object. On success, it MUST contain `{"psbt": "<base64-encoded psbt>"}` updated to
include any new signatures. On failure, it SHOULD contain `{"error": "<message>"}`.

## How the node uses the Signer API

The `enumeratesigners` RPC calls `<cmd> enumerate`, skips duplicate entries with the same `fingerprint`, and maps
the optional `model` field to the RPC `name` field.

The `walletdisplayaddress` RPC obtains the inferred descriptor for the provided address (a descriptor of a single
key, with the origin information of the wallet if the wallet has any). It then calls
`<cmd> --fingerprint <fingerprint> --chain <name> displayaddress --desc <descriptor>` and validates the answer.

The `walletdisplayaddress` RPC is implemented in `src/wallet/rpcwallet.cpp`, the DeFi Blockchain node has no
`src/wallet/rpc/` directory. The `enumeratesigners` RPC is implemented in `src/rpc/external_signer.cpp`.

The transport to the device is implemented in `src/util/subprocess.h` (a dependency free fork/exec helper) and
`src/common/run_command.{h,cpp}` (the execution of the command and the parsing of the JSON answer), the interface
to the device is implemented in `src/external_signer.{h,cpp}`.
