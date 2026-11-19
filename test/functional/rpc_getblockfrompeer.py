#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the getblockfrompeer RPC.

The getblockfrompeer RPC is a debugging only RPC which requests a block from
a given peer. The header of the block has to be known already, the block data
is then fetched from the peer.
"""
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.mininode import P2PDataStore
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, wait_until


class GetBlockFromPeerTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-whitelist=127.0.0.1", "-dummypos=1"]]

    def run_test(self):
        node = self.nodes[0]
        p2p = node.add_p2p_connection(P2PDataStore())

        genesis = int(node.getbestblockhash(), 16)
        block_time = node.getblock(node.getbestblockhash())["time"] + 1

        self.log.info("Build the active chain of the node")
        blocks = []
        tip = genesis
        for height in range(1, 3):
            block = create_block(tip, create_coinbase(height), block_time)
            block.solve()
            blocks.append(block)
            tip = block.sha256
            block_time += 1
        p2p.send_blocks_and_test(blocks, node, success=True)
        assert_equal(node.getblockcount(), 2)

        self.log.info("Build a block on a side chain and only send its header")
        # The side chain block has less work than the active chain, hence the
        # node does not download it on its own.
        side_block = create_block(genesis, create_coinbase(1), block_time)
        side_block.solve()
        side_block_hash = side_block.hash
        p2p.block_store[side_block.sha256] = side_block
        p2p.last_block_hash = side_block.sha256
        p2p.send_message(msg_headers([CBlockHeader(side_block)]))

        # The node knows the header of the block but not the block data.
        assert_raises_rpc_error(
            -1, "Block not found on disk", node.getblock, side_block_hash
        )

        peers = node.getpeerinfo()
        assert_equal(len(peers), 1)
        peer_id = peers[0]["id"]

        self.log.info("Arguments must be valid")
        assert_raises_rpc_error(
            -8, "hash must be of length 64", node.getblockfrompeer, "1234", peer_id
        )

        self.log.info("We must already have the header")
        assert_raises_rpc_error(
            -1, "Block header missing", node.getblockfrompeer, "00" * 32, peer_id
        )

        self.log.info("Non-existent peer generates an error")
        assert_raises_rpc_error(
            -1,
            "Peer does not exist",
            node.getblockfrompeer,
            side_block_hash,
            peer_id + 1,
        )

        self.log.info("Fetch the block from the peer")
        result = node.getblockfrompeer(side_block_hash, peer_id)
        assert_equal(result, {})

        def block_is_available():
            try:
                return node.getblock(side_block_hash)["hash"] == side_block_hash
            except Exception:
                return False

        wait_until(block_is_available, timeout=10)
        assert_equal(node.getblock(side_block_hash)["height"], 1)

        self.log.info("Don't fetch blocks we already have")
        assert_raises_rpc_error(
            -1,
            "Block already downloaded",
            node.getblockfrompeer,
            side_block_hash,
            peer_id,
        )


if __name__ == "__main__":
    GetBlockFromPeerTest().main()
