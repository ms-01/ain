Bitcoin Core
=============

> **Legacy directory.** The canonical documentation of this repository is the
> suite below [`docs/`](../docs/README.md), which follows
> [ADR-0002](../docs/developer-manual/adr/ADR-0002-documentation-structure.md).
> The pages listed in [Migrated pages](#migrated-pages) have been rewritten
> there. The files of this directory stay in place because the build system
> ships them (`Makefile.am` → `DIST_DOCS`) and because existing links still
> point at them.

Migrated pages
---------------------

| Legacy page | Canonical page |
| --- | --- |
| [setup-nodes.md](setup-nodes.md) | [Node Software Manual → Installation → Binaries](../docs/node-manual/installation/binaries.md) |
| [setup-nodes-docker.md](setup-nodes-docker.md) | [Node Software Manual → Installation → Docker](../docs/node-manual/installation/docker.md) |
| [files.md](files.md) | [Node Software Manual → Installation → Data directory](../docs/node-manual/installation/data-directory.md) |
| [defi-conf.md](defi-conf.md) | [Node Software Manual → Configuration](../docs/node-manual/configuration/index.md) |
| [init.md](init.md) | [Node Software Manual → Operations](../docs/node-manual/operations/index.md) |
| [tor.md](tor.md) | [Node Software Manual → Configuration → Networking and privacy](../docs/node-manual/configuration/networking-privacy.md) |
| [i2p.md](i2p.md) | [Node Software Manual → Configuration → Networking and privacy](../docs/node-manual/configuration/networking-privacy.md) |
| [dnsseed-policy.md](dnsseed-policy.md) | [Node Software Manual → Configuration → Networking and privacy](../docs/node-manual/configuration/networking-privacy.md) |
| [reduce-memory.md](reduce-memory.md) | [Node Software Manual → Configuration → Performance and IBD](../docs/node-manual/configuration/performance-ibd.md) |
| [reduce-traffic.md](reduce-traffic.md) | [Node Software Manual → Configuration → Performance and IBD](../docs/node-manual/configuration/performance-ibd.md) |
| [setup-masternodes.md](setup-masternodes.md) | [Node Software Manual → Operations → Masternodes](../docs/node-manual/operations/masternodes.md) |
| [JSON-RPC-interface.md](JSON-RPC-interface.md) | [Node Software Manual → Operations → RPC and REST interfaces](../docs/node-manual/operations/rpc-rest-interfaces.md) |
| [REST-interface.md](REST-interface.md) | [Node Software Manual → Operations → RPC and REST interfaces](../docs/node-manual/operations/rpc-rest-interfaces.md) |
| [zmq.md](zmq.md) | [Node Software Manual → Operations → ZMQ notifications](../docs/node-manual/operations/zmq-notifications.md) |
| [external-signer.md](external-signer.md) | [Node Software Manual → Operations → Backups and external signers](../docs/node-manual/operations/backups-signers.md) |

The JSON-RPC and `defi-cli` command reference is generated from the node
itself and lives below
[Node Software Manual → RPC & CLI Reference](../docs/node-manual/rpc-cli/index.md).

Setup
---------------------
Bitcoin Core is the original Bitcoin client and it builds the backbone of the network. It downloads and, by default, stores the entire history of Bitcoin transactions, which requires a few hundred gigabytes of disk space. Depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download Bitcoin Core, visit [bitcoincore.org](https://bitcoincore.org/en/download/).

Running
---------------------
The following are some helpful notes on how to run Bitcoin Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/bitcoin-qt` (GUI) or
- `bin/bitcoind` (headless)

### Windows

Unpack the files into a directory, and then run bitcoin-qt.exe.

### macOS

Drag Bitcoin Core to your applications folder, and then run Bitcoin Core.

### Need Help?

* See the documentation at the [Bitcoin Wiki](https://en.bitcoin.it/wiki/Main_Page)
for help and more information.
* Ask for help on [#bitcoin](http://webchat.freenode.net?channels=bitcoin) on Freenode. If you don't have an IRC client, use [webchat here](http://webchat.freenode.net?channels=bitcoin).
* Ask for help on the [BitcoinTalk](https://bitcointalk.org/) forums, in the [Technical Support board](https://bitcointalk.org/index.php?board=4.0).

Building
---------------------
The following are developer notes on how to build Bitcoin Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)
- [Gitian Building Guide (External Link)](https://github.com/bitcoin-core/docs/blob/master/gitian-building.md)

Development
---------------------
The Bitcoin repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://dev.visucore.com/bitcoin/doxygen/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)

### Resources
* Discuss on the [BitcoinTalk](https://bitcointalk.org/) forums, in the [Development & Technical Discussion board](https://bitcointalk.org/index.php?board=6.0).
* Discuss project-specific development on #bitcoin-core-dev on Freenode. If you don't have an IRC client, use [webchat here](http://webchat.freenode.net/?channels=bitcoin-core-dev).
* Discuss general Bitcoin development on #bitcoin-dev on Freenode. If you don't have an IRC client, use [webchat here](http://webchat.freenode.net/?channels=bitcoin-dev).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [bitcoin.conf Configuration File](bitcoin-conf.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [ZMQ](zmq.md)
- [PSBT support](psbt.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
This product includes software developed by the OpenSSL Project for use in the [OpenSSL Toolkit](https://www.openssl.org/). This product includes
cryptographic software written by Eric Young ([eay@cryptsoft.com](mailto:eay@cryptsoft.com)).
