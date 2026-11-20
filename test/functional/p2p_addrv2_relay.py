#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the relay of the privacy network addresses (BIP155, ADDRv2).

A Tor v3 onion address or an I2P destination that is received in an "addrv2"
message is stored in the address manager, it is persisted in `peers.dat` and
it survives a restart. It is only relayed to the peers that announced the
support for the BIP155 address relay: the legacy "addr" message never
contains such an address.

The local Tor v3 service of the node is announced to the peers that support
the BIP155 address relay as well.

The file `peers.dat` is written in the legacy format as long as the address
manager contains no address that needs the BIP155 format, so that the file
stays readable by the nodes that do not support it.
"""

import base64
import hashlib
import os
import socket
import threading
import time

from test_framework.messages import (
    CAddress,
    CAddressV2,
    MIN_VERSION_SUPPORTED,
    NODE_NETWORK,
    msg_addrv2,
    msg_addr,
    msg_getaddr,
    msg_sendaddrv2,
    msg_verack,
)
from test_framework.mininode import mininode_lock, P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, p2p_port, wait_until

# The Tor v3 onion address of the ed25519 public key 0x01..0x20 and its raw
# bytes.
ONION_BYTES = bytes(range(1, 33))
ONION_ADDRESS = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion"
# The I2P destination of the bytes 0x00..0x1f and its raw bytes.
I2P_BYTES = bytes(range(32))
I2P_ADDRESS = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p"
# The Tor v3 hidden service of the node: the ed25519 public key 0x21..0x40.
LOCAL_PUBKEY = bytes(range(0x21, 0x41))
LOCAL_ADDRESS = "eercgjbfeytsqkjkfmwc2lrpgaytemzugu3doobzhi5typj6h5ah5dqd.onion"
SERVICE_ID = LOCAL_ADDRESS[: -len(".onion")]
PRIVATE_KEY = "ED25519-V3:2fDs5G7dgS9q5g8ZgMS7VAd4wEqS3jJvB8rFqZ2zXKqQFdQHmAm"
# The default port of the regtest network. The Tor v3 service of the node is
# registered with that port.
REGTEST_PORT = 19555

# Number of addresses seeded into the address manager.
NUM_ADDRESSES = 50
# Timeout for the address messages of the node. The node sends them with a
# poisson distributed delay, so the timeout has to be generous.
ADDR_TIMEOUT = 300
# The format bytes of the address manager file.
FILE_FORMAT_LEGACY = 1
FILE_FORMAT_ADDRV2 = 2


def onion_address(pubkey):
    """The Tor v3 onion address of an ed25519 public key."""
    checksum = hashlib.sha3_256(b".onion checksum" + pubkey + b"\x03").digest()[:2]
    return base64.b32encode(pubkey + checksum + b"\x03").decode().lower()


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
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            conn.settimeout(1.0)
            self.conn = conn
            self._handle_connection(conn)

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
                except OSError:
                    break
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


class AddrV2Peer(P2PInterface):
    """Collects the address messages of the node.

    The support for the BIP155 address relay is announced with a "sendaddrv2"
    message. A peer without it only receives the legacy "addr" message.
    """

    def __init__(self, addrv2=True, request=False):
        super().__init__()
        self.addrs = []
        self.addrs_v2 = []
        self.addrv2 = addrv2
        self.request = request

    def on_version(self, message):
        assert message.nVersion >= MIN_VERSION_SUPPORTED
        if self.addrv2:
            self.send_message(msg_sendaddrv2())
        self.send_message(msg_verack())
        if self.request:
            self.send_message(msg_getaddr())
        self.nServices = message.nServices

    def on_addr(self, message):
        self.addrs.append([(addr.ip, addr.port) for addr in message.addrs])

    def on_addrv2(self, message):
        self.addrs_v2.append(
            [(addr.net, addr.addr, addr.port) for addr in message.addrs]
        )

    def privacy_entries(self):
        """The entries of the received messages that are not IPv4 addresses."""
        entries = []
        for message_addrs in self.addrs_v2:
            for net, addr, port in message_addrs:
                if net != CAddressV2.NET_IPV4:
                    entries.append((net, addr, port))
        return entries

    def legacy_entries(self):
        """The entries of the received legacy "addr" messages."""
        return [entry for message_addrs in self.addrs for entry in message_addrs]


class AddrV2RelayTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The whitelist grants PF_NOBAN, which bypasses the "addr" rate limit.
        self.extra_args = [["-whitelist=127.0.0.1"]]

    def setup_network(self):
        self.mock_tor = MockTorControlServer(p2p_port(self.num_nodes + 20))
        self.mock_tor.start()
        super().setup_network()

    def setup_nodes(self):
        self.extra_args = [
            [
                "-whitelist=127.0.0.1",
                "-listenonion=1",
                "-onion=127.0.0.1:9050",
                f"-torcontrol=127.0.0.1:{self.mock_tor.port}",
                "-debug=tor",
            ]
        ]
        super().setup_nodes()

    def peers_dat(self):
        return os.path.join(self.nodes[0].datadir, self.nodes[0].chain, "peers.dat")

    def log_contains(self, needle):
        log_file = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, "debug.log")
        with open(log_file, encoding="utf-8") as f:
            return needle in f.read()

    def seed_addrman(self):
        """Fill the address manager of the node through the regular p2p path."""
        seeder = self.nodes[0].add_p2p_connection(AddrV2Peer(addrv2=False))
        msg = msg_addr()
        now = int(time.time())
        for i in range(NUM_ADDRESSES):
            addr = CAddress()
            addr.time = now
            addr.nServices = NODE_NETWORK
            # Every address must live in its own /16 netgroup, otherwise the
            # address manager maps all of them into the same bucket and drops
            # the colliding ones.
            addr.ip = "11.%d.0.1" % i
            addr.port = 10000 + (i % 1000)
            msg.addrs.append(addr)
        seeder.send_and_ping(msg)
        wait_until(
            lambda: len(self.nodes[0].getnodeaddresses(1000, "ipv4")) > 0, timeout=60
        )

    def send_privacy_address(self, peer, net, raw, port=1234):
        """Send a privacy network address to the node in an "addrv2" message.

        The message is sent twice: the node only relays the addresses of a
        peer that did not ask for addresses itself.
        """
        msg = msg_addrv2()
        msg.addrs.append(
            CAddressV2(
                time=int(time.time()),
                nServices=NODE_NETWORK,
                net=net,
                addr=raw,
                port=port,
            )
        )
        peer.send_and_ping(msg)
        peer.send_and_ping(msg)

    def addrv2_peers(self):
        """The peers that announced the support for the BIP155 address relay."""
        return [info for info in self.nodes[0].getpeerinfo() if info["addrv2"]]

    def run_test(self):
        try:
            self._run_test()
        finally:
            self.mock_tor.stop()

    def _run_test(self):
        node = self.nodes[0]

        # The onion address of the hidden service of the node is derived from
        # its public key.
        assert_equal(onion_address(LOCAL_PUBKEY) + ".onion", LOCAL_ADDRESS)

        # F1: the address manager is written in the legacy format as long as
        # it contains no address that needs the BIP155 format, and the legacy
        # file is read again after a restart.
        self.seed_addrman()
        self.restart_node(0)
        assert len(node.getnodeaddresses(1000, "ipv4")) > 0
        peers_dat = self.peers_dat()
        assert os.path.exists(peers_dat)
        with open(peers_dat, "rb") as f:
            legacy_file = f.read()
        # The file starts with the network magic, the format byte follows.
        assert_equal(legacy_file[4], FILE_FORMAT_LEGACY)

        # F2: the Tor v3 hidden service of the node is created.
        wait_until(
            lambda: self.mock_tor.command_received("ADD_ONION") is not None,
            timeout=30,
        )
        wait_until(
            lambda: self.log_contains("advertising service {}".format(LOCAL_ADDRESS)),
            timeout=30,
        )

        # The peers that are connected before the privacy addresses are
        # learned: they are the candidates for the relay of the addresses.
        relay_peer = node.add_p2p_connection(AddrV2Peer())
        # The node must know that the peer supports the BIP155 address relay
        # before the address is learned, otherwise the peer is not a candidate
        # for the relay.
        wait_until(lambda: len(self.addrv2_peers()) > 0, timeout=60)
        legacy_peer = node.add_p2p_connection(AddrV2Peer(addrv2=False, request=True))

        # F3: a Tor v3 onion address that is received in an "addrv2" message
        # is stored in the address manager.
        sender = node.add_p2p_connection(AddrV2Peer())
        wait_until(lambda: len(self.addrv2_peers()) > 1, timeout=60)
        self.send_privacy_address(sender, CAddressV2.NET_TORV3, ONION_BYTES)
        wait_until(lambda: len(node.getnodeaddresses(1000, "onion")) > 0, timeout=60)
        stored = node.getnodeaddresses(1000, "onion")
        assert_equal(len(stored), 1)
        assert_equal(stored[0]["address"], ONION_ADDRESS)
        assert_equal(stored[0]["network"], "onion")

        # F4: an I2P destination that is received in an "addrv2" message is
        # stored in the address manager as well.
        self.send_privacy_address(sender, CAddressV2.NET_I2P, I2P_BYTES, port=4321)
        wait_until(lambda: len(node.getnodeaddresses(1000, "i2p")) > 0, timeout=60)
        stored_i2p = node.getnodeaddresses(1000, "i2p")
        assert_equal(len(stored_i2p), 1)
        assert_equal(stored_i2p[0]["address"], I2P_ADDRESS)
        assert_equal(stored_i2p[0]["network"], "i2p")

        # F5: the privacy addresses are relayed to the peers that support the
        # BIP155 address relay...
        wait_until(
            lambda: (CAddressV2.NET_TORV3, ONION_BYTES, 1234)
            in relay_peer.privacy_entries(),
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        wait_until(
            lambda: (CAddressV2.NET_I2P, I2P_BYTES, 4321)
            in relay_peer.privacy_entries(),
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        relayed = relay_peer.privacy_entries()
        assert (CAddressV2.NET_TORV3, ONION_BYTES, 1234) in relayed
        assert (CAddressV2.NET_I2P, I2P_BYTES, 4321) in relayed

        # ... but never to a peer that only understands the legacy "addr"
        # message.
        wait_until(
            lambda: len(legacy_peer.legacy_entries()) > 0,
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        for ip, _port in legacy_peer.legacy_entries():
            assert not ip.endswith(".onion")
            assert not ip.endswith(".b32.i2p")
        assert legacy_peer.is_connected
        assert legacy_peer.privacy_entries() == []

        # F6: the local Tor v3 service of the node is announced to the peers
        # that support the BIP155 address relay, but not to the others.
        wait_until(
            lambda: (CAddressV2.NET_TORV3, LOCAL_PUBKEY, REGTEST_PORT)
            in relay_peer.privacy_entries(),
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        for ip, _port in legacy_peer.legacy_entries():
            assert not ip.endswith(".onion")

        # F7: the privacy addresses are persisted in the address manager file
        # and they survive a restart. The file is written in the BIP155
        # capable format now.
        self.stop_node(0)
        assert os.path.exists(peers_dat)
        with open(peers_dat, "rb") as f:
            addrv2_file = f.read()
        # The file starts with the network magic, the format byte follows.
        assert_equal(addrv2_file[4], FILE_FORMAT_ADDRV2)
        assert ONION_BYTES in addrv2_file
        assert I2P_BYTES in addrv2_file

        self.start_node(0)
        stored = node.getnodeaddresses(1000, "onion")
        assert_equal(len(stored), 1)
        assert_equal(stored[0]["address"], ONION_ADDRESS)
        stored_i2p = node.getnodeaddresses(1000, "i2p")
        assert_equal(len(stored_i2p), 1)
        assert_equal(stored_i2p[0]["address"], I2P_ADDRESS)

        # F8: an operator configured privacy address is not stored in the
        # address manager: the addnode path does not use the address manager
        # at all.
        node.addnode(ONION_ADDRESS, "add")
        node.addnode(I2P_ADDRESS, "add")
        assert_equal(len(node.getnodeaddresses(1000, "onion")), 1)
        assert_equal(len(node.getnodeaddresses(1000, "i2p")), 1)

        # F9: a file with an unsupported format is backed up and the address
        # manager is recreated.
        self.stop_node(0)
        with open(peers_dat, "r+b") as f:
            # The file starts with the network magic, the format byte follows.
            f.seek(4)
            f.write(bytes([3]))
        self.start_node(0)
        assert os.path.exists(peers_dat + ".bak")
        assert_equal(len(node.getnodeaddresses(1000, "onion")), 0)
        assert_equal(len(node.getnodeaddresses(1000, "i2p")), 0)
        # The new file is written in the legacy format.
        with open(peers_dat, "rb") as f:
            recreated_file = f.read()
        assert_equal(recreated_file[4], FILE_FORMAT_LEGACY)


if __name__ == "__main__":
    AddrV2RelayTest().main()
