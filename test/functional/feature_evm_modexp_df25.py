#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test DF25Height activation gating for EVM modexp even-modulus gas cost.

Before DF25Height, the modexp precompile (0x..05) charges 20x the EIP-2565 gas
cost for even moduli (4000 gas instead of 200 gas for 3^5 mod 8).
At and above DF25Height, it charges the plain EIP-2565 cost (200 gas) for both
odd and even moduli.
"""

from test_framework.evm_key_pair import EvmKeyPair
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal

DF25_HEIGHT = 120
GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000

MODEXP = "0x0000000000000000000000000000000000000005"
PLAIN_ACCOUNT = "0x00000000000000000000000000000000000000ee"


def modexp_input(base: int, exp: int, modulus: int) -> bytes:
    return (
        (1).to_bytes(32, "big")
        + (1).to_bytes(32, "big")
        + (1).to_bytes(32, "big")
        + bytes([base, exp, modulus])
    )


class EvmModexpDF25Test(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-dummypos=0",
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=51",
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
                "-df23height=105",
                "-df24height=105",
                f"-df25height={DF25_HEIGHT}",
                "-subsidytest=1",
            ]
        ]

    def setup(self):
        self.address = self.nodes[0].get_genesis_keys().ownerAuthAddress
        self.eth_address = EvmKeyPair.from_node(self.nodes[0])
        self.w3 = self.nodes[0].w3
        self.sender = self.w3.to_checksum_address(self.eth_address.address)
        self.privkey = self.eth_address.privkey

        self.nodes[0].generate(105)
        self.nodes[0].setgov(
            {
                "ATTRIBUTES": {
                    "v0/params/feature/evm": "true",
                    "v0/params/feature/transferdomain": "true",
                    "v0/transferdomain/dvm-evm/enabled": "true",
                    "v0/transferdomain/dvm-evm/src-formats": ["p2pkh", "bech32"],
                    "v0/transferdomain/dvm-evm/dest-formats": ["erc55"],
                }
            }
        )
        self.nodes[0].generate(2)

        self.nodes[0].utxostoaccount({self.address: "100@DFI"})
        self.nodes[0].generate(1)
        self.nodes[0].transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "50@DFI", "domain": 2},
                    "dst": {"address": self.sender, "amount": "50@DFI", "domain": 3},
                    "singlekeycheck": False,
                }
            ]
        )
        self.nodes[0].generate(1)

        # Touch the plain account so its creation never affects gas deltas.
        self.send_tx(to=PLAIN_ACCOUNT)

    def send_tx(self, to: str, data: str = "0x", gas: int = TX_GAS):
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "to": self.w3.to_checksum_address(to),
            "gas": gas,
            "gasPrice": GAS_PRICE,
        }
        if data and data != "0x":
            tx["data"] = data
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        return self.w3.eth.wait_for_transaction_receipt(txid)

    def direct_delta(self, target: str, data: bytes) -> int:
        hex_data = "0x" + data.hex()
        used_target = self.send_tx(to=target, data=hex_data)["gasUsed"]
        used_plain = self.send_tx(to=PLAIN_ACCOUNT, data=hex_data)["gasUsed"]
        return used_target - used_plain

    def estimate_delta(self, target: str, data: bytes) -> int:
        hex_data = "0x" + data.hex()
        est_target = self.w3.eth.estimate_gas(
            {"from": self.sender, "to": self.w3.to_checksum_address(target), "data": hex_data}
        )
        est_plain = self.w3.eth.estimate_gas(
            {"from": self.sender, "to": self.w3.to_checksum_address(PLAIN_ACCOUNT), "data": hex_data}
        )
        return est_target - est_plain

    def run_test(self):
        self.setup()

        odd_data = modexp_input(3, 5, 7)
        even_data = modexp_input(3, 5, 8)

        # --- Below DF25Height ---
        assert self.nodes[0].getblockcount() < DF25_HEIGHT - 4

        assert_equal(
            bytes(
                self.w3.eth.call(
                    {"from": self.sender, "to": self.w3.to_checksum_address(MODEXP), "data": "0x" + odd_data.hex()}
                )
            ),
            bytes([5]),
        )
        assert_equal(
            bytes(
                self.w3.eth.call(
                    {"from": self.sender, "to": self.w3.to_checksum_address(MODEXP), "data": "0x" + even_data.hex()}
                )
            ),
            bytes([3]),
        )

        # Mined tx gasUsed: odd = 200, even = 4000 (20x penalty)
        assert_equal(self.direct_delta(MODEXP, odd_data), 200)
        assert_equal(self.direct_delta(MODEXP, even_data), 4000)
        # eth_estimateGas matches pre-DF25 gas accounting (binary search margin)
        est_odd_pre = self.estimate_delta(MODEXP, odd_data)
        est_even_pre = self.estimate_delta(MODEXP, even_data)
        assert 200 <= est_odd_pre < 500, f"unexpected pre-DF25 odd estimate delta: {est_odd_pre}"
        assert 4000 <= est_even_pre <= 5000, f"unexpected pre-DF25 even estimate delta: {est_even_pre}"

        # --- Activate DF25Height ---
        blocks_needed = DF25_HEIGHT - self.nodes[0].getblockcount()
        assert blocks_needed > 0
        self.nodes[0].generate(blocks_needed)
        assert self.nodes[0].getblockcount() >= DF25_HEIGHT

        # Mined tx gasUsed: both odd and even = 200 (EIP-2565)
        assert_equal(self.direct_delta(MODEXP, odd_data), 200)
        assert_equal(self.direct_delta(MODEXP, even_data), 200)
        # eth_estimateGas matches post-DF25 gas accounting (binary search margin)
        est_odd_post = self.estimate_delta(MODEXP, odd_data)
        est_even_post = self.estimate_delta(MODEXP, even_data)
        assert 200 <= est_odd_post < 500, f"unexpected post-DF25 odd estimate delta: {est_odd_post}"
        assert 200 <= est_even_post < 500, f"unexpected post-DF25 even estimate delta: {est_even_post}"


if __name__ == "__main__":
    EvmModexpDF25Test().main()
