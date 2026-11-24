#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that malformed SPV anchor transactions do not crash defid or cause a restart crash-loop."""

from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import (
    CScript,
    OP_0,
    OP_CHECKSIG,
    OP_DUP,
    OP_EQUALVERIFY,
    OP_HASH160,
    OP_RETURN,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_greater_than_or_equal

REGTEST_ANCHORS_ADDRESS = "n1h1kShnyiw3qRR6MM1FnwShaNVoVwBTnF"
B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def base58_to_pubkey_hash(addr):
    n = 0
    for ch in addr:
        n = n * 58 + B58_ALPHABET.index(ch)
    raw = n.to_bytes(25, "big")
    return raw[1:21]


def build_crafted_btc_tx(anchor_script, op_return_script, nonce):
    tx = CTransaction()
    tx.nVersion = 2
    tx.vin = [CTxIn(COutPoint(nonce, 0), b"", 0xFFFFFFFF)]
    tx.vout = [
        CTxOut(1000, anchor_script),
        CTxOut(0, op_return_script),
    ]
    tx.nLockTime = 0
    return tx.serialize().hex()


class SpvAnchorDosTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-spv=1"]]

    def run_test(self):
        node = self.nodes[0]
        pubkey_hash = base58_to_pubkey_hash(REGTEST_ANCHORS_ADDRESS)
        anchor_script = CScript(
            [OP_DUP, OP_HASH160, pubkey_hash, OP_EQUALVERIFY, OP_CHECKSIG]
        )

        # 1. OP_RETURN OP_0 (empty push -> previously nullptr dereference in memcmp)
        tx_empty_push = build_crafted_btc_tx(
            anchor_script, CScript([OP_RETURN, OP_0]), nonce=1
        )
        node.spv_sendrawtx(tx_empty_push)

        # 2. Short OP_RETURN payload (< 3-byte BtcAnchorMarker -> previously heap OOB read)
        tx_short_marker = build_crafted_btc_tx(
            anchor_script, CScript([OP_RETURN, b"DF"]), nonce=2
        )
        node.spv_sendrawtx(tx_short_marker)

        # 3. BtcAnchorMarker + 2 bytes (previously uncaught std::ios_base::failure on uint32 dataSize)
        tx_truncated_size = build_crafted_btc_tx(
            anchor_script, CScript([OP_RETURN, b"DFA\x01\x02"]), nonce=3
        )
        node.spv_sendrawtx(tx_truncated_size)

        # Node must stay alive and respond to RPCs
        height_before = node.getblockcount()
        assert_greater_than_or_equal(height_before, 0)

        # Node must survive a full restart cleanly
        self.restart_node(0, extra_args=["-spv=1"])
        height_after = self.nodes[0].getblockcount()
        assert_greater_than_or_equal(height_after, height_before)


if __name__ == "__main__":
    SpvAnchorDosTest().main()
