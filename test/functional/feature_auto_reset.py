#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the automatic chainstate reset and mempool discard on node startup (v4.2.6.1).

Scenario:
  * Node is initially run simulating pre-fix v4.2.5 behavior (no gate, legacy version).
  * Node mines blocks across ACTIVATION_HEIGHT up to height 210.
  * A transaction is placed in the mempool so mempool.dat is populated on shutdown.
  * Node is stopped.
  * Node is restarted with v4.2.6.1 rules (-balanceoverflowheight=205, -mnversiongateheight=205, -autoreset=1).
  * On startup, the Auto-Reset hook in ThreadImport detects that the active chain
    contains a legacy block at ACTIVATION_HEIGHT (height 205) without bit 24.
  * Node automatically calls InvalidateBlock on that block, rewinding the active
    chain to ACTIVATION_HEIGHT - 1 (height 204).
  * Node automatically purges the legacy mempool.
  * Node then mines a new block at ACTIVATION_HEIGHT, which now carries bit 24.
  * Node restarts again to verify that the upgraded chain is not rewound.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal

VERSION_BIT_BALANCE_OVERFLOW = 1 << 24
LEGACY_BLOCK_VERSION = 0x20000000  # 536870912
ACTIVATION_HEIGHT = 205


class AutoResetTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # Start node simulating legacy pre-fix v4.2.5 node (gates disabled, blockversion legacy)
        self.extra_args = [[
            "-mnversiongateheight=1000000",
            "-balanceoverflowheight=1000000",
            f"-blockversion={LEGACY_BLOCK_VERSION}",
            "-autoreset=0",
        ]]

    def run_test(self):
        node = self.nodes[0]

        # Initial chain starts at height 200 on regtest
        assert_equal(node.getblockcount(), 200)

        # 1. Mine legacy blocks across ACTIVATION_HEIGHT up to height 210
        # (Blocks 201-210 will have nVersion = 0x20000000 without bit 24)
        node.generate(10)
        assert_equal(node.getblockcount(), 210)

        # Verify block at ACTIVATION_HEIGHT does NOT have bit 24 set
        hash205 = node.getblockhash(ACTIVATION_HEIGHT)
        block205 = node.getblock(hash205)
        assert_equal(block205["version"] & VERSION_BIT_BALANCE_OVERFLOW, 0)

        # 2. Add an unconfirmed transaction to the mempool
        recipient = node.getnewaddress()
        node.sendtoaddress(recipient, 1.0)
        mempool_before = node.getrawmempool()
        assert len(mempool_before) > 0, "Mempool should have unconfirmed transactions"

        # 3. Stop node 0 (persisting mempool.dat)
        self.stop_node(0)

        # 4. Restart node 0 with v4.2.6.1 rules active (-autoreset=1, gate and overflow active at 205)
        self.start_node(0, extra_args=[
            f"-mnversiongateheight={ACTIVATION_HEIGHT}",
            f"-balanceoverflowheight={ACTIVATION_HEIGHT}",
            "-autoreset=1",
        ])

        # 5. Verify that Auto-Reset rewound the active chain to ACTIVATION_HEIGHT - 1 (204)
        current_height = node.getblockcount()
        assert_equal(current_height, ACTIVATION_HEIGHT - 1)

        # 6. Verify that the legacy mempool was discarded
        mempool_after = node.getrawmempool()
        assert_equal(mempool_after, [])

        # 7. Mine a new block at ACTIVATION_HEIGHT with the upgraded node
        hash_new = node.generate(1)[0]
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT)

        # Verify the new block carries bit 24 (VERSION_BIT_BALANCE_OVERFLOW)
        block_new = node.getblock(hash_new)
        assert (block_new["version"] & VERSION_BIT_BALANCE_OVERFLOW) != 0, \
            f"Expected bit 24 in new block version {hex(block_new['version'])}"

        # 8. Verify restart with clean chain does NOT rewind again
        self.stop_node(0)
        self.start_node(0, extra_args=[
            f"-mnversiongateheight={ACTIVATION_HEIGHT}",
            f"-balanceoverflowheight={ACTIVATION_HEIGHT}",
            "-autoreset=1",
        ])
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT)


if __name__ == "__main__":
    AutoResetTest().main()
