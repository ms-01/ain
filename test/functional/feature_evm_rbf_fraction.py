#!/usr/bin/env python3
# Copyright (c) 2024 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the EVM replace-by-fee (RBF) fee increment with a fractional governance value.

The governance attribute `v0/evm/block/rbf_increment_fee_pct` holds a fraction of 100% scaled by
1e8 (e.g. `0.015` = 1.5% = 1_500_000, `0.5` = 50% = 50_000_000, the default being `COIN / 10` =
10%).

* Below the `DF25Height` activation height the value is truncated to whole percentage points
  (legacy behaviour): 1.5% is applied as 1%.
* From `DF25Height` on the exact fraction is applied: 1.5% is applied as 1.5%.

The test runs two **disconnected** nodes (each with its own chain), so that the legacy and the
exact mode never exchange transactions:

* node 0: regtest default => `DF25Height = 1` => exact mode,
* node 1: `-df25height=-1` => DF25 disabled => legacy mode.

Concrete numbers (gas price 1000 gwei, gas limit 21001, base fee 10 gwei on the mempool path):

* tip fee of the original tx: `21001 * (1000 - 10) gwei = 20_790_990 gwei = 207_909_900 sat`
* legacy threshold (1% truncated): `207_909_900 + 2_079_099 = 209_988_999 sat`
* exact threshold (1.5%): `207_909_900 + 3_118_648 = 211_028_548 sat`
* replacement at 1010 gwei: `210_010_000 sat` (accepted in legacy mode, rejected in exact mode)
* replacement at 1020 gwei: `212_110_100 sat` (accepted in both modes)
"""
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_raises_rpc_error


class EVMRbfFractionTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        args = [
            "-txordering=2",
            "-dummypos=0",
            "-txnotokens=0",
            "-amkheight=50",
            "-bayfrontheight=51",
            "-dakotaheight=50",
            "-eunosheight=80",
            "-fortcanningheight=82",
            "-fortcanninghillheight=84",
            "-fortcanningroadheight=86",
            "-fortcanningcrunchheight=88",
            "-fortcanningspringheight=90",
            "-fortcanninggreatworldheight=94",
            "-fortcanningepilogueheight=96",
            "-grandcentralheight=101",
            "-metachainheight=105",
            "-subsidytest=1",
            "-ethdebug=1",
        ]
        # node 0 runs with the regtest default (DF25Height = 1 => exact mode), node 1 has DF25
        # disabled (-df25height=-1 => legacy mode)
        self.extra_args = [args, args + ["-df25height=-1"]]

        self.eth_address = "0x9b8a4af42140d8a4c153a822f02571a1dd037e89"
        self.eth_address_privkey = (
            "af990cc3ba17e776f7f57fcc59942a82846d75833fa17d2ba59ce6858d886e23"
        )
        self.to_address = "0x6C34CBb9219d8cAa428835D2073E8ec88BA0a110"
        self.to_address_privkey = (
            "17b8cb134958b3d8422b6c43b0732fcdb8c713b524df2d45de12f0c7e214ba35"
        )

    def setup_network(self):
        # Do not connect the nodes: each node runs its own chain, so that the legacy and the
        # exact mode cannot exchange transactions.
        self.setup_nodes()

    def prepare_node(self, node):
        """Import the EVM keys, fund the EVM address and mine some blocks."""
        node.importprivkey(self.eth_address_privkey)
        node.importprivkey(self.to_address_privkey)

        # generate chain and move to the metachain height
        node.generate(101)
        node.generate(4)

        owner = node.get_genesis_keys().ownerAuthAddress
        node.utxostoaccount({owner: "201@DFI"})
        node.setgov(
            {
                "ATTRIBUTES": {
                    "v0/params/feature/evm": "true",
                    "v0/params/feature/transferdomain": "true",
                    "v0/transferdomain/dvm-evm/enabled": "true",
                    "v0/transferdomain/dvm-evm/src-formats": ["p2pkh", "bech32"],
                    "v0/transferdomain/dvm-evm/dest-formats": ["erc55"],
                    "v0/transferdomain/evm-dvm/src-formats": ["erc55"],
                    "v0/transferdomain/evm-dvm/auth-formats": ["bech32-erc55"],
                    "v0/transferdomain/evm-dvm/dest-formats": ["p2pkh", "bech32"],
                }
            }
        )
        node.generate(2)
        node.transferdomain(
            [
                {
                    "src": {"address": owner, "amount": "100@DFI", "domain": 2},
                    "dst": {"address": self.eth_address, "amount": "100@DFI", "domain": 3},
                    "singlekeycheck": False,
                }
            ]
        )
        node.generate(1)

    def set_increment(self, node, value):
        node.setgov({"ATTRIBUTES": {"v0/evm/block/rbf_increment_fee_pct": value}})
        node.generate(1)

    def test_fractional_increment(self, node, exact_mode):
        """A fractional governance value is truncated below DF25 and applied exactly from DF25 on."""
        self.set_increment(node, "0.015")  # 1.5%

        nonce = node.w3.eth.get_transaction_count(self.eth_address)

        # original transaction: 1000 gwei, gas limit 21001
        node.evmtx(self.eth_address, nonce, 1000, 21001, self.to_address, 1)

        # replacement with a 1% higher gas price:
        #  - legacy mode: 1.5% is truncated to 1%, so the replacement is accepted
        #  - exact mode: 1.5% is required, so the replacement is rejected
        if exact_mode:
            assert_raises_rpc_error(
                -26,
                "evm-low-fee",
                node.evmtx,
                self.eth_address,
                nonce,
                1010,
                21001,
                self.to_address,
                1,
            )
        else:
            node.evmtx(self.eth_address, nonce, 1010, 21001, self.to_address, 1)

        # a replacement with a 2% higher gas price is accepted in both modes
        nonce2 = nonce + 1
        node.evmtx(self.eth_address, nonce2, 1000, 21001, self.to_address, 1)
        node.evmtx(self.eth_address, nonce2, 1020, 21001, self.to_address, 1)

    def test_whole_percent_increment(self, node):
        """A whole percentage value behaves identically in both modes."""
        self.set_increment(node, "0.01")  # 1%

        # commit the transactions of the previous scenario, so that the nonce is up to date
        node.generate(1)
        nonce = node.w3.eth.get_transaction_count(self.eth_address)

        # original transaction: 1000 gwei, gas limit 21001
        node.evmtx(self.eth_address, nonce, 1000, 21001, self.to_address, 1)

        # a replacement with a 0.5% higher gas price is rejected in both modes
        assert_raises_rpc_error(
            -26,
            "evm-low-fee",
            node.evmtx,
            self.eth_address,
            nonce,
            1005,
            21001,
            self.to_address,
            1,
        )

        # a replacement with a 1% higher gas price is accepted in both modes
        nonce2 = nonce + 1
        node.evmtx(self.eth_address, nonce2, 1000, 21001, self.to_address, 1)
        node.evmtx(self.eth_address, nonce2, 1010, 21001, self.to_address, 1)

    def run_test(self):
        # node 0: DF25 active from height 1 => the exact fraction is applied
        self.prepare_node(self.nodes[0])
        self.test_fractional_increment(self.nodes[0], exact_mode=True)
        self.test_whole_percent_increment(self.nodes[0])

        # node 1: DF25 disabled => the legacy whole-percent truncation is applied
        self.prepare_node(self.nodes[1])
        self.test_fractional_increment(self.nodes[1], exact_mode=False)
        self.test_whole_percent_increment(self.nodes[1])


if __name__ == "__main__":
    EVMRbfFractionTest().main()
