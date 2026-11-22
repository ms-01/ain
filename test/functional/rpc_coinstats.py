#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the gettxoutsetinfo RPC and its UTXO set hash types.

Test the following RPCs:
    - gettxoutsetinfo

Tests correspond to code in rpc/blockchain.cpp and node/coinstats.cpp.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)
from test_framework.coinstats import (
    hash_serialized_2,
    muhash,
    utxo_set,
)

# The keys that are returned for every hash type.
COMMON_KEYS = [
    "height",
    "bestblock",
    "transactions",
    "txouts",
    "bogosize",
    "disk_size",
    "total_amount",
]


class CoinStatsTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        node.generate(5)

        self.log.info("Test the default hash type (hash_serialized_2)")
        default = node.gettxoutsetinfo()
        assert_equal(sorted(default.keys()), sorted(COMMON_KEYS + ["hash_serialized_2"]))
        assert_equal(len(default["hash_serialized_2"]), 64)
        assert_equal(len(default["bestblock"]), 64)

        self.log.info("Test that the default hash type is explicitly selectable")
        explicit = node.gettxoutsetinfo("hash_serialized_2")
        assert_equal(explicit, default)

        self.log.info("Test the muhash hash type")
        muhash_res = node.gettxoutsetinfo("muhash")
        assert_equal(sorted(muhash_res.keys()), sorted(COMMON_KEYS + ["muhash"]))
        assert_equal(len(muhash_res["muhash"]), 64)
        for key in COMMON_KEYS:
            assert_equal(muhash_res[key], default[key])

        self.log.info("Test the none hash type")
        none_res = node.gettxoutsetinfo("none")
        assert_equal(sorted(none_res.keys()), sorted(COMMON_KEYS))
        for key in COMMON_KEYS:
            assert_equal(none_res[key], default[key])

        self.log.info("Test that the hash type can be passed as a named argument")
        assert_equal(node.gettxoutsetinfo(hash_type="muhash"), muhash_res)
        assert_equal(node.gettxoutsetinfo(hash_type="hash_serialized_2"), default)

        self.log.info("Test that an invalid hash type is rejected")
        assert_raises_rpc_error(-8, "Invalid hash_type", node.gettxoutsetinfo, "bogus")

        self.log.info("Test that the reported hashes are stable")
        assert_equal(node.gettxoutsetinfo("muhash"), node.gettxoutsetinfo("muhash"))
        assert_equal(node.gettxoutsetinfo(), node.gettxoutsetinfo())

        self.log.info("Cross check the UTXO set hashes against an independent implementation")
        utxos = utxo_set(node)
        assert_equal(len(utxos), default["txouts"])
        assert_greater_than(len(utxos), 0)
        # The token id is part of the hashed output, all outputs of this test
        # chain are plain DFI outputs.
        assert_equal([utxo["token_id"] for utxo in utxos], [0] * len(utxos))
        assert_equal(muhash(utxos), node.gettxoutsetinfo("muhash")["muhash"])
        assert_equal(hash_serialized_2(default["bestblock"], utxos), default["hash_serialized_2"])

        self.log.info("Test that the hashes change when the UTXO set changes")
        node.generate(1)
        changed = node.gettxoutsetinfo()
        assert changed["hash_serialized_2"] != default["hash_serialized_2"]
        changed_muhash = node.gettxoutsetinfo("muhash")
        assert changed_muhash["muhash"] != muhash_res["muhash"]
        utxos = utxo_set(node)
        assert_equal(muhash(utxos), changed_muhash["muhash"])
        assert_equal(hash_serialized_2(changed["bestblock"], utxos), changed["hash_serialized_2"])


if __name__ == "__main__":
    CoinStatsTest().main()
