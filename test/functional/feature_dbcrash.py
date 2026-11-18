#!/usr/bin/env python3
# Copyright (c) 2017-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test recovery from a crash during chainstate writing.

DeFiChain specific behaviour
----------------------------
Unlike Bitcoin Core, a node that crashed while writing the chainstate cannot
recover on its own: CChainState::ReplayBlocks() is disabled for safety
reasons, because the custom state (masternode/account, burn and vault
history) cannot be rolled back and replayed.  A node with an inconsistent
coins database therefore refuses to start ("Unable to replay blocks ...") and
has to be recovered with -reindex-chainstate, which rebuilds the chainstate
from the block index.

This test verifies the recovery procedure that is supported by DeFiChain:

- simulate a crash during a chainstate write (-dbcrashratio),
- check that the node refuses to start afterwards,
- recover the node with -reindex-chainstate,
- check that the rebuilt chainstate is identical to the chainstate of a node
  that never crashed.

- 4 nodes
  * node0, node1, and node2 will have different dbcrash ratios, and different
    dbcache sizes
  * node3 will be a regular node, with no crashing.
  * The nodes will not connect to each other.

- use default test framework starting chain. initialize starting_tip_height to
  tip height.

- Main loop:
  * generate transactions on node3, enough to fill up a block.
  * uniformly randomly pick a tip height from starting_tip_height to
    tip_height; with probability 1/(height_difference+4), invalidate this block.
  * mine enough blocks to overtake tip_height at start of loop.
  * for each node in [node0,node1,node2]:
     - for each mined block:
       * submit block to node
       * if node crashed on/after submitting:
         - recover the node (-reindex-chainstate)
         - check that utxo matches node3 using gettxoutsetinfo
     - flush the chainstate of the node to disk (this is where the simulated
       crash happens) and check the utxo set again
