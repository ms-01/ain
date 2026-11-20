#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that a corrupted mempool.dat does not break the node on startup.

A mempool.dat file can be corrupted or crafted, e.g. by a truncated write, a
bit flip or a hostile file. The node has to start cleanly in that case: it
must log a warning, keep an empty and consistent mempool and must neither
crash nor hang.

Test is as follows:

  - create a transaction on node1 and relay it to node0, so that node0 has a
    mempool.dat file with a transaction that its own wallet does not know
  - replace the acceptance time of that transaction with an extreme value and
    verify that the entry is rejected as implausible
  - stop the node, so that the mempool.dat file can be manipulated
  - for every corrupted variant write the file, start the node and verify
    that the node comes up, logs the expected warning and has an empty
    mempool
"""

import os
import struct

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, wait_until

MEMPOOL_DUMP_VERSION = 1
INT64_MAX = (1 << 63) - 1


class MempoolPersistCorruptTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            ["-persistmempool=1"],
            ["-persistmempool=1"],
        ]

    def mempool_path(self):
        return os.path.join(self.nodes[0].datadir, "regtest", "mempool.dat")

    def debug_log_path(self):
        return os.path.join(self.nodes[0].datadir, "regtest", "debug.log")

    def write_mempool(self, data):
        with open(self.mempool_path(), "wb") as mempool_file:
            mempool_file.write(data)

    def log_offset(self):
        try:
            return os.path.getsize(self.debug_log_path())
        except OSError:
            return 0

    def log_since(self, offset):
        with open(self.debug_log_path(), "rb") as log_file:
            log_file.seek(offset)
            return log_file.read().decode("utf-8", errors="replace")

    def start_with_mempool(self, data, expected_log):
        """Write the given mempool.dat content, start the node and check the log."""
        self.write_mempool(data)
        offset = self.log_offset()
        self.start_node(0)

        node = self.nodes[0]
        # The node has to be fully functional again.
        assert_equal(node.getblockcount(), 200)
        # The corrupted file must not leave any transaction behind.
        assert_equal(len(node.getrawmempool()), 0)

        log = self.log_since(offset)
        assert expected_log in log, f"expected '{expected_log}' in the debug log:\n{log}"

        self.stop_node(0)

    def run_test(self):
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        self.log.debug("Relay a transaction to node0, so that its mempool file is not empty")
        txid = node1.sendtoaddress(node1.getnewaddress(), 1.0)
        wait_until(lambda: txid in node0.getrawmempool(), timeout=30)
        entry_time = node0.getmempoolentry(txid)["time"]

        # Stop node1, so that it does not relay the transaction to node0 again
        # and node0 stays in control of the contents of its mempool.
        self.stop_node(1)
        self.stop_node(0)

        self.log.debug("A mempool file with an extreme acceptance time must be rejected")
        with open(self.mempool_path(), "rb") as mempool_file:
            mempool_data = mempool_file.read()
        # The acceptance time is stored as a signed 64 bit integer.
        pattern = struct.pack("<q", entry_time)
        assert pattern in mempool_data, "the acceptance time was not found in mempool.dat"
        patched = mempool_data.replace(pattern, struct.pack("<q", INT64_MAX), 1)
        self.start_with_mempool(patched, "implausible timestamp")

        self.log.debug("An empty mempool file must not break the startup")
        self.start_with_mempool(b"", "Failed to deserialize mempool data on disk")

        self.log.debug("A mempool file with an unsupported version must be rejected")
        self.start_with_mempool(
            struct.pack("<Q", MEMPOOL_DUMP_VERSION + 1),
            "Mempool file has an unsupported version",
        )

        self.log.debug("A mempool file with garbage content must be rejected")
        self.start_with_mempool(b"\x00" * 64, "Mempool file has an unsupported version")

        self.log.debug("A truncated mempool file must be rejected")
        self.start_with_mempool(
            struct.pack("<Q", MEMPOOL_DUMP_VERSION) + struct.pack("<Q", 1) + b"\x01\x02\x03",
            "Failed to deserialize mempool data on disk",
        )

        self.log.debug("A mempool file with an absurd number of transactions must be rejected")
        self.start_with_mempool(
            struct.pack("<Q", MEMPOOL_DUMP_VERSION) + struct.pack("<Q", (1 << 64) - 1),
            "exceeds the maximum",
        )

        self.log.debug("A mempool file with an absurd number of fee deltas must be rejected")
        self.start_with_mempool(
            struct.pack("<Q", MEMPOOL_DUMP_VERSION)
            + struct.pack("<Q", 0)
            + b"\xfe"
            + struct.pack("<I", 200000),
            "exceeds the maximum",
        )

        self.log.debug("A mempool file with an oversized fee delta count must be rejected")
        self.start_with_mempool(
            struct.pack("<Q", MEMPOOL_DUMP_VERSION)
            + struct.pack("<Q", 0)
            + b"\xfe\xff\xff\xff\xff",
            "Failed to deserialize mempool data on disk",
        )


if __name__ == "__main__":
    MempoolPersistCorruptTest().main()
