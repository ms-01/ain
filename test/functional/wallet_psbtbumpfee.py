#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the psbtbumpfee RPC.

The RPC returns an unsigned PSBT of the replacement transaction instead of
signing and broadcasting it. This makes it usable with wallets that have
disabled private keys (watch-only, descriptor or hardware signer wallets),
where the signing has to be done by an external signer.
"""
from decimal import Decimal

from test_framework.descriptors import descsum_create
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    connect_nodes_bi,
)


class PSBTBumpFeeTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-mintxfee=0.00002", "-walletrbf=1"],
            ["-mintxfee=0.00002", "-walletrbf=1"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        connect_nodes_bi(self.nodes, 0, 1)

    def run_test(self):
        signer_node, watcher_node = self.nodes

        self.log.info("Mining blocks...")
        signer_node.generate(101)
        self.sync_blocks()

        # The watcher wallet only knows the public key of the signer wallet.
        signer_address = signer_node.getnewaddress("", "legacy")
        change_address = signer_node.getnewaddress("", "legacy")

        watcher_node.createwallet(
            wallet_name="watcher", disable_private_keys=True, blank=True
        )
        watcher = watcher_node.get_wallet_rpc("watcher")
        assert signer_node.getwalletinfo()["private_keys_enabled"]
        assert not watcher.getwalletinfo()["private_keys_enabled"]

        requests = []
        for address in (signer_address, change_address):
            pubkey = signer_node.getaddressinfo(address)["pubkey"]
            desc = descsum_create("pkh({})".format(pubkey))
            requests.append({"desc": desc, "timestamp": "now"})
        # The change address is imported as an internal descriptor, so that it is
        # not part of the address book of the wallet and therefore recognized as
        # a change output.
        requests[1]["internal"] = True
        result = watcher.importdescriptors(requests)
        assert_equal(result[0]["success"], True)
        assert_equal(result[1]["success"], True)

        # Fund the address, the watcher wallet sees the coin as a watch-only output.
        signer_node.sendtoaddress(signer_address, 1)
        self.sync_mempools()
        signer_node.generate(1)
        self.sync_blocks()

        unspent = watcher.listunspent()
        assert_equal(len(unspent), 1)
        assert_equal(unspent[0]["address"], signer_address)

        # The watcher wallet creates the transaction that is bumped later on, the
        # signer wallet signs it.
        dest_address = signer_node.getnewaddress()
        psbt = watcher.walletcreatefundedpsbt(
            [], {dest_address: 0.5}, 0, {"changeAddress": change_address}
        )["psbt"]
        psbt_signed = signer_node.walletprocesspsbt(psbt, True)
        final = signer_node.finalizepsbt(psbt_signed["psbt"])
        assert final["complete"]
        original_txid = signer_node.sendrawtransaction(final["hex"])
        assert_equal(len(signer_node.decodepsbt(psbt)["tx"]["vin"]), 1)
        self.sync_mempools()
        assert original_txid in signer_node.getrawmempool()

        # bumpfee is not available for wallets with disabled private keys.
        assert_raises_rpc_error(
            -4,
            "bumpfee is not available with wallets that have private keys disabled. Use psbtbumpfee instead.",
            watcher.bumpfee,
            original_txid,
        )

        # The wallet state was not changed by the failed bumpfee call.
        assert original_txid in watcher_node.getrawmempool()
        assert "replaced_by_txid" not in watcher.gettransaction(original_txid)

        bumped_psbt = watcher.psbtbumpfee(original_txid)
        assert_equal(bumped_psbt["errors"], [])
        assert "psbt" in bumped_psbt
        assert "txid" not in bumped_psbt
        assert_equal(
            bumped_psbt["origfee"],
            -watcher.gettransaction(original_txid)["fee"],
        )
        assert_greater_than(
            bumped_psbt["fee"], -watcher.gettransaction(original_txid)["fee"]
        )

        # The returned PSBT is a valid and unsigned PSBT of the replacement transaction.
        decoded = signer_node.decodepsbt(bumped_psbt["psbt"])
        assert_greater_than(len(decoded["tx"]["vin"]), 0)
        assert "partial_signatures" not in decoded["inputs"][0]
        assert not signer_node.finalizepsbt(bumped_psbt["psbt"])["complete"]

        # The original transaction was neither replaced nor removed from the mempool.
        self.sync_mempools()
        assert original_txid in signer_node.getrawmempool()
        assert original_txid in watcher_node.getrawmempool()

        # The external signer can complete and broadcast the replacement transaction.
        bumped_signed = signer_node.walletprocesspsbt(bumped_psbt["psbt"], True)
        final = signer_node.finalizepsbt(bumped_signed["psbt"])
        assert final["complete"]
        bumped_txid = signer_node.sendrawtransaction(final["hex"])
        self.sync_mempools()
        assert bumped_txid in signer_node.getrawmempool()
        assert original_txid not in signer_node.getrawmempool()

        # A PSBTv2 can be requested as well.
        psbt_v2 = watcher.psbtbumpfee(original_txid, {"psbt_version": 2})
        assert_equal(psbt_v2["errors"], [])
        assert "psbt" in psbt_v2

        # The total fee option is not supported by psbtbumpfee.
        assert_raises_rpc_error(
            -8,
            "totalFee is not supported by psbtbumpfee, use the fee estimation options instead.",
            watcher.psbtbumpfee,
            original_txid,
            {"totalFee": 100000},
        )

        self.log.info("Success")


if __name__ == "__main__":
    PSBTBumpFeeTest().main()
