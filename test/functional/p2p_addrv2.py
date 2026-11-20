#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the BIP155 (ADDRv2) address relay.

A peer that announces the support for the BIP155 address relay with a
"sendaddrv2" message receives the address sample in the "addrv2" format, a
legacy peer receives it in the legacy "addr" format and stays connected.

Malformed "addrv2" messages neither disconnect the peer nor increase its
misbehaviour score: an entry with an unknown network id, an entry with an
unexpected address size and an oversized address are simply ignored. The
entries behind a malformed entry are still processed by the node.

Onion addresses that are received in an "addrv2" message are stored in the
address manager and relayed to the peers that support the BIP155 address
relay. A peer that does not support it never receives such an address.
"""

import socket
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
from test_framework.util import assert_equal, wait_until

# Number of addresses seeded into the address manager.
NUM_ADDRESSES = 200
# Lower bound of the addresses reported by "getnodeaddresses". The RPC returns
# a random sample of the address manager, so a lower bound is checked here
# instead of the exact number of stored addresses.
MIN_REPORTED_ADDRESSES = 10
# Timeout for the address response of the node. The node sends the response
# with a poisson distributed delay, so the timeout has to be generous.
ADDR_TIMEOUT = 300
# The Tor v3 onion address of the ed25519 public key 0x01..0x20.
ONION_ADDRESS = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion"
ONION_BYTES = bytes(range(1, 33))


class AddrV2Peer(P2PInterface):
    """Collects the address messages of the node.

    The support for the BIP155 address relay is announced with a "sendaddrv2"
    message, either after the verack (like the node does it) or before it
    (like the upstream Bitcoin Core does it).
    """

    def __init__(self, addrv2=True, before_verack=False, request=False):
        super().__init__()
        self.addrs = []
        self.addrs_v2 = []
        self.commands = []
        self.addrv2 = addrv2
        self.before_verack = before_verack
        self.request = request

    def on_version(self, message):
        assert message.nVersion >= MIN_VERSION_SUPPORTED
        # The version message is sent automatically when the connection is
        # opened, the verack and the capability message are sent here.
        if self.before_verack:
            if self.addrv2:
                self.send_message(msg_sendaddrv2())
            self.send_message(msg_verack())
        else:
            self.send_message(msg_verack())
            if self.addrv2:
                self.send_message(msg_sendaddrv2())
        if self.request:
            self.send_message(msg_getaddr())
        self.nServices = message.nServices

    def on_verack(self, message):
        self.commands.append("verack")

    def on_sendaddrv2(self, message):
        self.commands.append("sendaddrv2")

    def on_addr(self, message):
        self.addrs.append([(addr.ip, addr.port) for addr in message.addrs])

    def on_addrv2(self, message):
        self.addrs_v2.append(
            [(addr.net, addr.addr, addr.port) for addr in message.addrs]
        )


class P2PAddrV2Test(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # The whitelist grants PF_NOBAN, which bypasses the "addr" rate limit.
        # The onion network is only reachable with a Tor proxy: the addresses
        # of an unreachable network are not stored in the address manager.
        self.extra_args = [["-whitelist=127.0.0.1", "-onion=127.0.0.1:9050"]]

    def setup_network(self):
        self.peers = []
        super().setup_network()

    def add_peer(self, *args, **kwargs):
        peer = self.nodes[0].add_p2p_connection(AddrV2Peer(*args, **kwargs))
        self.peers.append(peer)
        return peer

    def seed_addrman(self):
        """Fill the address manager of the node through the regular p2p path."""
        seeder = self.add_peer(addrv2=False)
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
            lambda: len(self.nodes[0].getnodeaddresses(1000)) >= MIN_REPORTED_ADDRESSES,
            timeout=60,
        )

    def total_addr_processed(self):
        """The number of addresses that the node processed for all its peers."""
        return sum(info["addr_processed"] for info in self.nodes[0].getpeerinfo())

    def run_test(self):
        self.seed_addrman()

        # F1: a peer that announces the support for the BIP155 address relay
        # receives the address sample in the "addrv2" format.
        peer = self.add_peer(request=True)
        wait_until(
            lambda: len(peer.addrs_v2) > 0,
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        # The peer did not receive a legacy "addr" message.
        assert_equal(peer.addrs, [])
        entries = peer.addrs_v2[0]
        assert len(entries) > 0
        # The sample contains the addresses that were seeded into the address
        # manager. The IPv4 addresses use the 4 byte form of the BIP155 wire
        # format.
        ips = set()
        for net, addr, port in entries:
            assert_equal(net, CAddressV2.NET_IPV4)
            assert_equal(len(addr), 4)
            ips.add(socket.inet_ntoa(addr))
        assert all(ip.startswith("11.") for ip in ips)

        # F2: the verack of the node precedes its "sendaddrv2" message, so that
        # a peer which does not know the message does not receive it in the
        # window in which it still expects the verack.
        assert_equal(peer.commands, ["verack", "sendaddrv2"])

        # F3: a legacy peer still receives the legacy "addr" message and stays
        # connected.
        legacy = self.add_peer(addrv2=False, request=True)
        wait_until(
            lambda: len(legacy.addrs) > 0,
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        assert_equal(legacy.addrs_v2, [])
        assert legacy.is_connected

        # F4: a peer that announces the support before its verack is accepted
        # as well and receives the "addrv2" message.
        early = self.add_peer(before_verack=True, request=True)
        wait_until(
            lambda: len(early.addrs_v2) > 0,
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        assert_equal(early.addrs, [])

        # F5: the capability of a peer is exposed by "getpeerinfo".
        peer_infos = self.nodes[0].getpeerinfo()
        assert_equal(len([info for info in peer_infos if info["addrv2"]]), 2)

        # F6: a malformed "addrv2" message neither disconnects the peer nor
        # increases its misbehaviour score: the invalid entries are ignored.
        malformed = self.add_peer()
        now = int(time.time())
        # An entry with an unknown network id.
        msg = msg_addrv2()
        msg.addrs.append(
            CAddressV2(
                time=now,
                nServices=NODE_NETWORK,
                net=7,
                addr=b"\x01\x02\x03\x04",
                port=1234,
            )
        )
        # An entry with a known network id and an unexpected address size.
        msg.addrs.append(
            CAddressV2(
                time=now,
                nServices=NODE_NETWORK,
                net=CAddressV2.NET_IPV4,
                addr=b"\x01\x02\x03\x04\x05\x06\x07\x08",
                port=1234,
            )
        )
        # An entry with an address that is longer than the maximum.
        msg.addrs.append(
            CAddressV2(
                time=now,
                nServices=NODE_NETWORK,
                net=CAddressV2.NET_IPV4,
                addr=b"\x01" * 1024,
                port=1234,
            )
        )
        # A valid entry, so that the node has something to process.
        msg.addrs.append(
            CAddressV2(
                time=now,
                nServices=NODE_NETWORK,
                net=CAddressV2.NET_IPV4,
                addr=socket.inet_aton("12.0.0.1"),
                port=1234,
            )
        )
        # The peer is still connected after the malformed message and the node
        # still answers its messages.
        before = self.total_addr_processed()
        malformed.send_and_ping(msg)
        # The malformed entries are ignored, but the valid entry behind them is
        # still processed by the node: it is the only entry of the message that
        # is counted as a processed address.
        assert_equal(self.total_addr_processed(), before + 1)

        # F7: an onion address that is received in an "addrv2" message is
        # stored in the address manager. It is only relayed to the peers that
        # announced the support for the BIP155 address relay.
        onion = self.add_peer()
        msg = msg_addrv2()
        msg.addrs.append(
            CAddressV2(
                time=now,
                nServices=NODE_NETWORK,
                net=CAddressV2.NET_TORV3,
                addr=ONION_BYTES,
                port=1234,
            )
        )
        onion.send_and_ping(msg)
        # The onion address is stored in the address manager.
        wait_until(
            lambda: len(self.nodes[0].getnodeaddresses(1000, "onion")) > 0,
            timeout=60,
        )
        # A peer that does not support the BIP155 address relay never receives
        # the onion address: it does not even receive an "addrv2" message and
        # the legacy "addr" message never contains such an address.
        for other in self.peers:
            if not other.addrv2:
                assert other.addrs_v2 == []
                for entries in other.addrs:
                    for ip, _port in entries:
                        assert not ip.endswith(".onion")


if __name__ == "__main__":
    P2PAddrV2Test().main()
