#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the block filter index in combination with block pruning.

The block filter index needs the block data and the undo data of every block
which it has not processed yet. Therefore the pruning code must never remove
data which is still needed by a running index. This is implemented with prune
locks, which bound the automatic as well as the manual prune path.

The test uses the -fastprune debug option (regtest only) which uses much
smaller block files, so that the pruning code can be exercised without writing
gigabytes of block data.
"""

import os

from test_framework.test_framework import DefiTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Number of blocks which are kept by the node and are never pruned.
MIN_BLOCKS_TO_KEEP = 288

# The index is started with this name (see BaseIndex::GetName()).
INDEX_NAME = "basic block filter index"


class BlockFilterIndexPruneTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1", "-blockfilterindex=basic"]]

    def blocks_dir(self):
        return os.path.join(self.nodes[0].datadir, self.chain, "blocks")

    def has_block_file(self, index):
        return os.path.isfile(
            os.path.join(self.blocks_dir(), "blk{:05}.dat".format(index))
        )

    def debug_log(self):
        with open(
            os.path.join(self.nodes[0].datadir, self.chain, "debug.log"),
            encoding="utf-8",
        ) as f:
            return f.read()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Check that the node starts with -prune and -blockfilterindex")
        # The startup rejection of the combination must be gone.
        assert (
            "Prune mode is incompatible with -blockfilterindex" not in self.debug_log()
        )
        assert_equal(node.getblockchaininfo()["pruneheight"], 0)

        self.log.info(
            "Mine blocks so that the pruning window contains complete block files"
        )
        # The pruning window (MIN_BLOCKS_TO_KEEP) must be large enough to contain
        # at least one complete block file, otherwise nothing can be pruned.
        node.generate(MIN_BLOCKS_TO_KEEP + 200)
        height = node.getblockcount()
        assert_equal(height, MIN_BLOCKS_TO_KEEP + 200)

        self.log.info("Check that the filter index is working")
        tip = node.getbestblockhash()
        assert_greater_than(len(node.getblockfilter(tip, "basic")["filter"]), 0)

        self.log.info("Prune the blocks which are not needed anymore")
        with node.assert_debug_log(["{} limited pruning to height".format(INDEX_NAME)]):
            prune_height = node.pruneblockchain(height - MIN_BLOCKS_TO_KEEP)
        # Anti-vacuity guard: the pruning must really have removed data.
        assert_greater_than(prune_height, 0)
        assert_greater_than(node.getblockchaininfo()["pruneheight"], 0)
        assert not self.has_block_file(
            0
        ), "blk00000.dat is missing when should still be there"
        self.log.info("Pruned up to height %d" % prune_height)

        self.log.info("Check that the filter index still works after the pruning")
        # The filter data is stored in the index database, so it is still
        # available for blocks which have been pruned.
        for block_hash in [tip, node.getblockhash(1), node.getblockhash(prune_height)]:
            assert_greater_than(
                len(node.getblockfilter(block_hash, "basic")["filter"]), 0
            )
        assert "Failed to read block" not in self.debug_log()

        self.log.info("Restart the node and check that the index is still in sync")
        self.restart_node(
            0, extra_args=["-fastprune", "-prune=1", "-blockfilterindex=basic"]
        )
        node = self.nodes[0]
        assert_greater_than(
            len(node.getblockfilter(node.getbestblockhash(), "basic")["filter"]), 0
        )
        assert "Failed to read block" not in self.debug_log()

        self.log.info("Check that the prune lock is moved back on a reorg")
        fork_height = node.getblockcount() - 2
        with node.assert_debug_log(["{} prune lock moved back to".format(INDEX_NAME)]):
            node.invalidateblock(node.getblockhash(fork_height))
        # The index must still work after the reorg.
        assert_greater_than(
            len(node.getblockfilter(node.getbestblockhash(), "basic")["filter"]), 0
        )

        self.log.info("Prune beyond the best block of the index (index disabled)")
        # Restart the node without the index, so that the index does not catch
        # up with the chain and the pruning is not limited by the prune lock.
        self.stop_node(0)
        self.start_node(0, extra_args=["-fastprune", "-prune=1"])
        node = self.nodes[0]
        index_best = node.getblockcount()
        prune_height = 0
        # Mine and prune until the pruned data lies beyond the best block of the
        # index (the pruning is limited to the last MIN_BLOCKS_TO_KEEP blocks
        # and to the block file boundaries).
        for _ in range(20):
            node.generate(100)
            height = node.getblockcount()
            prune_height = node.pruneblockchain(height - MIN_BLOCKS_TO_KEEP)
            if prune_height > index_best:
                break
        assert_greater_than(prune_height, index_best)
        self.log.info(
            "Pruned up to height %d (index best block is at %d)"
            % (prune_height, index_best)
        )
        self.stop_node(0)

        self.log.info("Check that the node fails fast when the index is enabled again")
        expected_msg = "best block of the index goes beyond pruned data"
        node.assert_start_raises_init_error(
            extra_args=["-fastprune", "-prune=1", "-blockfilterindex=basic"],
            expected_msg=expected_msg,
            match=ErrorMatch.PARTIAL_REGEX,
        )

        self.log.info("Check that the node starts again without the index")
        self.start_node(0, extra_args=["-fastprune", "-prune=1"])
        node = self.nodes[0]
        assert_equal(node.getblockcount(), height)
        # The block filter index is not enabled, so the RPC must fail.
        assert_raises_rpc_error(
            -1,
            "Index is not enabled for filtertype basic",
            node.getblockfilter,
            node.getbestblockhash(),
            "basic",
        )

        self.log.info("Done")


if __name__ == "__main__":
    BlockFilterIndexPruneTest().main()
