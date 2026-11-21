#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the time offset tracker (node::TimeOffsets).

The tracker collects the clock offsets of the outbound peers, reports their median via
`getnetworkinfo.timeoffset` and warns the operator if the median exceeds 10 minutes.
"""

import os
import time

from test_framework.mininode import (
    P2PInterface,
    P2PListener,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# Clock skew of the test peers (in seconds).
SKEW = 60 * 60
# Number of peers: the median is only computed with 5 or more samples.
NUM_PEERS = 5


class SkewedClockPeer(P2PInterface):
    """A peer whose clock is ahead of the local clock."""

    def __init__(self, skew):
        super().__init__()
        self.skew = skew

    def peer_accept(self, *args, **kwargs):
        super().peer_accept(*args, **kwargs)
        # The version message is sent as soon as the connection has been made.
        self.on_connection_send_msg.nTime = int(time.time()) + self.skew


def read_debug_log(node):
    with open(
        os.path.join(node.datadir, node.chain, "debug.log"), encoding="utf-8"
    ) as debug_log:
        return debug_log.read()


class TimeOffsetsTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]

        def connection_factory():
            return SkewedClockPeer(SKEW)

        listeners = [P2PListener(connection_factory) for _ in range(NUM_PEERS)]
        ports = [listener.start() for listener in listeners]

        # The node connects to the listeners, so that the peers are manual (outbound) peers.
        self.restart_node(
            0, extra_args=["-connect=127.0.0.1:{}".format(port) for port in ports]
        )

        peers = [listener.wait_for_connection() for listener in listeners]
        for peer in peers:
            peer.sync_with_ping()

        # Only outbound peers are sampled, the median of their offsets is reported.
        wait_until(
            lambda: abs(node.getnetworkinfo()["timeoffset"] - SKEW) <= 5, timeout=60
        )

        # The operator is warned, because the median is above the 10 minute threshold.
        assert "out of sync with the network" in read_debug_log(node)

        # The peer level offset is reported as well.
        peer_infos = node.getpeerinfo()
        assert_equal(len(peer_infos), NUM_PEERS)
        for peer_info in peer_infos:
            assert_equal(peer_info["inbound"], False)
            # The offset of the individual peer is reported as well. The sample is taken from the
            # version message of the peer, so it can be off by a second.
            assert abs(peer_info["timeoffset"] - SKEW) <= 5


if __name__ == "__main__":
    TimeOffsetsTest().main()
