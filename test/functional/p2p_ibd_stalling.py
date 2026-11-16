#!/usr/bin/env python3
# Copyright (c) 2024-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the adaptive block download stalling timeout and the manual peer cooldown.

The node disconnects peers that stall the block download. To avoid disconnecting peer
after peer when the local bandwidth is not sufficient to download a block within the
stalling timeout, the timeout is doubled whenever a peer is disconnected for stalling and
decays slowly back to its default value whenever a block is connected.

Manual peers (-connect / -addnode) are not disconnected when they stall the block
download. Instead, block downloads from them are paused for a cooldown period and their
outstanding block requests are released, so that the blocks can be requested from other
peers again.

The stalling detection of the node requires the block download window to be full of
blocks that are in flight, but not delivered. Therefore a number of peers is needed to
fill the window: every peer holds up to MAX_BLOCKS_IN_TRANSIT_PER_PEER blocks in flight.
"""

import os
import re
import time

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    MSG_BLOCK,
    MSG_TYPE_MASK,
    msg_block,
    msg_headers,
)
from test_framework.mininode import (
    P2PDataStore,
    P2PListener,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)

# Size of the block download window of the node.
BLOCK_DOWNLOAD_WINDOW = 1024
# Number of blocks that can be in flight per peer.
MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16
# Number of peers that are needed to fill the block download window with blocks that are
# in flight, but not delivered.
NUM_BLOCK_HOLDERS = BLOCK_DOWNLOAD_WINDOW // MAX_BLOCKS_IN_TRANSIT_PER_PEER
# Number of peers that do not hold any block in flight and therefore report the peer that
# holds the block at the end of the download window as the stalling peer.
NUM_REPORTERS = 2
# Default stalling timeout of the node, in seconds.
DEFAULT_STALLING_TIMEOUT = 2


class P2PStaller(P2PDataStore):
    """A P2P peer that does not serve the blocks it is supposed to stall on."""

    def __init__(self, stall_blocks=None):
        super().__init__()
        self.stall_blocks = stall_blocks if stall_blocks is not None else []

    def on_getdata(self, message):
        for inv in message.inv:
            self.getdata_requests.append(inv.hash)
            if (inv.type & MSG_TYPE_MASK) == MSG_BLOCK:
                if inv.hash not in self.stall_blocks:
                    self.send_message(msg_block(self.block_store[inv.hash]))

    def on_getheaders(self, message):
        pass


class P2PStallingTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.mocktime = int(time.time()) + 1

    def run_test(self):
        self.test_stalling()
        self.test_manual_peer_stalling()

    # Helpers

    def build_blocks(self, count):
        """Create a chain of blocks on top of the current tip of the node."""
        node = self.nodes[0]
        tip = int(node.getbestblockhash(), 16)
        height = node.getblockcount() + 1
        block_time = node.getblock(node.getbestblockhash())['time'] + 1
        blocks = []
        block_dict = {}
        for _ in range(count):
            block = create_block(tip, create_coinbase(height), block_time)
            block.nVersion = 4
            block.solve()
            blocks.append(block)
            tip = block.sha256
            block_time += 1
            height += 1
            block_dict[block.sha256] = block
        return blocks, block_dict

    def headers_message(self, blocks):
        headers = msg_headers()
        headers.headers = [CBlockHeader(block) for block in blocks]
        return headers

    def sync_peers(self, peers):
        """Make sure that the message loop of the node ran for all given peers."""
        for peer in peers:
            if peer.is_connected:
                peer.sync_with_ping()

    def advance_mocktime(self, node, seconds, peers):
        """Advance the mocktime of the node and let it process its messages."""
        self.mocktime += seconds
        node.setmocktime(self.mocktime)
        self.sync_peers(peers)

    def debug_log(self):
        node = self.nodes[0]
        log_path = os.path.join(node.datadir, node.chain, "debug.log")
        with open(log_path, encoding="utf-8") as log_file:
            return log_file.read()

    def paused_peer_id(self):
        """Return the id of the peer that was paused for stalling, if any."""
        match = re.search(r'Pausing block downloads from stalling manual peer=(\d+)', self.debug_log())
        return int(match.group(1)) if match else None

    def peer_inflight(self, node, peer_id):
        """Return the heights of the blocks that are in flight for the given peer."""
        for peer_info in node.getpeerinfo():
            if peer_info['id'] == peer_id:
                return peer_info['inflight']
        return None

    def connect_stalling_peers(self, node, blocks, block_dict, num_peers):
        """Connect peers that announce the given chain, but do not serve any block."""
        headers = self.headers_message(blocks)
        peers = []
        for _ in range(num_peers):
            peer = node.add_p2p_connection(P2PStaller(list(block_dict.keys())))
            peer.block_store = block_dict
            peers.append(peer)
        for peer in peers:
            peer.send_message(headers)
        self.sync_peers(peers)
        return peers

    def test_stalling(self):
        self.log.info("Test that a stalling peer is disconnected and the timeout is increased")
        node = self.nodes[0]
        self.restart_node(0)

        # The node needs to know about a chain that is longer than its block download
        # window, so that it detects a stalling peer.
        blocks, block_dict = self.build_blocks(BLOCK_DOWNLOAD_WINDOW + 1)

        node.setmocktime(self.mocktime)

        # The peers hold the blocks of the download window in flight, but never deliver
        # them. The last peers do not hold any block in flight and report the peer that
        # holds the block at the end of the download window as the stalling peer.
        with node.assert_debug_log(expected_msgs=['Stall started'], timeout=120):
            peers = self.connect_stalling_peers(
                node, blocks, block_dict, NUM_BLOCK_HOLDERS + NUM_REPORTERS)
            wait_until(lambda: len(node.getpeerinfo()) == len(peers), timeout=60)
            self.sync_peers(peers)

        self.log.info("Check that the stalling peer is disconnected after the stalling timeout")
        with node.assert_debug_log(expected_msgs=[
                'is stalling block download, disconnecting',
                'Increased stalling timeout temporarily to 4 seconds'], timeout=60):
            self.advance_mocktime(node, DEFAULT_STALLING_TIMEOUT + 1, peers)
        peers[0].wait_for_disconnect()
        assert_equal(len(node.getpeerinfo()), len(peers) - 1)

        self.log.info("Check that the next stalling peer is disconnected with the doubled timeout")
        with node.assert_debug_log(expected_msgs=[
                'is stalling block download, disconnecting',
                'Increased stalling timeout temporarily to 8 seconds'], timeout=120):
            self.advance_mocktime(node, 5, peers)
        self.sync_peers(peers[1:])
        assert_equal(len(node.getpeerinfo()), len(peers) - 2)

        self.log.info("Check that the stalling timeout is reduced again when blocks are connected")
        # Disconnect the stalling peers, so that the blocks are requested from a peer that
        # serves them.
        for peer in peers:
            peer.peer_disconnect()
        wait_until(lambda: len(node.getpeerinfo()) == 0, timeout=60)

        with node.assert_debug_log(expected_msgs=['Decreased stalling timeout to 2 seconds'], timeout=120):
            serving_peer = node.add_p2p_connection(P2PDataStore())
            serving_peer.block_store = block_dict
            serving_peer.send_message(self.headers_message(blocks))
            self.sync_peers([serving_peer])
            # The stalling timeout decays with every connected block (8 -> 6 -> 5 -> 4 -> 3 -> 2).
            wait_until(lambda: node.getblockcount() >= 5, timeout=60)
            self.sync_peers([serving_peer])

    def test_manual_peer_stalling(self):
        self.log.info("Test that a manual peer is paused, but not disconnected, for stalling")
        node = self.nodes[0]

        blocks, block_dict = self.build_blocks(BLOCK_DOWNLOAD_WINDOW + 1)

        # The node connects to the listeners, so that the peers are manual peers.
        def connection_factory():
            return P2PStaller(list(block_dict.keys()))

        listeners = [P2PListener(connection_factory) for _ in range(NUM_BLOCK_HOLDERS + NUM_REPORTERS)]
        ports = [listener.start() for listener in listeners]

        node.set_mocktime(self.mocktime)
        self.restart_node(0, extra_args=['-connect=127.0.0.1:{}'.format(port) for port in ports])

        peers = []
        for listener in listeners:
            peer = listener.wait_for_connection()
            peer.block_store = block_dict
            peers.append(peer)

        # The peers are manual peers.
        for peer_info in node.getpeerinfo():
            assert_equal(peer_info['addnode'], True)

        headers = self.headers_message(blocks)

        with node.assert_debug_log(expected_msgs=['Stall started'], timeout=120):
            for peer in peers:
                peer.send_message(headers)
            wait_until(lambda: len(node.getpeerinfo()) == len(peers), timeout=60)
            self.sync_peers(peers)

        self.log.info("Check that the manual peer is paused instead of disconnected")
        with node.assert_debug_log(expected_msgs=[
                'Pausing block downloads from stalling manual peer',
                'Increased stalling timeout temporarily to 4 seconds'], timeout=60):
            self.advance_mocktime(node, DEFAULT_STALLING_TIMEOUT + 1, peers)

        # The manual peers are still connected.
        assert_equal(len(node.getpeerinfo()), len(peers))
        for peer in peers:
            assert_equal(peer.is_connected, True)

        self.log.info("Check that the outstanding block requests of the manual peer are released")
        paused_peer_id = self.paused_peer_id()
        assert paused_peer_id is not None
        assert_equal(self.peer_inflight(node, paused_peer_id), [])

        for listener in listeners:
            listener.stop()


if __name__ == '__main__':
    P2PStallingTest().main()
