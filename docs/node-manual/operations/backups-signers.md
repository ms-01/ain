# Backups and external signers

What to back up, how to restore it, and how to keep the private keys of a
masternode or a treasury wallet off the machine that runs the node.

Part of the [Node Software Manual](../index.md).

## What is worth backing up

| Item | Location | Why |
| --- | --- | --- |
| Wallets | `<datadir>/wallets/` | They hold the private keys. Without them the funds and the masternode collateral are gone. |
| Configuration | `<datadir>/defi.conf` or `/etc/defi/defi.conf` | It contains the RPC credentials, the `-masternode_operator` setting and the ports. |
| The whole data directory | `<datadir>/` | Convenience: it also contains the chain state, the mempool and the EVM state. |

The blocks and the chain state are reproducible — a node can download them
again. For an operator node that only stakes, the wallets and the
configuration are the only things that cannot be recreated. Back up the whole
directory anyway if the disk space allows it; it makes a fast recovery
possible.

!!! warning
    A copy of the wallet directory that is made while the node is running can
    be inconsistent. Stop the node first, or use the wallet-aware backup
    below.

## Backing up a wallet

### The safe way: `backupwallet`

The node copies the wallet database while it is running, in a consistent
state:

```bash
./defi-cli -rpcwallet=mn-owner backupwallet /var/backups/defi/mn-owner.bak
```

The destination can be a directory or a file name. The copy is taken from a
consistent snapshot of the wallet database, so the wallet does **not** have to
be unlocked — unlike for `dumpwallet`, which requires an unlocked wallet.

### The automatic backups

The node can copy every wallet into its wallet directory on a fixed interval:

```text
backupwallet=60
```

The option takes the interval in minutes; `0` (the default) disables it. The
copies are written next to the wallet as
`auto.backup.<walletname>.bak1` and `auto.backup.<walletname>.bak2`, and the
two files are rotated. This is a safety net against a corrupted wallet
database, not a substitute for an off-machine backup.

### The human-readable export: `dumpwallet`

`dumpwallet` writes all keys of a wallet — including the imported keys that
the seed does not cover — into a human-readable file:

```bash
./defi-cli -rpcwallet=mn-owner dumpwallet /var/backups/defi/mn-owner-dump.txt
```

The file must not exist yet, the wallet has to be unlocked, and the file is
**not encrypted**: it contains the private keys in clear text. Treat it like
a private key. Delete it as soon as the keys are back in a wallet, or store
it in an encrypted container.

### Copying the wallet directory

With the node stopped:

```bash
sudo systemctl stop defid.service
sudo cp -a /var/lib/defi/wallets /var/backups/defi/wallets-$(date +%F)
sudo systemctl start defid.service
```

Copy the whole `wallets/` directory, not only the `wallet.dat` file: the
Berkeley DB environment (`wallet.dat/`) belongs to it.

## Restoring a wallet

### From a `backupwallet` copy

```bash
sudo systemctl stop defid.service
sudo cp /var/backups/defi/mn-owner.bak /var/lib/defi/wallets/mn-owner/wallet.dat
sudo chown -R defi:defi /var/lib/defi/wallets
sudo systemctl start defid.service

./defi-cli listwallets
./defi-cli -rpcwallet=mn-owner getbalances
```

If the backup was taken from a different wallet name, rename the directory
accordingly — the wallet name is the directory name below `wallets/`.

### From a dump file

```bash
./defi-cli -rpcwallet=mn-owner importprivkey "<private-key>" "restored" false
```

For a wallet that is restored from its seed, create a new wallet, set the
seed and let the wallet rescan the chain:

```bash
# 1. Create the wallet (name, private keys, blank, passphrase, avoid_reuse).
./defi-cli createwallet "mn-owner" false false "<passphrase>" false

# 2. Restore the seed. The seed is the WIF key that the dump file marks
#    with "hdseed=1".
./defi-cli -rpcwallet=mn-owner sethdseed true "<wif-seed>"

# 3. The wallet rescans the chain and finds its transactions.
./defi-cli -rpcwallet=mn-owner getbalances
```

`sethdseed` requires a node that is no longer in the initial block download,
and it replaces the seed of the wallet — make a new backup afterwards.

### Testing a restore

A backup that was never restored is not a backup. Restore it into a scratch
data directory on a separate machine and check that the balances and the
masternode metadata are there:

```bash
defid -regtest -datadir=/tmp/restore-test -server=1
./defi-cli -regtest -datadir=/tmp/restore-test -rpcwallet=mn-owner getbalances
```

## Keeping the keys safe

* **Encrypt the wallet.** `encryptwallet "<passphrase>"` sets the passphrase
  of a wallet; `walletpassphrase` unlocks it for a single command. The
  passphrase is not stored anywhere — if you lose it, the wallet is lost.
* **Keep the owner key offline.** For a masternode, the owner key only has
  to sign the creation, the update and the resign transaction. Run the owner
  wallet on an air-gapped machine and move the signed transactions over.
  Only the operator key has to be online — see
  [Masternodes](masternodes.md).
* **Do not use the RPC interface of a public node for your keys.** A node
  with an exposed RPC port is a node that can be asked to spend your money.
* **Verify every backup.** Check the size of the file and keep a checksum
  next to it:

  ```bash
  sha256sum /var/backups/defi/mn-owner.bak > /var/backups/defi/mn-owner.bak.sha256
  ```

## External signers

The node can delegate the interaction with a hardware wallet to an external
program:

```bash
defid -signer=/opt/hwi/hwi.py
```

The scope of the integration is deliberately small:

* `enumeratesigners` — the list of the connected devices;
* `walletdisplayaddress` — the display of a receive address on the device.

The exchange of a PSBT with the device — the actual signing of a transaction
— is not implemented.

### Enumerating the devices

```bash
./defi-cli enumeratesigners
```

```json
{
  "signers": [
    {
      "fingerprint": "c8df832a",
      "name": "trezor_t"
    }
  ]
}
```

The fingerprint identifies the master key of the device.

### Importing the public keys

The public keys of the device are imported into a wallet with
`importdescriptors`. The descriptor has to carry the origin information of
the device, otherwise the node cannot select the device for an address:

```bash
./defi-cli importdescriptors '[{"desc":"wpkh([c8df832a/84h/1h/0h/0/0]02c9…)#3te6hhy7","timestamp":"now"}]'
```

### Verifying an address on the device

```bash
./defi-cli -rpcwallet=mn-owner getnewaddress
./defi-cli -rpcwallet=mn-owner walletdisplayaddress <address>
```

The node resolves the address to the descriptor of a single key and hands it
to the device, which displays it. Compare the address on the screen with the
one on your terminal.

### The `-signer` option

* It is a server option, so it can be set on the command line or in
  `defi.conf`. It is never read from an RPC request, so there is no command
  injection surface.
* The command string is split on whitespace. Quoting and escaping are **not**
  supported, so a path with a space cannot be used.
* The command is executed with `fork`/`exec`, never through a shell.

### The error messages

Both RPC methods check their preconditions in the same order: first the
build, then the `-signer` option, then the RPC specific checks. A build
without the external signer support answers with `External signer support is
not enabled in this build`, a node that was not started with `-signer`
answers with `Error: restart defid with -signer=<cmd>`.

!!! warning
    An external signer runs with the privileges of the node. Treat the signer
    program as part of the trusted base of the machine and keep the device
    firmware up to date.

## Next steps

* [Masternodes](masternodes.md) — which keys belong to which role.
* [Data directory](../installation/data-directory.md) — what else is in the
  data directory.
* [Operations](index.md) — the service management and the health checks.
