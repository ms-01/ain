#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that Tor onion addresses stay local to the node.

A node with a Tor v3 hidden service must neither advertise its onion address
to other peers nor store it in the address manager or in `addr.dat`. Onion
addresses have no representation in the legacy (pre-BIP155) wire format, so
they can only be reached through an explicit operator action (`addnode`,
`-connect`, `-proxy`).
"""

import os
import socket
import threading
import time

from test_framework.messages import (
    CAddress,
    MIN_VERSION_SUPPORTED,
    msg_addr,
    msg_getaddr,
    msg_verack,
)
from test_framework.mininode import P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import p2p_port, wait_until

# The service id of a Tor v3 hidden service is the 56 character base32 encoded
# ed25519 public key. The onion address is derived from it.
SERVICE_ID = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd"
ONION_ADDRESS = SERVICE_ID + ".onion"
I2P_ADDRESS = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p"
PRIVATE_KEY = "ED25519-V3:2fDs5G7dgS9q5g8ZgMS7VAd4wEqS3jJvB8rFqZ2zXKqQFdQHmAm"

# Number of addresses seeded into the address manager.
NUM_ADDRESSES = 50


class MockTorControlServer:
    """A minimal in-process implementation of the Tor control protocol."""

    def __init__(self, port):
        self.port = port
        self.sock = None
        self.conn = None
        self.running = False
        self.thread = None
        self.received_commands = []

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.settimeout(1.0)
        self.sock.bind(("127.0.0.1", self.port))
        self.sock.listen(1)
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.conn:
            self.conn.close()
        if self.sock:
            self.sock.close()
        if self.thread:
            self.thread.join(timeout=5)

    def _serve(self):
        while self.running:
            try:
                self.conn, _ = self.sock.accept()
                self.conn.settimeout(1.0)
                self._handle_connection(self.conn)
            except socket.timeout:
                continue
            except OSError:
                break

    def _handle_connection(self, conn):
        try:
            buf = b""
            while self.running:
                try:
                    data = conn.recv(1024)
                    if not data:
                        break
                    buf += data
                    while b"\r\n" in buf:
                        line, buf = buf.split(b"\r\n", 1)
                        command = line.decode("utf-8").strip()
                        if command:
                            self.received_commands.append(command)
                            conn.sendall(self._get_response(command).encode("utf-8"))
                except socket.timeout:
                    continue
        finally:
            conn.close()

    def _get_response(self, command):
        if command == "PROTOCOLINFO 1":
            return (
                "250-PROTOCOLINFO 1\r\n"
                "250-AUTH METHODS=NULL\r\n"
                '250-VERSION Tor="0.4.8.0"\r\n'
                "250 OK\r\n"
            )
        elif command == "AUTHENTICATE":
            return "250 OK\r\n"
        elif command.startswith("ADD_ONION"):
            return (
                f"250-ServiceID={SERVICE_ID}\r\n"
                f"250-PrivateKey={PRIVATE_KEY}\r\n"
                "250 OK\r\n"
            )
        else:
            return "510 Unrecognized command\r\n"

    def command_received(self, prefix):
        for command in self.received_commands:
            if command.startswith(prefix):
                return command
        return None


class OnionCollector(P2PInterface):
    """Collects the addresses the node sends to us.

    With `request` set, the verack and the "getaddr" request are sent
    directly after the connection is opened, so that the node processes them
    together with the version message and answers the request without the
    (poisson distributed) delay of the address broadcast.
    """

    def __init__(self, request=False):
        super().__init__()
        self.addresses = []
        self.version_addrs = []
        self.request = request

    def on_open(self):
        super().on_open()
        if self.request:
            self.send_message(msg_verack())
            self.send_message(msg_getaddr())

    def on_version(self, message):
        self.version_addrs.append((message.addrTo.ip, message.addrTo.port))
        self.version_addrs.append((message.addrFrom.ip, message.addrFrom.port))
        if self.request:
            # The verack was already sent in on_open().
            assert message.nVersion >= MIN_VERSION_SUPPORTED
            self.nServices = message.nServices
        else:
            super().on_version(message)

    def on_addr(self, message):
        self.addresses.extend([(addr.ip, addr.port) for addr in message.addrs])


class OnionLocalOnlyTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The whitelist grants PF_NOBAN, which bypasses the "addr" rate limit.
        self.extra_args = [["-whitelist=127.0.0.1"]]

    def setup_network(self):
        self.mock_tor = MockTorControlServer(p2p_port(self.num_nodes + 20))
        self.mock_tor.start()
        try:
            super().setup_network()
        finally:
            self.mock_tor.stop()

    def setup_nodes(self):
        self.extra_args = [
            [
                "-whitelist=127.0.0.1",
                "-listenonion=1",
                f"-torcontrol=127.0.0.1:{self.mock_tor.port}",
                "-debug=tor",
            ]
        ]
        super().setup_nodes()

    def run_test(self):
        node = self.nodes[0]

        # Wait until the hidden service has been created.
        wait_until(
            lambda: self.mock_tor.command_received("ADD_ONION") is not None,
            timeout=30,
        )
        wait_until(
            lambda: self._log_contains(f"advertising service {ONION_ADDRESS}"),
            timeout=30,
        )

        # An onion address that is explicitly configured must not end up in the
        # address manager either: onion addresses have no representation in the
        # legacy (pre-BIP155) wire format and are therefore never stored.
        node.addnode(ONION_ADDRESS, "add")
        node.addnode(I2P_ADDRESS, "add")
        addresses = node.getnodeaddresses(1000)
        assert not any(addr["address"].endswith(".onion") for addr in addresses)
        assert not any(addr["address"].endswith(".b32.i2p") for addr in addresses)

        # Seed the address manager, so that the node has an address sample to
        # send.
        seeder = node.add_p2p_connection(OnionCollector())
        msg = msg_addr()
        now = int(time.time())
        for i in range(NUM_ADDRESSES):
            addr = CAddress()
            addr.time = now
            # Every address must live in its own /16 netgroup, otherwise the
            # address manager maps all of them into the same bucket and drops
            # the colliding ones.
            addr.ip = "11.%d.0.1" % i
            addr.port = 10000 + (i % 1000)
            msg.addrs.append(addr)
        seeder.send_and_ping(msg)
        wait_until(
            lambda: len(node.getnodeaddresses(1000)) > 0,
            timeout=60,
        )

        # The onion address must not be advertised to other peers.
        requester = node.add_p2p_connection(OnionCollector(request=True))
        wait_until(lambda: len(requester.addresses) > 0, timeout=60)
        assert not any(ip.endswith(".onion") for ip, _ in requester.addresses)
        assert not any(ip.endswith(".b32.i2p") for ip, _ in requester.addresses)
        assert not any(ip.endswith(".onion") for ip, _ in requester.version_addrs)
        assert not any(ip.endswith(".b32.i2p") for ip, _ in requester.version_addrs)

        # The onion address must not be persisted in the address manager.
        addr_dat = os.path.join(node.datadir, node.chain, "addr.dat")
        if os.path.exists(addr_dat):
            with open(addr_dat, "rb") as f:
                assert SERVICE_ID.encode() not in f.read()

    def _log_contains(self, needle):
        log_file = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, "debug.log")
        with open(log_file, encoding="utf-8") as f:
            return needle in f.read()


if __name__ == "__main__":
    OnionLocalOnlyTest().main()