"""

import errno
import http.client
import os
import random
import time

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    ToHex,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    create_confirmed_utxos,
    hex_str_to_bytes,
)

# Number of utxos that are created on the reference node at the beginning of
# the test.  This is deliberately small: every crash requires a full rebuild
# of the chainstate (-reindex-chainstate), which takes a few seconds for the
# chain of this test.
INITIAL_UTXO_COUNT = 200
# Number of transactions that are created per iteration of the main loop.
TRANSACTIONS_PER_ITERATION = 100
# Number of iterations of the main loop.
MAIN_LOOP_ITERATIONS = 10
# Number of attempts to recover a node that crashed.
RECOVERY_ATTEMPTS = 3
# Timeout (seconds) that is used to wait for a node to finish its start-up.
STARTUP_TIMEOUT = 120


class ChainstateWriteCrashTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = False
        self.rpc_timeout = 180

        # Set -maxmempool=0 to turn off mempool memory sharing with dbcache
        # Set -rpcservertimeout=900 to reduce socket disconnects in this
        # long-running test
        self.base_args = [
            "-limitdescendantsize=0",
            "-maxmempool=0",
            "-rpcservertimeout=900",
            # Keep the batches that are written to the coins database small, so
            # that a chainstate write is split into several partial batches
            # even for the small workload of this test.  Only a partial batch
            # can trigger the simulated crash (-dbcrashratio).
            "-dbbatchsize=5000",
        ]

        # Set different crash ratios and cache sizes.  Note that not all of
        # -dbcache goes to the in-memory coins cache.
        self.node0_args = ["-dbcrashratio=8", "-dbcache=4"] + self.base_args
        self.node1_args = ["-dbcrashratio=16", "-dbcache=8"] + self.base_args
        self.node2_args = ["-dbcrashratio=24", "-dbcache=16"] + self.base_args

        # Node3 is a normal node with default args, except will mine full blocks
        # and non-standard txs (e.g. txs with "dust" outputs)
        self.node3_args = ["-blockmaxweight=4000000", "-acceptnonstdtxn"]
        self.extra_args = [
            self.node0_args,
            self.node1_args,
            self.node2_args,
            self.node3_args,
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args)
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()
        # Leave them unconnected, we'll use submitblock directly in this test

    # ------------------------------------------------------------------
    # helpers to inspect and control the nodes
    # ------------------------------------------------------------------

    def node_args(self, node_index, extra=None, without_crash_ratio=False):
        """Return the command line arguments of a node."""
        args = list(self.extra_args[node_index])
        if without_crash_ratio:
            args = [arg for arg in args if not arg.startswith("-dbcrashratio")]
        if extra:
            args = args + list(extra)
        return args

    def debug_log_path(self, node_index):
        return os.path.join(self.nodes[node_index].datadir, self.chain, "debug.log")

    def log_size(self, node_index):
        """Return the current size of the debug log of a node."""
        try:
            return os.path.getsize(self.debug_log_path(node_index))
        except OSError:
            return 0

    def log_contains(self, node_index, offset, needle):
        """Check if the debug log of a node contains a string after an offset."""
        try:
            with open(
                self.debug_log_path(node_index), encoding="utf-8", errors="replace"
            ) as log_file:
                log_file.seek(offset)
                return needle in log_file.read()
        except OSError:
            return False

    def node_stderr(self, node_index):
        """Return the stderr output of the current run of a node."""
        stderr = self.nodes[node_index].stderr
        if stderr is None:
            return ""
        try:
            with open(stderr.name, encoding="utf-8", errors="replace") as stderr_file:
                return stderr_file.read()
        except OSError:
            return ""

    def node_refused_to_start(self, node_index):
        """Check if a node aborted because the coins database is inconsistent.

        DeFiChain cannot replay an interrupted chainstate write, so the node
        aborts during initialisation with this error message.
        """
        return "Unable to replay blocks" in self.node_stderr(node_index)

    def start_node_silently(self, node_index, extra_args=None):
        """Start a node without waiting for its RPC connection.

        The RPC of a node that aborts during initialisation must not be
        polled: the RPC worker waits for cs_main while the main thread waits
        for the HTTP event thread, which delays the exit of the node by the
        full RPC timeout.  The state of the node is therefore inspected
        through its log files instead.
        """
        node = self.nodes[node_index]
        if node.process is not None and node.process.poll() is None:
            # Make sure that no process of a previous run is left behind.
            self.kill_node(node_index)
        node.start(extra_args)

    def kill_node(self, node_index):
        """Kill a node that does not exit on its own and reset its state."""
        node = self.nodes[node_index]
        if node.process is not None and node.process.poll() is None:
            self.log.debug("Killing node %d", node_index)
            node.process.kill()
            node.process.wait()
        node.running = False
        node.process = None
        node.rpc_connected = False
        node.rpc = None
        node.evm_rpc = None
        for stream in (node.stderr, node.stdout):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass

    def wait_for_tip(self, node_index, block_hash, timeout=STARTUP_TIMEOUT):
        """Wait until a block is part of the active chain of a node.

        A node that rebuilds its chainstate connects the blocks in the
        background, so the node is only usable again once it caught up.
        """
        expected_height = self.nodes[3].getblock(block_hash)["height"]
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.nodes[node_index].process.poll() is not None:
                raise AssertionError(
                    "Node %d died while it was catching up" % node_index
                )
            try:
                if self.nodes[node_index].getblockcount() >= expected_height:
                    block = self.nodes[node_index].getblock(block_hash)
                    # A block that is not on the active chain has -1
                    # confirmations.
                    if block.get("confirmations", -1) != -1:
                        return
            except Exception:
                pass
            time.sleep(0.5)
        raise AssertionError(
            "Node %d did not reach block %s" % (node_index, block_hash)
        )

    def flush_node(self, node_index):
        """Flush the chainstate of a node to disk and return its utxo hash.

        gettxoutsetinfo() forces a flush of the coins database, which is where
        the simulated crash (-dbcrashratio) happens.  None is returned if the
        node crashed while flushing.
        """
        try:
            return self.nodes[node_index].gettxoutsetinfo()["hash_serialized_2"]
        except Exception as e:
            if self.nodes[node_index].process.poll() is None:
                # The node is still running, so this is not a crash.
                raise
            self.log.debug("node %d crashed while flushing: %s", node_index, e)
            return None

    # ------------------------------------------------------------------
    # crash recovery
    # ------------------------------------------------------------------

    def try_plain_restart(self, node_index):
        """Try to restart a node without rebuilding its chainstate.

        Returns True if the node came up, False if it refused to start.  A
        node that crashed while writing the chainstate always refuses to
        start, because ReplayBlocks() is disabled on DeFiChain.
        """
        log_offset = self.log_size(node_index)
        self.start_node_silently(node_index, self.node_args(node_index))
        deadline = time.time() + STARTUP_TIMEOUT
        while time.time() < deadline:
            if self.node_refused_to_start(node_index):
                self.log.info(
                    "Node %d refused to start: the chainstate has to be rebuilt",
                    node_index,
                )
                self.kill_node(node_index)
                return False
            if self.nodes[node_index].process.poll() is not None:
                # The node exited during initialisation.
                self.kill_node(node_index)
                return False
            if self.log_contains(node_index, log_offset, "Done loading"):
                self.nodes[node_index].wait_for_rpc_connection()
                return True
            time.sleep(0.2)
        self.kill_node(node_index)
        return False

    def rebuild_chainstate(self, node_index):
        """Rebuild the chainstate of a node from its block index.

        This is the recovery procedure that is supported by DeFiChain.  The
        rebuild is done without the crash simulation, so that the recovery of
        a node cannot be interrupted by another crash.  The crash simulation
        is enabled again once the node is recovered.
        """
        self.log.info("Rebuilding the chainstate of node %d", node_index)
        self.start_node_silently(
            node_index,
            self.node_args(
                node_index, extra=["-reindex-chainstate"], without_crash_ratio=True
            ),
        )
        self.nodes[node_index].wait_for_rpc_connection()

    def enable_crash_simulation(self, node_index):
        """Restart a node with the crash simulation enabled again.

        The recovery of a node is done without the crash simulation, so that
        it cannot be interrupted by another crash.  The crash simulation is
        enabled again with a restart of the node; the chainstate is
        consistent at that point.
        """
        node = self.nodes[node_index]
        if node.process is not None and node.process.poll() is None:
            # The node is killed instead of stopped: the shutdown would flush
            # the coins cache, which is not needed here.
            self.kill_node(node_index)
        self.start_node_silently(node_index, self.node_args(node_index))
        self.nodes[node_index].wait_for_rpc_connection()

    def restart_node(self, node_index, expected_tip):
        """Recover a node that crashed and wait until it caught up again.

        A node that crashed while writing the chainstate cannot be restarted
        normally: it refuses to start because the coins database is
        inconsistent.  The node is therefore recovered by rebuilding its
        chainstate from the block index (-reindex-chainstate).

        Returns the utxo hash of the recovered node.
        """
        self.log.info(
            "Recovering node %d (expected tip %s)", node_index, expected_tip
        )

        for attempt in range(RECOVERY_ATTEMPTS):
            if self.try_plain_restart(node_index):
                self.log.info(
                    "Node %d restarted without a chainstate rebuild", node_index
                )
            else:
                # The node refused to start: the interrupted chainstate write
                # has to be rebuilt.
                self.crashed_on_restart += 1
                self.rebuild_chainstate(node_index)
                self.rebuild_counts[node_index] += 1

            try:
                # A node that rebuilds its chainstate connects the blocks in
                # the background, so wait until it caught up again.
                self.wait_for_tip(node_index, expected_tip)
                utxo_hash = self.flush_node(node_index)
            except Exception as e:
                self.log.info("Node %d crashed while recovering: %s", node_index, e)
                utxo_hash = None
            if utxo_hash is not None:
                # The crash simulation is enabled again for the rest of the
                # test.
                self.enable_crash_simulation(node_index)
                return utxo_hash
            # The node crashed again while its chainstate was flushed.
            self.crashed_on_restart += 1

        raise AssertionError("Unable to recover node %d" % node_index)

    # ------------------------------------------------------------------
    # test steps
    # ------------------------------------------------------------------

    def submit_block_catch_error(self, node_index, block):
        """Try submitting a block to the given node.

        Catch any exceptions that indicate the node has crashed.
        Returns true if the block was submitted successfully; false otherwise."""

        try:
            self.nodes[node_index].submitblock(block)
            return True
        except (http.client.CannotSendRequest, http.client.RemoteDisconnected) as e:
            self.log.debug("node %d submitblock raised exception: %s", node_index, e)
            return False
        except OSError as e:
            self.log.debug(
                "node %d submitblock raised OSError exception: errno=%s",
                node_index,
                e.errno,
            )
            if e.errno in [errno.EPIPE, errno.ECONNREFUSED, errno.ECONNRESET]:
                # The node has likely crashed
                return False
            else:
                # Unexpected exception, raise
                raise

    def sync_node3blocks(self, block_hashes):
        """Use submitblock to sync node3's chain with the other nodes

        If submitblock fails, recover the node and get the new utxo hash.
        If any nodes crash while updating, we'll compare utxo hashes to
        ensure recovery was successful."""

        node3_utxo_hash = self.nodes[3].gettxoutsetinfo()["hash_serialized_2"]

        # Retrieve all the blocks from node3
        blocks = []
        for block_hash in block_hashes:
            blocks.append([block_hash, self.nodes[3].getblock(block_hash, 0)])

        # Deliver each block to each other node
        for i in range(3):
            self.log.debug("Syncing blocks to node %d", i)
            for block_hash, block in blocks:
                self.log.debug("submitting block %s", block_hash)
                if not self.submit_block_catch_error(i, block):
                    # TODO: more carefully check that the crash is due to -dbcrashratio
                    # (change the exit code perhaps, and check that here?)
                    self.wait_for_node_exit(i, timeout=30)
                    self.log.debug(
                        "Restarting node %d after block hash %s", i, block_hash
                    )
                    self.restart_node(i, block_hash)
                    self.restart_counts[i] += 1

            # Flush the chainstate of the node to disk.  This is where the
            # simulated crash (-dbcrashratio) happens, so the node may have to
            # be recovered before its utxo set can be compared.
            self.log.debug("Flushing the chainstate of node %d", i)
            utxo_hash = self.flush_node(i)
            if utxo_hash is None:
                self.wait_for_node_exit(i, timeout=30)
                self.log.debug("Recovering node %d after the flush", i)
                utxo_hash = self.restart_node(i, self.nodes[3].getbestblockhash())
                self.restart_counts[i] += 1

            # Check that the utxo hash matches node3's utxo set
            self.log.debug("Checking txoutsetinfo matches for node %d", i)
            assert_equal(utxo_hash, node3_utxo_hash)

    def verify_utxo_hash(self):
        """Verify that the utxo hash of each node matches node3.

        Recover any nodes that crash while querying."""
        node3_utxo_hash = self.nodes[3].gettxoutsetinfo()["hash_serialized_2"]
        self.log.info("Verifying utxo hash matches for all nodes")

        for i in range(3):
            utxo_hash = self.flush_node(i)
            if utxo_hash is None:
                self.wait_for_node_exit(i, timeout=30)
                utxo_hash = self.restart_node(i, self.nodes[3].getbestblockhash())
            assert_equal(utxo_hash, node3_utxo_hash)

    def generate_small_transactions(self, node, count, utxo_list):
        FEE = 1000  # TODO: replace this with node relay fee based calculation
        num_transactions = 0
        random.shuffle(utxo_list)
        while len(utxo_list) >= 2 and num_transactions < count:
            tx = CTransaction()
            input_amount = 0
            for i in range(2):
                utxo = utxo_list.pop()
                tx.vin.append(CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"])))
                input_amount += int(utxo["amount"] * COIN)
            output_amount = (input_amount - FEE) // 3

            if output_amount <= 0:
                # Sanity check -- if we chose inputs that were too small, skip
                continue

            for i in range(3):
                tx.vout.append(
                    CTxOut(output_amount, hex_str_to_bytes(utxo["scriptPubKey"]))
                )

            # Sign and send the transaction to get into the mempool
            tx_signed_hex = node.signrawtransactionwithwallet(ToHex(tx))["hex"]
            node.sendrawtransaction(tx_signed_hex)
            num_transactions += 1

    def run_test(self):
        # Track test coverage statistics
        self.restart_counts = [0, 0, 0]  # Track the restarts for nodes 0-2
        self.rebuild_counts = [0, 0, 0]  # Track the chainstate rebuilds for nodes 0-2
        self.crashed_on_restart = 0  # Track count of crashes during recovery

        # Start by creating a lot of utxos on node3
        initial_height = self.nodes[3].getblockcount()
        utxo_list = create_confirmed_utxos(
            self.nodes[3].getnetworkinfo()["relayfee"],
            self.nodes[3],
            INITIAL_UTXO_COUNT,
        )
        self.log.info("Prepped %d utxo entries", len(utxo_list))

        # Sync these blocks with the other nodes
        block_hashes_to_sync = []
        for height in range(initial_height + 1, self.nodes[3].getblockcount() + 1):
            block_hashes_to_sync.append(self.nodes[3].getblockhash(height))

        self.log.debug("Syncing %d blocks with other nodes", len(block_hashes_to_sync))
        # Syncing the blocks could cause nodes to crash, so the test begins here.
        self.sync_node3blocks(block_hashes_to_sync)

        starting_tip_height = self.nodes[3].getblockcount()

        # Main test loop:
        # each time through the loop, generate a bunch of transactions,
        # and then either mine a single new block on the tip, or some-sized reorg.
        for i in range(MAIN_LOOP_ITERATIONS):
            self.log.info(
                "Iteration %d, generating %d transactions %s",
                i,
                TRANSACTIONS_PER_ITERATION,
                self.restart_counts,
            )
            # Generate a bunch of small-ish transactions
            self.generate_small_transactions(
                self.nodes[3], TRANSACTIONS_PER_ITERATION, utxo_list
            )
            # Pick a random block between current tip, and starting tip
            current_height = self.nodes[3].getblockcount()
            random_height = random.randint(starting_tip_height, current_height)
            self.log.debug(
                "At height %d, considering height %d", current_height, random_height
            )
            if random_height > starting_tip_height:
                # Randomly reorg from this point with some probability (1/4 for
                # tip, 1/5 for tip-1, ...)
                if random.random() < 1.0 / (current_height + 4 - random_height):
                    self.log.debug("Invalidating block at height %d", random_height)
                    self.nodes[3].invalidateblock(
                        self.nodes[3].getblockhash(random_height)
                    )

            # Now generate new blocks until we pass the old tip height
            self.log.debug("Mining longer tip")
            block_hashes = []
            while current_height + 1 > self.nodes[3].getblockcount():
                block_hashes.extend(
                    self.nodes[3].generate(
                        min(10, current_height + 1 - self.nodes[3].getblockcount())
                    )
                )
            self.log.debug("Syncing %d new blocks...", len(block_hashes))
            self.sync_node3blocks(block_hashes)
            utxo_list = self.nodes[3].listunspent()
            self.log.debug("Node3 utxo count: %d", len(utxo_list))

        # Check that the utxo hashes agree with node3
        # Useful side effect: each utxo cache gets flushed here, so that we
        # won't get crashes on shutdown at the end of the test.
        self.verify_utxo_hash()

        # Check the test coverage
        self.log.info(
            "Restarted nodes: %s; chainstate rebuilds: %s; crashes on restart: %d",
            self.restart_counts,
            self.rebuild_counts,
            self.crashed_on_restart,
        )

        # If no nodes were restarted, we didn't test anything.
        assert self.restart_counts != [0, 0, 0]

        # Make sure we tested the case of a node that could not be restarted
        # without a rebuild of its chainstate.
        assert self.crashed_on_restart > 0

        # Warn if any of the nodes escaped restart.
        for i in range(3):
            if self.restart_counts[i] == 0:
                self.log.warning("Node %d never crashed during utxo flush!", i)


if __name__ == "__main__":
    ChainstateWriteCrashTest().main()
