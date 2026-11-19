#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the cached "getaddr" responses.

The node serves the same address sample to every requestor within the cache
lifetime, so that an attacker cannot scrape the address manager by opening
many connections. After the cache entry expired, a fresh sample is served.
"""

import time

from test_framework.messages import (
    CAddress,
    MIN_VERSION_SUPPORTED,
    msg_addr,
    msg_getaddr,
    msg_verack,
    NODE_NETWORK,
)
from test_framework.mininode import mininode_lock, P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, wait_until

# Number of addresses seeded into the address manager.
NUM_ADDRESSES = 200
# Lower bound of the addresses reported by "getnodeaddresses". The RPC
# returns a random sample (23%) of the address manager, so a lower bound is
# checked here instead of the exact number of stored addresses.
MIN_REPORTED_ADDRESSES = 20
# Maximum lifetime of a cached response (21h base lifetime + 6h jitter).
CACHE_LIFETIME = 27 * 60 * 60
# Timeout for the "addr" response of the node. The node sends the response
# with a poisson distributed delay, so the timeout has to be generous.
ADDR_TIMEOUT = 300


class AddrCollector(P2PInterface):
    """Collects all "addr" messages received from the node."""

    def __init__(self):
        super().__init__()
        self.addr_messages = []

    def on_addr(self, message):
        self.addr_messages.append([(addr.ip, addr.port) for addr in message.addrs])


class AddrRequester(AddrCollector):
    """Requests the address sample as early as possible.

    The verack and the "getaddr" request are sent directly after the version
    message, so that the node processes all three messages in one batch and
    answers the request without the (poisson distributed) delay of the
    address broadcast.
    """

    def on_open(self):
        super().on_open()
        self.send_message(msg_verack())
        self.send_message(msg_getaddr())

    def on_version(self, message):
        # The verack was already sent in on_open().
        assert message.nVersion >= MIN_VERSION_SUPPORTED
        self.nServices = message.nServices


class P2PAddrRelayCacheTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # The whitelist grants PF_NOBAN, which bypasses the "addr" rate limit
        # and allows the test to seed the address manager quickly.
        self.extra_args = [["-whitelist=127.0.0.1"]]

    def seed_addrman(self):
        """Fill the address manager of the node through the regular p2p path."""
        seeder = self.nodes[0].add_p2p_connection(AddrCollector())
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
        # The address manager drops the few addresses which collide in their
        # bucket and "getnodeaddresses" only reports a random sample of the
        # known addresses, so only a lower bound can be asserted here.
        wait_until(
            lambda: len(self.nodes[0].getnodeaddresses(1000)) >= MIN_REPORTED_ADDRESSES,
            timeout=60,
        )
        return len(self.nodes[0].getnodeaddresses(1000))

    def request_addrs(self):
        """Open a new inbound connection and request the address sample."""
        p2p = self.nodes[0].add_p2p_connection(AddrRequester())
        wait_until(
            lambda: len(p2p.addr_messages) > 0,
            timeout=ADDR_TIMEOUT,
            lock=mininode_lock,
        )
        # The response to "getaddr" carries the address sample. Any other
        # (self-)announcement is smaller, so pick the largest message.
        return max(p2p.addr_messages, key=len)

    def run_test(self):
        num_reported = self.seed_addrman()
        assert num_reported >= MIN_REPORTED_ADDRESSES

        # F1: the node answers a "getaddr" request of an inbound peer with a
        # non-empty address sample.
        sample1 = self.request_addrs()
        assert len(sample1) > 0
        # The cached sample is capped at MAX_ADDR_TO_SEND (1000).
        assert len(sample1) <= 1000

        # F2: a second, independent inbound connection is served the very same
        # sample (cache hit) - the address manager cannot be scraped by
        # reconnecting.
        sample2 = self.request_addrs()
        assert_equal(sorted(sample2), sorted(sample1))

        # F3: after the maximum lifetime of a cache entry the node serves a
        # fresh sample again.
        self.nodes[0].setmocktime(int(time.time()) + CACHE_LIFETIME + 1)
        sample3 = self.request_addrs()
        assert len(sample3) > 0
        assert sorted(sample3) != sorted(sample1)
        self.nodes[0].setmocktime(0)


if __name__ == "__main__":
    P2PAddrRelayCacheTest().main()
