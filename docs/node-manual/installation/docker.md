# Docker

Running the DeFiChain node in a container.

Part of the [Node Software Manual](../index.md) ·
[Installation](index.md).

## Contents

* [Image](#image)
* [Running the node](#running-the-node)
* [Data volume](#data-volume)
* [Ports](#ports)
* [Remote access](#remote-access)
* [Docker Compose](#docker-compose)
* [Logging](#logging)

## Image

The official image is published on Docker Hub as `defi/defichain`:

```bash
docker pull defi/defichain:latest
```

| Tag | Content |
| --- | --- |
| `latest` | The latest stable release. |
| `defi/defichain:<version>` | A pinned release, built from the matching tag of the repository. |

The image is built from `contrib/dockerfiles/x86_64-pc-linux-gnu.dockerfile`
and supports the `linux/amd64` platform. It contains the release binaries in
`/app/bin` (`defid`, `defi-cli`, `defi-tx`, `defi-wallet`), and `/app/bin` is
part of the `PATH`, so `defi-cli` can be called directly.

The container runs unprivileged as the user `defi` (group `defi`).

## Running the node

```bash
# Foreground, with the log on the terminal.
docker run --rm -it defi/defichain

# As a detached service.
docker run -d --name defi-node defi/defichain
```

Any argument after the image name replaces the default command
(`/app/bin/defid`), so all node options are passed through:

```bash
docker run --rm -it defi/defichain \
  defid \
  -printtoconsole \
  -server=1 \
  -regtest=1 \
  -rpcallowip=172.17.0.0/16
```

## Data volume

The container declares `/data` as a volume. The default data directory
`/home/defi/.defi` is a symbolic link to it, so everything the node writes is
kept in the volume:

```bash
# Named volume.
docker run -d --name defi-node -v defi-data:/data defi/defichain

# Bind mount to a directory on the host.
docker run -d --name defi-node -v ${PWD}/data-dir:/data defi/defichain
```

A configuration file placed at `/data/defi.conf` is picked up automatically.
Because the file is read by the `defi` user, it has to be readable by that
user inside the container.

## Ports

The image exposes the JSON-RPC, gRPC and ETH JSON-RPC ports of the supported
networks. The P2P port is not part of `EXPOSE`; publish it explicitly if the
container should accept inbound peer connections.

| Network | P2P | JSON-RPC | gRPC | ETH JSON-RPC |
| --- | --- | --- | --- | --- |
| mainnet | 8555 | 8554 | 8550 | 8551 |
| testnet | 18555 | 18554 | 18550 | 18551 |
| changi | 20555 | 20554 | 20550 | 20551 |
| regtest | 19555 | 19554 | 19550 | 19551 |

```bash
docker run --rm -it \
  -p 19554:19554 \
  -p 19555:19555 \
  defi/defichain \
  defid \
  -printtoconsole \
  -server=1 \
  -regtest=1 \
  -rpcallowip=172.17.0.0/16 \
  -rpcbind=0.0.0.0
```

!!! warning
    `-rpcbind` is ignored unless `-rpcallowip` is given as well, and
    `-rpcallowip=0.0.0.0/0` opens the RPC server to the world. Restrict it to
    the addresses that really need access, and always use `-rpcauth` for
    remote clients.

## Remote access

There are two ways to talk to a running daemon.

### Cookie authentication (local only)

A client that runs as the same user on the same machine can read the cookie
file and therefore needs no credentials:

```bash
docker exec defi-node defi-cli -regtest getmintinginfo
```

Inside the container the cookie is at `/data/regtest/.cookie`; on mainnet the
network part is missing.

### `rpcauth` authentication (remote)

Generate a salted hash and the matching password with the reference script
`contrib/rpcauth/rpcauth.py` from the repository:

```bash
python3 contrib/rpcauth/rpcauth.py myuser
```

It prints a line for the configuration file and the generated password:

```text
String to be appended to defi.conf:
rpcauth=myuser:7d9ba5ae63c3d4dc30583ff4fe65a67e$9e3634e81c11659e3de036d0bf88f89cd169c1039e6e09607562d54765c649cc
Your password:
qDDZdeQ5vw9XXFeVnXT4PZ--tGN2xNjjR4nrtyszZx0=
```

Start the node with the hashed credential:

```bash
docker run --rm --name defi-node -it defi/defichain \
  defid \
  -printtoconsole \
  -server=1 \
  -regtest=1 \
  -rpcallowip=172.17.0.0/16 \
  -rpcauth='myuser:7d9ba5ae63c3d4dc30583ff4fe65a67e$9e3634e81c11659e3de036d0bf88f89cd169c1039e6e09607562d54765c649cc'
```

The value is hashed, so it is safe to pass it on the command line — the
password itself never appears in the process list. Some shells (zsh, for
example) need the single quotes.

From a second container on the same Docker network:

```bash
docker run -it --rm --link defi-node defi/defichain \
  defi-cli \
  -rpcconnect=defi-node \
  -regtest \
  -rpcuser=myuser \
  -stdinrpcpass \
  getbalance
```

`defi-cli` asks for the password on the terminal and sends it to the node.

## Docker Compose

```yaml
services:
  defichain:
    image: defi/defichain:latest
    container_name: defi-node
    restart: unless-stopped
    volumes:
      - defi-data:/data
    command: >
      defid
      -printtoconsole
      -server=1
      -regtest=1

volumes:
  defi-data:
```

## Logging

The container writes the node log to stdout, so `docker logs` shows it:

```bash
docker logs -f defi-node
```

The node additionally writes `debug.log` into the data directory, which is
part of the volume.
