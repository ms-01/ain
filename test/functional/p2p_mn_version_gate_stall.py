#!/usr/bin/env python3
# Copyright (c) 2026-present The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or copy at http://www.opensource.org/licenses/mit-license.php.
"""Test the operator diagnostic for a chain that is stalled above the masternode version gate.

If the next block of the chain is subject to the masternode version gate
(VERSION_BIT_BALANCE_OVERFLOW) and the chain does not make any progress, the node
cannot advance the chain on its own: a quorum of masternodes running gate-capable
software has to produce the next block. This state is silent by default, the node
therefore logs a warning that names the stalled height, the gate height and the
required version bit.

The warning must only be raised if

* the chain really does not make progress (a node that keeps advancing its tip never
  triggers it), and
* the next block is actually subject to the version gate.

The test covers four nodes:

* node 0: the gate applies to the next block and the chain does not progress, the
  warning has to appear,
* node 1: the gate does not apply to the next block, no warning,
* node 2: the gate applies but the maximum tip age is not reached, no warning,
* node 3: the gate applies and the chain keeps progressing, no warning.
"""

import os
import time

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
)

# The log message of the operator diagnostic.
STALL_MESSAGE = "Masternode version gate stall detected"

# Height of the gate for the nodes 0, 2 and 3. The cached test chain has 200 blocks, so
# the next block (height 201) is subject to the gate.
GATE_HEIGHT = 201

# Maximum time that the test waits for the warning of the node 0.
WAIT_TIMEOUT = 180

# Interval in which the node 3 mines a block to keep the chain progressing.
PROGRESS_INTERVAL = 10

# The clock of a node is advanced by this amount of seconds to simulate a chain that does
# not progress any further.
MOCK_TIME_ADVANCE = 600


class MasternodeVersionGateStallTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 4
        self.extra_args = [
            # Node 0: the gate applies to the next block, the maximum tip age is one
            # second so that a stalled chain is detected quickly.
            [f"-mnversiongateheight={GATE_HEIGHT}", "-maxtipage=1"],
            # Node 1: the gate does not apply to the next block.
            ["-mnversiongateheight=1000", "-maxtipage=1"],
            # Node 2: the gate applies but the chain has to be stale for an hour before
            # the stall is reported.
            [f"-mnversiongateheight={GATE_HEIGHT}", "-maxtipage=3600"],
            # Node 3: the gate applies but the chain keeps progressing.
            [f"-mnversiongateheight={GATE_HEIGHT}", "-maxtipage=1"],
        ]

    def setup_network(self):
        # The nodes must not be connected to each other: every test case works on its own
        # chain and the blocks of the nodes would otherwise be exchanged.
        self.setup_nodes()

    def run_test(self):
        self.test_stall_is_reported()
        self.test_no_warning_without_gate()
        self.test_no_warning_within_max_tip_age()
        self.test_no_warning_while_chain_progresses()

    # Helpers

    def debug_log(self, node):
        log_path = os.path.join(node.datadir, node.chain, "debug.log")
        with open(log_path, encoding="utf-8") as log_file:
            return log_file.read()

    def stall_message(self, node):
        """Return the stall warning of the node or None if it was not logged yet."""
        for line in self.debug_log(node).splitlines():
            if STALL_MESSAGE in line:
                return line
        return None

    def advance_clock(self, node):
        """Advance the clock of the node to simulate a chain that does not progress."""
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        node.setmocktime(tip_time + MOCK_TIME_ADVANCE)
        return tip_time + MOCK_TIME_ADVANCE

    # Test cases

    def test_stall_is_reported(self):
        self.log.info("Test that a stalled chain above the version gate is reported")
        node = self.nodes[0]
        progressing_node = self.nodes[3]

        # The chain of the node is not advanced any further, the next block is subject to
        # the version gate.
        assert_equal(node.getblockcount(), 200)
        assert_equal(
            node.getblockchaininfo()["masternode_version_gate_height"], GATE_HEIGHT
        )
        assert_equal(node.getblockchaininfo()["masternode_version_gate_active"], True)

        # The clock of the nodes 0, 1 and 2 is advanced so that a chain that does not
        # progress is detected. The node 3 keeps its chain progressing.
        self.advance_clock(node)
        self.advance_clock(self.nodes[1])
        self.advance_clock(self.nodes[2])

        message = None
        deadline = time.time() + WAIT_TIMEOUT
        while time.time() < deadline and message is None:
            time.sleep(PROGRESS_INTERVAL)
            # The node 3 keeps its chain progressing while the node 0 is stalled.
            progressing_node.generate(1)
            message = self.stall_message(node)

        assert message is not None, "the version gate stall was not reported"

        # The message has to name the stalled height, the gate height, the required
        # version bit and the fact that the node cannot advance the chain on its own.
        assert "no chain progress" in message
        assert "tip height 200" in message
        assert "next block height 201" in message
        assert f"masternode version gate height {GATE_HEIGHT}" in message
        assert "0x01000000" in message
        assert "quorum of masternodes" in message
        assert "cannot advance the chain on its own" in message

    def test_no_warning_without_gate(self):
        self.log.info("Test that no warning is raised if the gate does not apply")
        node = self.nodes[1]

        assert_equal(node.getblockcount(), 200)
        assert_equal(node.getblockchaininfo()["masternode_version_gate_active"], False)
        assert self.stall_message(node) is None

    def test_no_warning_within_max_tip_age(self):
        self.log.info("Test that no warning is raised before the maximum tip age")
        node = self.nodes[2]

        assert_equal(node.getblockcount(), 200)
        assert_equal(node.getblockchaininfo()["masternode_version_gate_active"], True)
        assert self.stall_message(node) is None

    def test_no_warning_while_chain_progresses(self):
        self.log.info("Test that no warning is raised while the chain progresses")
        node = self.nodes[3]

        # The chain of the node keeps progressing, the node must never report a stall even
        # though the next block is subject to the version gate.
        assert node.getblockcount() > 200
        assert_equal(node.getblockchaininfo()["masternode_version_gate_active"], True)
        node.generate(1)
        assert self.stall_message(node) is None


if __name__ == "__main__":
    MasternodeVersionGateStallTest().main()
