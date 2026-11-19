#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that -fastprune can store blocks which are larger than the reduced
maximum block file size.

The -fastprune debug option (regtest only) reduces the maximum size of a block
file to 64 KiB, so that the pruning code can be exercised without writing
gigabytes of block data. A single block may be larger than that limit, in which
case the maximum block file size has to be adapted for that block (as upstream
Bitcoin Core does). Without that adaptation the node loops endlessly while
looking for a block file which fits and finally dies with an out-of-memory
abort.
"""

import os

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# Number of outputs of the large transaction. Each output adds roughly 50 bytes
# to the serialized transaction, so 2500 outputs result in a block which is
# clearly larger than the 64 KiB block file limit of -fastprune.
NUM_OUTPUTS = 2500

# The maximum size of a block file in -fastprune mode.
MAX_BLOCK_FILE_SIZE = 0x10000

# An extended public key which is only used to derive the addresses of the
# large transaction. The keys are not needed by the wallet, the outputs of the
# transaction do not have to be spendable.
TPUB = "tpubDAXcJ7s7ZwicqjprRaEWdPoHKrCS215qxGYxpusRLLmJuT69ZSicuGdSfyvyKpvUNYBW1s2U3NSrT6vrCYB9e6nZUEvrqnwXPF8ArTCRXMY"


class FastPruneTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mine blocks to obtain spendable coins")
        node.generate(101)
        assert_equal(node.getblockcount(), 101)

        self.log.info("Derive the addresses of the large transaction")
        descriptor = "wpkh({}/0/*)".format(TPUB)
        checksum = node.getdescriptorinfo(descriptor)["checksum"]
        addresses = node.deriveaddresses(
            "{}#{}".format(descriptor, checksum), [0, NUM_OUTPUTS - 1]
        )
        assert_equal(len(addresses), NUM_OUTPUTS)

        self.log.info(
            "Create a transaction which is larger than the maximum block file size"
        )
        outputs = {address: 0.0001 for address in addresses}
        raw_tx = node.createrawtransaction([], outputs)
        funded = node.fundrawtransaction(raw_tx)
        signed = node.signrawtransactionwithwallet(funded["hex"])
        assert signed["complete"], "the transaction was not signed completely"
        txid = node.sendrawtransaction(signed["hex"])
        assert txid in node.getrawmempool()

        self.log.info("Mine the block which contains the oversized transaction")
        # Before the fix this call never returns, the node stops answering and
        # finally dies with "Error: Out of memory. Terminating.".
        node.generate(1)
        assert_equal(node.getblockcount(), 102)

        self.log.info(
            "Check that the node is still alive and serves the oversized block"
        )
        tip = node.getbestblockhash()
        assert_equal(node.getblockcount(), 102)
        block = node.getblock(tip)
        assert_greater_than(block["size"], MAX_BLOCK_FILE_SIZE)
        assert txid in block["tx"]
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        self.log.info("Check that the block file with the oversized block exists")
        blocks_dir = os.path.join(self.nodes[0].datadir, self.chain, "blocks")
        block_file_sizes = [
            os.path.getsize(os.path.join(blocks_dir, f))
            for f in os.listdir(blocks_dir)
            if f.startswith("blk") and f.endswith(".dat")
        ]
        assert len(block_file_sizes) > 0
        # The block file which contains the oversized block is larger than the
        # 64 KiB limit of -fastprune, the limit is adapted dynamically.
        assert_greater_than(max(block_file_sizes), MAX_BLOCK_FILE_SIZE)

        self.log.info("Done")


if __name__ == "__main__":
    FastPruneTest().main()
