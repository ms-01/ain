#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the token_split precompile with an invalid token contract address."""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class EVMTokenSplitInvalidAddressTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-dummypos=0",
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=51",
                "-dakotaheight=51",
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
            ],
        ]

    def setup(self):
        self.address = self.nodes[0].get_genesis_keys().ownerAuthAddress

        self.evm_address = self.nodes[0].getnewaddress("", "erc55")
        # Move to the metachain height and activate the EVM
        self.nodes[0].generate(105)
        self.nodes[0].utxostoaccount({self.address: "201@DFI"})
        self.nodes[0].setgov(
            {
                "ATTRIBUTES": {
                    "v0/params/feature/evm": "true",
                    "v0/params/feature/transferdomain": "true",
                    "v0/transferdomain/dvm-evm/enabled": "true",
                    "v0/transferdomain/dvm-evm/src-formats": ["p2pkh", "bech32"],
                    "v0/transferdomain/dvm-evm/dest-formats": ["erc55"],
                }
            },
        )
        self.nodes[0].generate(2)
        # Move some DFI into the EVM domain, so that an EVM block is committed
        self.nodes[0].transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "1@DFI", "domain": 2},
                    "dst": {
                        "address": self.evm_address,
                        "amount": "1@DFI",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        self.nodes[0].generate(1)

    def address_word(self, address):
        # ABI encodes an address as a 32 byte word
        assert_equal(len(address), 42)
        return "0" * 24 + address[2:]

    def amount_word(self, amount):
        return format(amount, "064x")

    def split_input(self, sender, token_contract, amount):
        # migrateTokensOnAddress(address sender, address tokenContract, uint256 amount)
        # The function selector is not part of the precompile input.
        return (
            "0x"
            + self.address_word(sender)
            + self.address_word(token_contract)
            + self.amount_word(amount)
        )

    def run_test(self):
        self.setup()

        self.test_eth_call_with_invalid_token_contract()

    def test_eth_call_with_invalid_token_contract(self):
        # The precompile is only reachable for callers with a DST20 shaped address
        # (0xff followed by 17 zero bytes).
        caller = "0xff" + "00" * 18 + "ff"
        # A token contract address below the base address (0xdf00...00)
        token_contract = "0x" + "00" * 19 + "01"
        sender = "0x" + "00" * 19 + "02"

        height_before = self.nodes[0].getblockcount()
        eth_height_before = int(self.nodes[0].eth_blockNumber(), 16)

        params = {
            "from": caller,
            "to": "0x000000000000000000000000000000000000000a",
            "data": self.split_input(sender, token_contract, 0),
            "gas": "0x100000",
        }

        # The call must fail with a defined error and must not crash the node
        assert_raises_rpc_error(
            -32001,
            "Invalid token contract address",
            self.nodes[0].eth_call,
            params,
        )

        # The node is still alive and serving requests
        assert_equal(self.nodes[0].getblockcount(), height_before)
        assert_equal(int(self.nodes[0].eth_blockNumber(), 16), eth_height_before)


if __name__ == "__main__":
    EVMTokenSplitInvalidAddressTest().main()
