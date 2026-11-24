#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test BLOCK_RELAY outbound connection slots (-blockrelayconnections)."""

import time

from test_framework.messages import (
    MSG_TX,
)
from test_framework.mininode import P2PInterface
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    p2p_port,
    wait_until,
)


class BlockRelayTestPeer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.tx_invs = []
        self.block_invs = []
        self.headers_received = []
        self.addr_received = 0
        self.getaddr_received = 0

    def on_inv(self, message):
        for inv in message.inv:
            if inv.type in (MSG_TX, 0x40000001, 5):  # MSG_TX, MSG_WITNESS_TX, MSG_WTX
                self.tx_invs.append(inv.hash)
            elif inv.type in (2, 0x40000002):  # MSG_BLOCK, MSG_WITNESS_BLOCK
                self.block_invs.append(inv.hash)

    def on_headers(self, message):
        self.headers_received.extend(message.headers)

    def on_addr(self, message):
        self.addr_received += 1

    def on_addrv2(self, message):
        self.addr_received += 1

    def on_getaddr(self, message):
        self.getaddr_received += 1


class BlockRelayOnlyTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [
            ["-blockrelayconnections=0"],
            ["-blockrelayconnections=1"],
            ["-blockrelayconnections=0"],
        ]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        # 1. Startup CLI validation (E7 / M5)
        self.log.info("Test CLI validation for -blockrelayconnections")
        self.stop_node(2)
        self.nodes[2].assert_start_raises_init_error(
            extra_args=["-blockrelayconnections=-1"],
            expected_msg="Error: -blockrelayconnections cannot be negative",
        )
        self.nodes[2].assert_start_raises_init_error(
            extra_args=["-maxconnections=10", "-blockrelayconnections=11"],
            expected_msg="Error: -blockrelayconnections (11) cannot exceed -maxconnections (10)",
        )
        self.start_node(2, extra_args=["-blockrelayconnections=0"])

        # 2. Default-off (-blockrelayconnections=0) rejects block-relay-only addconnection
        self.log.info("Test default-off (-blockrelayconnections=0) behaviour")
        addr_node2 = f"127.0.0.1:{p2p_port(2)}"
        assert_raises_rpc_error(
            -1,
            "Unable to add block-relay-only connection",
            self.nodes[0].addconnection,
            addr_node2,
            "block-relay-only",
        )
        # Opening an outbound-full-relay connection succeeds and reports connection_type
        res = self.nodes[0].addconnection(addr_node2, "outbound-full-relay")
        assert_equal(res["connection_type"], "outbound-full-relay")
        wait_until(lambda: len(self.nodes[0].getpeerinfo()) == 1, timeout=10)
        assert_equal(self.nodes[0].getpeerinfo()[0]["connection_type"], "outbound-full-relay")
        wait_until(lambda: len(self.nodes[2].getpeerinfo()) == 1, timeout=10)
        assert_equal(self.nodes[2].getpeerinfo()[0]["connection_type"], "inbound")

        # 3. Opt-in (-blockrelayconnections=1) on node 1
        self.log.info("Test opt-in (-blockrelayconnections=1) slot enforcement and relay isolation")
        start_height = self.nodes[1].getblockcount()

        # Open 1 block-relay-only connection from node 1 -> node 2
        res = self.nodes[1].addconnection(addr_node2, "block-relay-only")
        assert_equal(res["connection_type"], "block-relay-only")
        wait_until(
            lambda: any(
                p["connection_type"] == "block-relay-only" and p["version"] > 0
                for p in self.nodes[1].getpeerinfo()
            ),
            timeout=10,
        )
        br_peers = [p for p in self.nodes[1].getpeerinfo() if p["connection_type"] == "block-relay-only"]
        assert_equal(len(br_peers), 1)
        assert_equal(br_peers[0]["relaytxes"], False)

        # Attempting a 2nd block-relay-only connection beyond -blockrelayconnections=1 fails
        addr_node0 = f"127.0.0.1:{p2p_port(0)}"
        assert_raises_rpc_error(
            -1,
            "Unable to add block-relay-only connection",
            self.nodes[1].addconnection,
            addr_node0,
            "block-relay-only",
        )

        # Connect a full-relay P2P peer to node 1 as control
        full_peer = self.nodes[1].add_p2p_connection(BlockRelayTestPeer())
        full_peer.sync_with_ping()

        # Send a transaction on node 1: full-relay peer receives tx INV, block-relay-only peer (node 2) does NOT
        dest_addr = self.nodes[1].getnewaddress()
        txid = self.nodes[1].sendtoaddress(dest_addr, 1.0)
        wait_until(lambda: len(full_peer.tx_invs) > 0, timeout=10)
        # Give node 1 time to trickle any tx inv if it were going to (it must not)
        time.sleep(2)
        assert_equal(self.nodes[2].getrawmempool(), [])

        # Mine a block on node 1 containing the tx: node 2 receives the block
        self.nodes[1].generate(1)
        wait_until(lambda: self.nodes[2].getblockcount() == start_height + 1, timeout=15)
        assert_equal(self.nodes[2].getbestblockhash(), self.nodes[1].getbestblockhash())
        assert txid in self.nodes[2].getblock(self.nodes[2].getbestblockhash())["tx"]

        # Verify node 1 did not send GETADDR to its block-relay-only peer (node 2)
        node2_peers_from_1 = [
            p for p in self.nodes[2].getpeerinfo()
            if p["connection_type"] == "inbound" and p["bytesrecv_per_msg"].get("getaddr", 0) > 0
        ]
        # Only node 0 (outbound-full-relay) should have sent getaddr to node 2, not node 1 (block-relay-only)
        assert_equal(len(node2_peers_from_1), 1)


if __name__ == "__main__":
    BlockRelayOnlyTest().main()
