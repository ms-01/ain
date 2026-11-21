#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the DF25Height gated 2 hour future time rule.

Below the DF25 activation height the rule uses the legacy peer adjusted clock, so that the
acceptance window for historical blocks stays byte identical to the behaviour before the upgrade.
From the DF25 activation height on the unadjusted clock is used and the peer derived time offset
can no longer influence block acceptance.
"""

import os
import time
from io import BytesIO

from test_framework.messages import (
    NODE_NETWORK,
    NODE_WITNESS,
    CBlock,
)
from test_framework.mininode import (
    MAGIC_BYTES,
    NetworkThread,
    P2PDataStore,
    P2PInterface,
    msg_version,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# Clock skew of the test peers (in seconds). It has to stay below the -maxtimeadjustment bound
# (70 minutes) so that the legacy time offset is applied.
PEER_SKEW = 60 * 60
# The block is mined with a timestamp this many seconds in the future.
BLOCK_FUTURE = 3 * 60 * 60
# The legacy time offset is only applied with 5 or more samples (the filter is initialized with a
# single zero offset), so we need 4 peers with a skewed clock.
PEER_ADDRESSES = ["127.0.0.2", "127.0.0.3", "127.0.0.4", "127.0.0.5"]


class SkewedClockPeer(P2PInterface):
    """A peer with a skewed clock that connects from a dedicated loopback address.

    The legacy time offset tracker ignores duplicate peer addresses, so every peer needs its own
    loopback address to be counted.
    """

    def __init__(self, skew, local_addr):
        super().__init__()
        self.skew = skew
        self.local_addr = local_addr

    def peer_connect(self, dstaddr, dstport, net="regtest"):
        self.dstaddr = dstaddr
        self.dstport = dstport
        self.net = net
        self.on_connection_send_msg = None
        self.recvbuf = b""
        self.magic_bytes = MAGIC_BYTES[net]

        version = msg_version()
        version.nServices = NODE_NETWORK | NODE_WITNESS
        version.addrTo.ip = dstaddr
        version.addrTo.port = dstport
        version.addrFrom.ip = "0.0.0.0"
        version.addrFrom.port = 0
        version.nTime = int(time.time()) + self.skew
        self.on_connection_send_msg = version

        loop = NetworkThread.network_event_loop
        conn_gen_unsafe = loop.create_connection(
            lambda: self, host=dstaddr, port=dstport, local_addr=self.local_addr
        )
        return lambda: loop.call_soon_threadsafe(loop.create_task, conn_gen_unsafe)


def read_debug_log(node):
    with open(
        os.path.join(node.datadir, node.chain, "debug.log"), encoding="utf-8"
    ) as debug_log:
        return debug_log.read()


class DF25TimeTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            # node0: DF25 active, the unadjusted clock is used
            ["-df25height=1"],
            # node1: DF25 inactive, the legacy peer adjusted clock is used
            ["-df25height=100000000"],
            # node2: the miner of the block with the future timestamp
            [],
        ]

    def setup_network(self):
        # Keep the nodes isolated: the block under test is relayed by the test framework only, so
        # that the receiving nodes do not see it before the test arranges the time offsets.
        self.setup_nodes()

    def run_test(self):
        node_active = self.nodes[0]
        node_legacy = self.nodes[1]
        node_miner = self.nodes[2]

        # Mine a block with a timestamp that is in the future. The block itself is valid, only its
        # timestamp is beyond the acceptance window of the receiving nodes.
        block_time = int(time.time()) + BLOCK_FUTURE - 60
        node_miner.setmocktime(block_time)
        node_miner.generate(1)
        block_hash = node_miner.getbestblockhash()
        raw_block = node_miner.getblock(block_hash, 0)
        block = CBlock()
        block.deserialize(BytesIO(bytes.fromhex(raw_block)))
        block.rehash()
        assert_equal(block.nTime, block_time)
        assert_equal(block.hash, block_hash)

        # The node with the active DF25 rule rejects the block, because its timestamp is beyond
        # the (unadjusted) 2 hour window.
        p2p_active = node_active.add_p2p_connection(P2PDataStore())
        with node_active.assert_debug_log(expected_msgs=["time-too-new"]):
            p2p_active.send_blocks_and_test(
                [block], node_active, success=False, force_send=True, timeout=60
            )

        # The node below the activation height uses the legacy peer adjusted clock. We let four
        # peers with a skewed clock connect, so that the legacy time offset is applied.
        for address in PEER_ADDRESSES:
            node_legacy.add_p2p_connection(SkewedClockPeer(PEER_SKEW, (address, 0)))
        wait_until(
            lambda: "nTimeOffset = +{}".format(PEER_SKEW)
            in read_debug_log(node_legacy),
            timeout=60,
        )

        # The very same block is accepted: the legacy acceptance window is 2 hours plus the peer
        # derived offset.
        p2p_legacy = node_legacy.add_p2p_connection(P2PDataStore())
        p2p_legacy.send_blocks_and_test(
            [block], node_legacy, success=True, force_send=True, timeout=60
        )
        assert "time-too-new" not in read_debug_log(node_legacy)


if __name__ == "__main__":
    DF25TimeTest().main()
