#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the Tor control integration with a mock Tor control server.

Test plan:
- Start a mock Tor control server that answers the PROTOCOLINFO, AUTHENTICATE
  and ADD_ONION commands (no Tor daemon is needed).
- Start a node with -listenonion=1 and -torcontrol pointing at the mock.
- Check that the node requests a Tor v3 (ED25519-V3) hidden service, that it
  derives and logs the onion address from the service id returned by Tor and
  that the private key is cached with restrictive permissions.
- Check that the private key is never written to the log.
"""

import os
import socket
import threading

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    p2p_port,
    wait_until,
)

# The service id of a Tor v3 hidden service is the 56 character base32 encoded
# ed25519 public key. The onion address is derived from it.
SERVICE_ID = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd"
ONION_ADDRESS = SERVICE_ID + ".onion"
PRIVATE_KEY = "ED25519-V3:2fDs5G7dgS9q5g8ZgMS7VAd4wEqS3jJvB8rFqZ2zXKqQFdQHmAm"


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


class TorControlTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self):
        self.mock_tor = MockTorControlServer(p2p_port(self.num_nodes + 10))
        self.mock_tor.start()
        try:
            super().setup_network()
        finally:
            self.mock_tor.stop()

    def setup_nodes(self):
        self.extra_args = [
            [
                "-listenonion=1",
                f"-torcontrol=127.0.0.1:{self.mock_tor.port}",
                "-debug=tor",
            ]
        ]
        super().setup_nodes()

    def run_test(self):
        node = self.nodes[0]

        # The node must ask Tor for a Tor v3 (ED25519-V3) hidden service.
        wait_until(
            lambda: self.mock_tor.command_received("ADD_ONION") is not None,
            timeout=30,
        )
        add_onion = self.mock_tor.command_received("ADD_ONION")
        assert add_onion.startswith("ADD_ONION NEW:ED25519-V3 "), add_onion
        assert "Port=" in add_onion, add_onion

        # The node derives the onion address from the service id and logs it.
        wait_until(
            lambda: node.is_node_stopped() is False
            and self._log_contains(f"tor: Got service ID {SERVICE_ID}"),
            timeout=30,
        )
        assert self._log_contains(f"advertising service {ONION_ADDRESS}")

        # The private key is cached in the data directory with restrictive
        # permissions.
        key_file = os.path.join(node.datadir, node.chain, "onion_v3_private_key")
        wait_until(lambda: os.path.exists(key_file), timeout=30)
        with open(key_file, encoding="utf-8") as f:
            assert_equal(f.read(), PRIVATE_KEY)
        mode = os.stat(key_file).st_mode & 0o777
        assert (
            mode == 0o600
        ), f"the private key file must only be readable by its owner, got {oct(mode)}"

        # The private key must never be written to the log.
        assert not self._log_contains(PRIVATE_KEY)

    def _log_contains(self, needle):
        log_file = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, "debug.log")
        with open(log_file, encoding="utf-8") as f:
            return needle in f.read()


if __name__ == "__main__":
    TorControlTest().main()
