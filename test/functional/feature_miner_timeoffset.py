#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the interaction between the peer derived time offset and the mining path.

The mining path uses the unadjusted clock, because the validation limits are unadjusted as well.
A node whose peers report a skewed clock must therefore not mine a block with a timestamp that
its own validation would reject.
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
SKEW = 3 * 60 * 60
# The applied offset is bound by -maxtimeadjustment, so the bound has to be raised for this test.
MAX_TIME_ADJUSTMENT = 3 * 60 * 60
# Number of peers: the median is only computed with 5 or more samples.
NUM_PEERS = 5
# The unadjusted limit of the 30 second future time rule.
MAX_FUTURE_BLOCK_TIME = 30


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


class MinerTimeOffsetTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            # node0: the miner, with a raised bound for the applied time offset
            ["-maxtimeadjustment={}".format(MAX_TIME_ADJUSTMENT)],
            # node1: the network peer, it receives the mined block
            [],
        ]

    def run_test(self):
        node = self.nodes[0]
        peer_node = self.nodes[1]

        def connection_factory():
            return SkewedClockPeer(SKEW)

        listeners = [P2PListener(connection_factory) for _ in range(NUM_PEERS)]
        ports = [listener.start() for listener in listeners]

        # The node connects to the listeners, so that the peers are manual (outbound) peers.
        self.restart_node(
            0,
            extra_args=[
                "-maxtimeadjustment={}".format(MAX_TIME_ADJUSTMENT),
            ]
            + ["-connect=127.0.0.1:{}".format(port) for port in ports],
        )

        peers = [listener.wait_for_connection() for listener in listeners]
        for peer in peers:
            peer.sync_with_ping()

        # The peer derived offset is applied to the adjusted clock of the node ...
        wait_until(
            lambda: abs(node.getnetworkinfo()["timeoffset"] - SKEW) <= 5, timeout=60
        )

        # ... but the mining path uses the unadjusted clock, so the node does not mine a block
        # that its own validation would reject.
        address = node.get_genesis_keys().ownerAuthAddress
        assert_equal(
            node.generatetoaddress(nblocks=1, address=address, maxtries=100), 1
        )
        assert "time-too-new" not in read_debug_log(node)

        block_hash = node.getbestblockhash()
        block = node.getblock(block_hash)
        # The timestamp of the mined block is the unadjusted clock, not the adjusted one.
        assert block["time"] <= time.time() + MAX_FUTURE_BLOCK_TIME
        assert block["time"] >= time.time() - 60

        # The block is accepted by the network as well.
        raw_block = node.getblock(block_hash, 0)
        assert_equal(peer_node.submitblock(raw_block), None)
        assert_equal(peer_node.getbestblockhash(), block_hash)


if __name__ == "__main__":
    MinerTimeOffsetTest().main()
