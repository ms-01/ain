#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the gettxspendingprevout RPC and the -txospenderindex.

Test the following RPCs:
    - gettxspendingprevout

Tests correspond to code in rpc/mempool.cpp and index/txospenderindex.cpp.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


def prevout(txid, vout):
    """Build a prevout query dict for use with gettxspendingprevout"""
    return {"txid": txid, "vout": vout}


def unspent_out(txid, vout):
    """Expected result for an available output (not spent)"""
    return {"txid": txid, "vout": vout}


def spent_out(txid, vout, spending_tx_id):
    """Expected result for an output with a known spender"""
    return {"txid": txid, "vout": vout, "spendingtxid": spending_tx_id}


def spent_out_in_block(txid, vout, spending_tx_id, blockhash, spending_tx):
    """Expected result for an output spent in a confirmed block, with full tx data"""
    return {
        "txid": txid,
        "vout": vout,
        "spendingtxid": spending_tx_id,
        "blockhash": blockhash,
        "spendingtx": spending_tx,
    }


class GetTxSpendingPrevoutTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txospenderindex=1"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mine blocks to a wallet address and collect a spendable output")
        address = node.getnewaddress()
        node.generatetoaddress(101, address)
        unspent = node.listunspent()
        assert len(unspent) == 1
        utxo = unspent[0]
        utxo_prevout = prevout(utxo["txid"], utxo["vout"])

        self.log.info("Find the transaction spending an output in the mempool")
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        raw_tx = node.getrawtransaction(txid, True)
        # The wallet spends the (only) available output, so the first input of the
        # transaction refers to the output we are looking for.
        assert_equal(len(raw_tx["vin"]), 1)
        spent_prevout = prevout(raw_tx["vin"][0]["txid"], raw_tx["vin"][0]["vout"])
        assert_equal(spent_prevout, utxo_prevout)

        result = node.gettxspendingprevout([utxo_prevout])
        assert_equal(result, [spent_out(utxo["txid"], utxo["vout"], txid)])

        self.log.info("Return the full spending transaction on request")
        result = node.gettxspendingprevout([utxo_prevout], {"return_spending_tx": True})
        assert_equal(
            result,
            [
                {
                    **spent_out(utxo["txid"], utxo["vout"], txid),
                    "spendingtx": raw_tx["hex"],
                }
            ],
        )

        self.log.info("Find no transaction when the output is unspent")
        new_prevout = prevout(txid, raw_tx["vout"][0]["n"])
        assert_equal(node.gettxspendingprevout([new_prevout]), [unspent_out(txid, 0)])

        self.log.info("Find no transaction for an output that does not exist")
        assert_equal(
            node.gettxspendingprevout([prevout(txid, 42)]), [unspent_out(txid, 42)]
        )

        self.log.info("Mixed spent and unspent outputs")
        result = node.gettxspendingprevout([utxo_prevout, new_prevout])
        assert_equal(
            result,
            [
                spent_out(utxo["txid"], utxo["vout"], txid),
                unspent_out(txid, raw_tx["vout"][0]["n"]),
            ],
        )

        self.log.info("Unknown input fields")
        assert_raises_rpc_error(
            -3,
            "Unexpected key unknown",
            node.gettxspendingprevout,
            [{"txid": txid, "vout": 0, "unknown": 42}],
        )

        self.log.info("Invalid vout provided")
        assert_raises_rpc_error(
            -8,
            "Invalid parameter, vout cannot be negative",
            node.gettxspendingprevout,
            [prevout(txid, -1)],
        )

        self.log.info("Invalid txid provided")
        assert_raises_rpc_error(
            -3,
            "Expected type string for txid, got number",
            node.gettxspendingprevout,
            [{"txid": 42, "vout": 0}],
        )

        self.log.info("Missing outputs")
        assert_raises_rpc_error(
            -8, "Invalid parameter, outputs are missing", node.gettxspendingprevout, []
        )

        self.log.info("Missing vout")
        assert_raises_rpc_error(
            -3, "Missing vout", node.gettxspendingprevout, [{"txid": txid}]
        )

        self.log.info("Missing txid")
        assert_raises_rpc_error(
            -3, "Missing txid", node.gettxspendingprevout, [{"vout": 3}]
        )

        self.log.info("Find the transaction spending an output in a confirmed block")
        node.generatetoaddress(1, address)
        # The spending transaction is not necessarily confirmed in the chain tip
        # (the node may mine more than one block per generatetoaddress call), so
        # the expected block is derived from the chain instead of the tip.
        blockhash = node.gettransaction(txid)["blockhash"]
        result = node.gettxspendingprevout([utxo_prevout], {"return_spending_tx": True})
        assert_equal(
            result,
            [
                spent_out_in_block(
                    utxo["txid"],
                    utxo["vout"],
                    txid,
                    blockhash,
                    raw_tx["hex"],
                )
            ],
        )

        self.log.info("Only search the mempool")
        result = node.gettxspendingprevout(
            [utxo_prevout], {"return_spending_tx": True, "mempool_only": True}
        )
        assert_equal(result, [unspent_out(utxo["txid"], utxo["vout"])])

        self.log.info(
            "Check that the index is updated when a reorg replaces a spending transaction"
        )
        txid_reorg = node.sendtoaddress(node.getnewaddress(), 1)
        raw_reorg_tx = node.getrawtransaction(txid_reorg, True)
        reorg_prevout = prevout(
            raw_reorg_tx["vin"][0]["txid"], raw_reorg_tx["vin"][0]["vout"]
        )
        node.generatetoaddress(1, address)
        reorg_blockhash = node.gettransaction(txid_reorg)["blockhash"]

        # txid_reorg is confirmed and indexed as spending our output
        result = node.gettxspendingprevout(
            [reorg_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out_in_block(
                    reorg_prevout["txid"],
                    reorg_prevout["vout"],
                    txid_reorg,
                    reorg_blockhash,
                    raw_reorg_tx["hex"],
                )
            ],
        )

        # invalidate the block that contains the spending transaction, it returns
        # to the mempool and the index has to be rewound
        node.invalidateblock(reorg_blockhash)
        assert txid_reorg in node.getrawmempool()
        result = node.gettxspendingprevout(
            [reorg_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out(reorg_prevout["txid"], reorg_prevout["vout"], txid_reorg)
                | {"spendingtx": raw_reorg_tx["hex"]}
            ],
        )

        # mine the transaction again, the index has to report the new block
        node.generatetoaddress(1, address)
        reorg_blockhash = node.gettransaction(txid_reorg)["blockhash"]
        result = node.gettxspendingprevout(
            [reorg_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out_in_block(
                    reorg_prevout["txid"],
                    reorg_prevout["vout"],
                    txid_reorg,
                    reorg_blockhash,
                    raw_reorg_tx["hex"],
                )
            ],
        )

        self.log.info(
            "Check that the index is updated when a reorg cancels a spending transaction"
        )
        txid_cancel = node.sendtoaddress(node.getnewaddress(), 1)
        raw_cancel_tx = node.getrawtransaction(txid_cancel, True)
        cancel_prevout = prevout(
            raw_cancel_tx["vin"][0]["txid"], raw_cancel_tx["vin"][0]["vout"]
        )
        node.generatetoaddress(1, address)
        cancel_blockhash = node.gettransaction(txid_cancel)["blockhash"]

        # the spending transaction is confirmed and indexed
        result = node.gettxspendingprevout(
            [cancel_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out_in_block(
                    cancel_prevout["txid"],
                    cancel_prevout["vout"],
                    txid_cancel,
                    cancel_blockhash,
                    raw_cancel_tx["hex"],
                )
            ],
        )

        # invalidate the block, the spending transaction returns to the mempool and
        # the index must not report it as confirmed anymore
        node.invalidateblock(cancel_blockhash)
        assert txid_cancel in node.getrawmempool()
        result = node.gettxspendingprevout(
            [cancel_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out(cancel_prevout["txid"], cancel_prevout["vout"], txid_cancel)
                | {"spendingtx": raw_cancel_tx["hex"]}
            ],
        )

        self.log.info("Check mixed mempool and index result order")
        result = node.gettxspendingprevout([reorg_prevout, cancel_prevout])
        assert_equal(
            result,
            [
                {
                    "txid": reorg_prevout["txid"],
                    "vout": reorg_prevout["vout"],
                    "spendingtxid": txid_reorg,
                    "blockhash": reorg_blockhash,
                },
                spent_out(cancel_prevout["txid"], cancel_prevout["vout"], txid_cancel),
            ],
        )

        self.log.info(
            "Check that the index does not report blocks that left the active chain"
        )
        # Restart the node without a wallet and without a persisted mempool, so
        # that the mempool cannot shadow the index. The spending transaction of
        # the invalidated block is not part of the active chain anymore, so the
        # index must not report it and especially not the hash of the
        # disconnected block.
        self.restart_node(
            0,
            extra_args=[
                "-txospenderindex=1",
                "-disablewallet",
                "-persistmempool=0",
            ],
        )
        node = self.nodes[0]

        result = node.gettxspendingprevout(
            [cancel_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result, [unspent_out(cancel_prevout["txid"], cancel_prevout["vout"])]
        )

        # The spending transaction that was mined again is still part of the
        # active chain and the index reports the block it was confirmed in.
        result = node.gettxspendingprevout(
            [reorg_prevout], {"return_spending_tx": True}
        )
        assert_equal(
            result,
            [
                spent_out_in_block(
                    reorg_prevout["txid"],
                    reorg_prevout["vout"],
                    txid_reorg,
                    reorg_blockhash,
                    raw_reorg_tx["hex"],
                )
            ],
        )
        assert node.getblock(reorg_blockhash)["confirmations"] > 0

        self.log.info("Check the behaviour without the txospenderindex")
        self.restart_node(0, extra_args=[])

        # without the index the default is mempool_only, so a confirmed spend is
        # reported as unspent
        result = self.nodes[0].gettxspendingprevout([reorg_prevout])
        assert_equal(
            result, [unspent_out(reorg_prevout["txid"], reorg_prevout["vout"])]
        )

        # explicitly asking for the index fails, as it is not available
        assert_raises_rpc_error(
            -1,
            "Mempool lacks a relevant spend, and txospenderindex is unavailable.",
            self.nodes[0].gettxspendingprevout,
            [reorg_prevout],
            {"mempool_only": False},
        )


if __name__ == "__main__":
    GetTxSpendingPrevoutTest().main()
