# Binaries

How to install the DeFiChain node from a release archive on Linux, macOS or
Windows.

Part of the [Node Software Manual](../index.md) ·
[Installation](index.md).

## Contents

* [Download](#download)
* [Linux](#linux)
* [macOS](#macos)
* [Windows](#windows)
* [First start](#first-start)
* [Running as a service](#running-as-a-service)
* [Building from source](#building-from-source)

## Download

Download the archive for your platform from the
[GitHub releases page](https://github.com/DeFiCh/ain/releases) together with
its `.SHA256` file, then verify the checksum:

```bash
# 1. Compute the digest of the archive you downloaded.
sha256sum defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz

# 2. Show the digest that the release published.
cat defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz.SHA256
```

The two digests must be identical. Note that `sha256sum --check` does not
work with the published `.SHA256` files, because they contain the absolute
build path of the release machine. See
[Verifying a download](index.md#verifying-a-download) for the details.

The archive contains the four executables `defid`, `defi-cli`, `defi-tx` and
`defi-wallet` in a `bin/` directory.

## Linux

```bash
# Unpack into an installation directory.
sudo mkdir -p /opt/defichain
sudo tar -xzf defichain-4.2.7-x86_64-pc-linux-gnu.tar.gz -C /opt/defichain
ls /opt/defichain
```

The archive is unpacked into a version directory (`defichain-4.2.7`), which
makes upgrades easy: unpack the new version and point the service unit at it.

### Run as a dedicated user

The node should not run as `root`. Create a service account and give it
ownership of the data directory:

```bash
sudo useradd --system --create-home --home-dir /var/lib/defi --shell /usr/sbin/nologin defi
sudo mkdir -p /var/lib/defi
sudo chown -R defi:defi /var/lib/defi
```

### Systemd unit

The repository ships a reference unit in `contrib/init/defid.service`. Install
it and adjust the paths to your installation:

```bash
sudo cp contrib/init/defid.service /etc/systemd/system/defid.service
sudo systemctl daemon-reload
sudo systemctl enable defid
sudo systemctl start defid
sudo systemctl status defid
```

The unit expects:

| Path | Value |
| --- | --- |
| Binary | `/usr/bin/defid` |
| Configuration file | `/etc/defi/defi.conf` |
| Data directory | `/var/lib/defi` |
| PID file | `/run/defi/defid.pid` |

The unit creates the directories and sets their permissions. The
configuration file and the data directory should only be readable by the
`defi` user and group, so that access to `defi-cli` can be controlled by group
membership.

!!! note
    The command-line options in the shipped unit take precedence over
    `/etc/defi/defi.conf`, so `datadir` cannot be overridden from the
    configuration file alone. Some init systems offer their own mechanism
    (for example `DEFID_DATADIR` for OpenRC).

### Other init systems

`contrib/init/` also contains an OpenRC script (`defid.openrc`,
`defid.openrcconf`) and a CentOS/SysV script (`defid.init`). They expect the
same `defi` user and the same paths as the systemd unit.

## macOS

```bash
mkdir -p ~/defichain
tar -xzf defichain-4.2.7-x86_64-apple-darwin.tar.gz -C ~/defichain
~/defichain/defichain-4.2.7/bin/defid -version | head -1
```

The data directory is `~/Library/Application Support/DeFi`, the configuration
file is `~/Library/Application Support/DeFi/defi.conf`.

To run the node in the background, start it as a daemon:

```bash
~/defichain/defichain-4.2.7/bin/defid -daemon
```

For an automatic start at login, write a launchd plist for your own user; the
repository does not ship one, because the paths depend on the installation
directory.

## Windows

```powershell
# Unpack the zip archive, e.g. to C:\defichain\defichain-4.2.7
Expand-Archive -Path defichain-4.2.7-x86_64-w64-mingw32.zip -DestinationPath C:\defichain
C:\defichain\defichain-4.2.7\bin\defid.exe -version
```

The data directory is `%APPDATA%\DeFi Blockchain` and the configuration file
is `%APPDATA%\DeFi Blockchain\defi.conf`.

Verify the download on Windows with:

```powershell
Get-FileHash -Algorithm SHA256 defichain-4.2.7-x86_64-w64-mingw32.zip
Get-Content defichain-4.2.7-x86_64-w64-mingw32.zip.SHA256
```

and compare the two digests.

## First start

Start the node in the foreground the first time, so that you can see the log
output:

```bash
defid -server=1
```

The node creates the data directory, the configuration file location is
reported in the log (`Using data directory ...`), and the initial block
download begins. Stop it again with `Ctrl-C` or with:

```bash
defi-cli stop
```

!!! note
    The JSON-RPC server is not started unless `-server=1` is given. Without
    it the node validates the chain and participates in the peer-to-peer
    network, but `defi-cli`, the REST interface and the health endpoints do
    not answer.

Verify that the node answers:

```bash
defi-cli getblockchaininfo
defi-cli getnetworkinfo
```

!!! warning
    `defi-cli` needs the same `-datadir`, `-conf` and network option as the
    daemon. `defi-cli -testnet getblockchaininfo` talks to a testnet node,
    `defi-cli getblockchaininfo` to a mainnet node.

## Running as a service

The reference configurations are in `contrib/init/`:

| File | Purpose |
| --- | --- |
| `defid.service` | systemd unit |
| `defid.openrc` | OpenRC init script |
| `defid.openrcconf` | OpenRC `conf.d` file |
| `defid.init` | CentOS/SysV init script |
| `README.md` | Overview of the sample configurations |

### RPC authentication

When `defid` runs as a daemon, it needs an RPC credential:

* If `-rpcpassword` is set, that password is used.
* Otherwise the node falls back to a **cookie** file. The cookie is created
  with random content when the daemon starts and deleted when it exits. Read
  access to the cookie file is what grants RPC access, so the file must only be
  readable by the user that runs the node.

The cookie is stored in the data directory; `-rpccookiefile` overrides the
location. The cookie is the reason why `defi-cli` works out of the box on the
same machine, without any configuration.

For remote access use the salted hash form instead of a plain password:

```bash
python3 contrib/rpcauth/rpcauth.py myuser
```

The script prints a line of the form `rpcauth=myuser:<salt>$<hash>` for the
configuration file together with the generated password. Because the value is
hashed, it can be passed on the command line without leaking the password.

## Building from source

Building from source is only necessary if you change the node itself. The
build instructions are in the
[Developer & Architecture Manual](../../developer-manual/building-testing/index.md);
the short version on Linux is:

```bash
./make.sh build
```

The binaries end up in `build/bin/`. A source build has the same command-line
interface as a release build, so everything in the
[Configuration](../configuration/index.md) section applies unchanged.
