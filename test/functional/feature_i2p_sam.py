#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the I2P SAM v3 transport.

The test uses a mock SAM v3.1 bridge that runs *inside* the test process (no
background daemon, see the global rule G5): it speaks the subset of the SAM
protocol that the node uses and tunnels the established streams to a TCP
endpoint.

Test plan:
- The node creates an I2P SAM session and establishes an outbound I2P
  connection to a ".b32.i2p" destination through the bridge. The peer is
  reported with its I2P address, the TCP endpoint of the bridge never shows up.
- The I2P peer is neither persisted in the address manager nor advertised to
  the other peers (the guard rails of 0060 / 0119 stay in force).
- A bridge that reports an unreachable peer makes the node log a connect error
  and stay alive.
- A bridge that cannot be reached does not crash the node.
- An invalid "-i2psam" value, a conflicting "-i2p" / "-i2psam" pair and
  "-i2pacceptincoming" without "-i2psam" are init errors.
- The node shuts down cleanly while an I2P session is active.
"""

import base64
import hashlib
import os
import socket
import threading

from test_framework.test_framework import DefiTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, p2p_port, wait_until

# The size of an I2P destination without a certificate.
DESTINATION_LEN = 387


def i2p_b64(data):
    """Encode the given bytes the way the I2P SAM bridge does."""
    return base64.b64encode(data).decode().replace("+", "-").replace("/", "~")


def b32_i2p_address(destination):
    """The ".b32.i2p" address of an I2P destination."""
    digest = hashlib.sha256(destination).digest()
    return base64.b32encode(digest).decode().replace("=", "") + ".b32.i2p"


class MockSamBridge:
    """A mock SAM v3.1 bridge that runs inside the test process."""

    def __init__(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(8)
        self.port = self._sock.getsockname()[1]

        # The destination the bridge resolves the names to.
        self.destination = bytes(range(32))
        # The TCP endpoint the established streams are tunneled to.
        self.tunnel_target = None
        # If set, the bridge reports the peer as unreachable.
        self.reject_connect = False
        # Whether an inbound connection was already accepted.
        self.accepted_once = False

        self.session_count = 0
        self.dest_generate_count = 0
        self.connect_count = 0

        self._stop = False
        self._lock = threading.Lock()
        self._threads = []
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        try:
            self._sock.close()
        except OSError:
            pass
        for thread in self._threads:
            thread.join(timeout=10)

    # -- helpers ---------------------------------------------------------

    @staticmethod
    def _private_key():
        """A structurally valid I2P private key."""
        key = bytearray(range(256)) * 2
        key = key[:DESTINATION_LEN]
        # The certificate length (bytes 385-386) is zero.
        key[385] = 0
        key[386] = 0
        return bytes(key)

    @staticmethod
    def _read_line(sock):
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(1)
            if not chunk:
                return None
            data += chunk
        return data[:-1].decode()

    @staticmethod
    def _send(sock, line):
        sock.sendall((line + "\n").encode())

    def _tunnel(self, sock):
        """Pipe the stream to the tunnel target."""
        if self.tunnel_target is None:
            sock.close()
            return
        try:
            upstream = socket.create_connection(self.tunnel_target, timeout=10)
        except OSError:
            sock.close()
            return
        upstream.settimeout(None)

        def pump(src, dst):
            try:
                while True:
                    data = src.recv(65536)
                    if not data:
                        break
                    dst.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        forward = threading.Thread(target=pump, args=(sock, upstream), daemon=True)
        forward.start()
        pump(upstream, sock)
        forward.join(timeout=10)
        sock.close()
        upstream.close()

    def _handle(self, sock):
        try:
            while True:
                line = self._read_line(sock)
                if line is None:
                    return
                if line.startswith("HELLO"):
                    self._send(sock, "RESULT=OK VERSION=3.1")
                elif line.startswith("DEST GENERATE"):
                    with self._lock:
                        self.dest_generate_count += 1
                    key = i2p_b64(self._private_key())
                    self._send(
                        sock, "RESULT=OK DESTINATION=" + key + " PRIVATEKEY=" + key
                    )
                elif line.startswith("SESSION CREATE"):
                    with self._lock:
                        self.session_count += 1
                    self._send(sock, "RESULT=OK")
                elif line.startswith("NAMING LOOKUP"):
                    self._send(sock, "RESULT=OK VALUE=" + i2p_b64(self.destination))
                elif line.startswith("STREAM CONNECT"):
                    with self._lock:
                        self.connect_count += 1
                        reject = self.reject_connect
                    if reject:
                        self._send(sock, "RESULT=CANT_REACH_PEER")
                    else:
                        self._send(sock, "RESULT=OK")
                        self._tunnel(sock)
                    return
                elif line.startswith("STREAM ACCEPT"):
                    self._send(sock, "RESULT=OK")
                    with self._lock:
                        first = not self.accepted_once
                        self.accepted_once = True
                    if first:
                        # The bridge pushes the destination of the connecting peer.
                        self._send(sock, i2p_b64(self.destination))
                        self._tunnel(sock)
                    else:
                        # Keep the connection open, but do not accept a peer.
                        while True:
                            if not sock.recv(4096):
                                break
                    return
                else:
                    self._send(sock, "RESULT=ERROR UNKNOWN_COMMAND")
        except OSError:
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def _accept_loop(self):
        while not self._stop:
            try:
                sock, _ = self._sock.accept()
            except OSError:
                return
            thread = threading.Thread(target=self._handle, args=(sock,), daemon=True)
            thread.start()
            self._threads.append(thread)


class I2PSamTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.bridge = MockSamBridge()
        self.destination = self.bridge.destination
        self.destination_addr = b32_i2p_address(self.destination).lower()
        self.extra_args = [
            [],
            [
                "-i2psam=127.0.0.1:%d" % self.bridge.port,
                "-connect=" + self.destination_addr,
                "-dnsseed=0",
                "-debug=i2p",
            ],
        ]

    def setup_network(self):
        # The nodes are not connected to each other: node 1 reaches node 0
        # through the I2P SAM bridge.
        self.setup_nodes()
        # The established streams are tunneled to the P2P port of node 0.
        self.bridge.tunnel_target = ("127.0.0.1", p2p_port(self.nodes[0].index))

    def run_test(self):
        node1 = self.nodes[1]

        # The I2P session is created and the connection is established.
        wait_until(
            lambda: len(self.get_i2p_peers(node1)) > 0,
            timeout=60,
        )
        peers = self.get_i2p_peers(node1)
        assert_equal(len(peers), 1)
        peer = peers[0]
        # The peer is reported with its I2P address and not with the TCP
        # endpoint of the SAM bridge.
        assert_equal(peer["network"], "i2p")
        assert peer["addr"].startswith(self.destination_addr), peer
        assert "127.0.0.1" not in peer["addr"], peer
        assert str(self.bridge.port) not in peer["addr"], peer
        # The local address of the connection is our own I2P destination.
        assert peer["addrbind"].endswith(".b32.i2p:0"), peer
        assert "127.0.0.1" not in peer["addrbind"], peer
        assert_equal(peer["inbound"], False)
        # The node 0 sees the connection as an inbound connection.
        wait_until(lambda: len(self.nodes[0].getpeerinfo()) > 0, timeout=60)

        # The I2P peer is not persisted in the address manager ...
        for entry in node1.getnodeaddresses(count=1000):
            assert not entry["address"].endswith(".b32.i2p"), entry
        # ... and it is not advertised to the other peers.
        for entry in self.nodes[0].getnodeaddresses(count=1000):
            assert not entry["address"].endswith(".b32.i2p"), entry

        # The session is logged in the "i2p" category.
        with open(
            os.path.join(node1.datadir, node1.chain, "debug.log"), encoding="utf-8"
        ) as debug_log:
            log = debug_log.read()
        assert "I2P SAM session" in log, log
        assert self.destination_addr in log, log

        # The private key is stored with the permissions 0600.
        key_path = os.path.join(node1.datadir, node1.chain, "i2p_private_key")
        assert os.path.exists(key_path), key_path
        assert_equal(oct(os.stat(key_path).st_mode & 0o777), oct(0o600))

        # A bridge that reports the peer as unreachable makes the node log a
        # connect error, but the node stays alive.
        self.bridge.reject_connect = True
        self.stop_node(1)
        self.start_node(1)
        with open(
            os.path.join(node1.datadir, node1.chain, "debug.log"), encoding="utf-8"
        ) as debug_log:
            debug_log.seek(0, 2)
            offset = debug_log.tell()
        wait_until(lambda: self.has_i2p_error(node1, offset), timeout=60)
        assert_equal(len(node1.getpeerinfo()), 0)

        # A bridge that cannot be reached does not crash the node.
        self.stop_node(1)
        self.start_node(
            1,
            extra_args=[
                "-i2psam=127.0.0.1:1",
                "-connect=" + self.destination_addr,
                "-dnsseed=0",
            ],
        )
        assert_equal(len(node1.getpeerinfo()), 0)

        # The node shuts down cleanly while the I2P session is active.
        self.stop_node(1)

        # The inbound I2P connections are accepted (opt-in).
        self.bridge.reject_connect = False
        self.start_node(
            1,
            extra_args=[
                "-i2psam=127.0.0.1:%d" % self.bridge.port,
                "-i2pacceptincoming=1",
                "-dnsseed=0",
                "-debug=i2p",
            ],
        )
        wait_until(lambda: len(self.get_i2p_peers(node1)) > 0, timeout=60)
        peers = self.get_i2p_peers(node1)
        assert_equal(len(peers), 1)
        # The inbound peer is reported with its I2P address and the port 0.
        assert_equal(peers[0]["addr"], self.destination_addr + ":0")
        assert_equal(peers[0]["inbound"], True)
        assert peers[0]["addrbind"].endswith(".b32.i2p:0"), peers[0]
        self.stop_node(1)

        # The invalid configurations are init errors.
        self.test_init_errors()

    @staticmethod
    def get_i2p_peers(node):
        return [p for p in node.getpeerinfo() if p["network"] == "i2p"]

    def has_i2p_error(self, node, offset):
        with open(
            os.path.join(node.datadir, node.chain, "debug.log"), encoding="utf-8"
        ) as debug_log:
            debug_log.seek(offset)
            log = debug_log.read()
        return "Error connecting to" in log and self.destination_addr in log

    def test_init_errors(self):
        # An invalid SAM bridge is an init error.
        self.nodes[1].assert_start_raises_init_error(
            extra_args=["-i2psam=invalid!!"],
            expected_msg="Invalid -i2psam address or hostname",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        # Conflicting values are an init error.
        self.nodes[1].assert_start_raises_init_error(
            extra_args=["-i2psam=127.0.0.1:7656", "-i2p=127.0.0.1:7657"],
            expected_msg="Conflicting -i2p",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        # The inbound option requires a SAM bridge.
        self.nodes[1].assert_start_raises_init_error(
            extra_args=["-i2pacceptincoming=1"],
            expected_msg="-i2pacceptincoming requires -i2psam",
            match=ErrorMatch.PARTIAL_REGEX,
        )
        # The same value for both options is accepted.
        self.start_node(
            1,
            extra_args=["-i2psam=127.0.0.1:7656", "-i2p=127.0.0.1:7656", "-dnsseed=0"],
        )
        assert_equal(len(self.nodes[1].getpeerinfo()), 0)
        self.stop_node(1)


if __name__ == "__main__":
    I2PSamTest().main()
