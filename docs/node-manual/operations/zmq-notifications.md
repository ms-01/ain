# ZMQ notifications

The node can publish block and transaction events to [ZeroMQ](http://zeromq.org/)
subscribers. Use it when a downstream system needs the events as they happen
instead of polling the JSON-RPC interface.

Part of the [Node Software Manual](../index.md).

## What the interface does

The node acts as a *publisher*: it sends the notification to every connected
subscriber. The interface is read-only — a subscriber cannot send anything
back, and the node does not keep any state for it.

ZeroMQ sockets are self-connecting and self-healing: a connection that breaks
is restored automatically, and the publisher and the subscriber can be started
and stopped in any order. Because ZeroMQ is message oriented, the subscriber
receives the messages as complete objects; no reassembly buffer is needed.

## Prerequisites

The feature needs the ZeroMQ API 4.0.0 or newer (`libzmq`). It is compiled in
automatically when the library is found; a build with `--disable-zmq` has no
notification interface at all. The C++ wrapper is not needed.

The example client in `contrib/zmq/zmq_sub.py` additionally needs the Python
binding `python3-zmq`. That is only needed for the example, not for the
daemon.

## Enabling it

Give the node one option per notification, each with the address of the
ZeroMQ socket to publish to:

```text
zmqpubhashblock=tcp://127.0.0.1:28554
zmqpubhashtx=tcp://127.0.0.1:28554
zmqpubrawblock=tcp://127.0.0.1:28554
zmqpubrawtx=tcp://127.0.0.1:28554
```

| Option | Topic | Body |
| --- | --- | --- |
| `-zmqpubhashblock=<address>` | `hashblock` | The 32-byte hash of the new block. |
| `-zmqpubhashtx=<address>` | `hashtx` | The 32-byte hash of the new transaction. |
| `-zmqpubrawblock=<address>` | `rawblock` | The serialized block. |
| `-zmqpubrawtx=<address>` | `rawtx` | The serialized transaction. |

The socket type is `PUB` and the address has to be a valid ZeroMQ endpoint
(`tcp://host:port`, `ipc:///path/to/socket`, …). The same address can be used
by more than one notification — the node then publishes all of them on one
socket.

The high water mark of the `PUB` socket can be set per notification:

| Option | Default |
| --- | --- |
| `-zmqpubhashblockhwm=<n>` | `DEFAULT_ZMQ_SNDHWM` |
| `-zmqpubhashtxhwm=<n>` | `DEFAULT_ZMQ_SNDHWM` |
| `-zmqpubrawblockhwm=<n>` | `DEFAULT_ZMQ_SNDHWM` |
| `-zmqpubrawtxhwm=<n>` | `DEFAULT_ZMQ_SNDHWM` |

The value has to be an integer greater than or equal to zero. A high water
mark of `0` means "no limit".

Example with a file-based socket for the raw transactions:

```bash
defid -daemon \
    -zmqpubhashtx=tcp://127.0.0.1:28554 \
    -zmqpubrawtx=ipc:///tmp/defid.tx.raw \
    -zmqpubhashtxhwm=10000
```

## The message format

Every message consists of three parts:

1. the **topic** — the name of the notification, for example `hashblock`,
   without a terminating null byte;
2. the **body** — the payload described in the table above;
3. the **sequence number** — a 4-byte little-endian counter that is
   incremented for every message the node publishes.

```text
[ topic ][ body ][ sequence number (LE uint32) ]
```

The sequence number lets a subscriber detect lost messages: if the number
jumps, a notification was dropped. ZeroMQ does not guarantee the delivery of
every message — the publisher drops messages when the subscriber cannot keep
up.

## Writing a subscriber

The subscriber has to set the `ZMQ_SUBSCRIBE` option to the topic prefix it
wants to receive. Without it, no message arrives. A prefix of `hash`
subscribes to both `hashblock` and `hashtx`.

The repository ships an example client in `contrib/zmq/zmq_sub.py`:

```bash
# Install the Python binding.
sudo apt-get install python3-zmq

# Start the node with the notifications enabled (see above).

# Subscribe to all four topics.
python3 contrib/zmq/zmq_sub.py
```

The example prints the topic, the body and the sequence number of every
message. A minimal subscriber of your own looks like this:

```python
import struct
import zmq

context = zmq.Context()
socket = context.socket(zmq.SUB)
socket.setsockopt_string(zmq.SUBSCRIBE, "hashblock")
socket.connect("tcp://127.0.0.1:28554")

while True:
    topic, body, sequence = socket.recv_multipart()
    print(topic.decode(), body[::-1].hex(), struct.unpack("<I", sequence)[0])
```

!!! note
    The hashes are published in *internal* byte order. Reverse the 32 bytes
    to get the usual hex representation of the hash.

## Reorganisation and gaps

When the tip of the chain changes, the node publishes a notification for the
new tip. It does not tell the subscriber that a reorganisation happened and it
does not republish the blocks that were replaced. The subscriber has to
reconcile the chain itself, for example by comparing the new tip with the
last tip it has seen and by asking the node for the missing blocks over
JSON-RPC.

The same applies when the initial block download is running: the node
publishes the blocks as they arrive, and a subscriber that starts late has to
fill the gap itself.

## Security

!!! warning
    The ZeroMQ interface is **not authenticated and not encrypted**. Anyone
    who can reach the socket receives the block and transaction data of your
    node. Bind it to the loopback interface and reach it through a tunnel, or
    restrict it with a firewall.

The node does not broadcast anything that it did not receive from the
peer-to-peer network itself, so a subscriber learns nothing that a
peer-to-peer observer could not learn — with the exception of the fact that
the node accepted the transaction.

## Next steps

* [RPC and REST interfaces](rpc-rest-interfaces.md) — the pull interface for
  the same data.
* [Configuration](../configuration/index.md) — where the ZMQ options live in
  `defi.conf`.
