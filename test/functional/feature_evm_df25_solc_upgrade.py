#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test DF25 solc 0.8.28 upgrade with frozen 0.8.20 legacy artifacts and height-gated DST20 facade."""

import json
import os

from test_framework.evm_key_pair import EvmKeyPair
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    get_solc_artifact_path,
)


def load_legacy_bytecode(contract: str) -> str:
    path = os.path.join(
        os.path.dirname(__file__),
        "..",
        "..",
        "lib",
        "ain-contracts",
        "legacy_0_8_20",
        contract,
        "deployed_bytecode.json",
    )
    with open(path, "r", encoding="utf8") as f:
        return json.load(f)["object"]


def load_df25_dst20_bytecode() -> str:
    with open(
        get_solc_artifact_path("dst20", "deployed_bytecode.json"),
        "r",
        encoding="utf8",
    ) as f:
        return json.load(f)["object"]


class EvmDf25SolcUpgradeTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        base_args = [
            "-txordering=2",
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
            "-df23height=115",
            "-df24height=120",
            "-subsidytest=1",
        ]
        # Node 0: Regime 1 (DF25Height > DF24Height > DF23Height, mainnet-like)
        # Node 1: Regime 2 (DF25Height = 1 < DF23Height < DF24Height, regtest-like)
        self.extra_args = [
            base_args + ["-df25height=130"],
            base_args + ["-df25height=1"],
        ]

    def setup_network(self):
        self.setup_nodes()
        # Keep nodes isolated so each tests its own DF25Height regime cleanly

    def _activate_evm(self, node):
        node.generate(105)
        node.setgov(
            {
                "ATTRIBUTES": {
                    "v0/params/feature/evm": "true",
                    "v0/params/feature/transferdomain": "true",
                    "v0/transferdomain/dvm-evm/enabled": "true",
                    "v0/transferdomain/evm-dvm/enabled": "true",
                    "v0/transferdomain/dvm-evm/dat-enabled": "true",
                    "v0/transferdomain/evm-dvm/dat-enabled": "true",
                }
            }
        )
        node.generate(2)
        genesis_block = node.w3.eth.get_block("latest")
        return {
            "stateRoot": node.w3.to_hex(genesis_block["stateRoot"]),
            "transactionsRoot": node.w3.to_hex(genesis_block["transactionsRoot"]),
        }

    def _advance_to(self, node, target_height):
        cur = node.getblockcount()
        if target_height > cur:
            node.generate(target_height - cur)
        blk = node.w3.eth.get_block("latest")
        return {
            "stateRoot": node.w3.to_hex(blk["stateRoot"]),
            "transactionsRoot": node.w3.to_hex(blk["transactionsRoot"]),
        }

    def run_test(self):
        legacy_reserved = load_legacy_bytecode("dfi_reserved")
        legacy_td = load_legacy_bytecode("transfer_domain")
        legacy_dst20 = load_legacy_bytecode("dst20")
        legacy_dst20_v1 = load_legacy_bytecode("dst20_v1")
        legacy_dst20_v2 = load_legacy_bytecode("dst20_v2")
        legacy_dst20_v3 = load_legacy_bytecode("dst20_v3")
        df25_dst20 = load_df25_dst20_bytecode()

        # Sanity: solc 0.8.28 dst20 facade differs from frozen 0.8.20 facade
        assert (
            df25_dst20 != legacy_dst20
        ), "Expected 0.8.28 dst20 bytecode to differ from 0.8.20"

        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # 1. Activate EVM on both regimes and verify fixed-address genesis roots & bytecodes match
        roots0_genesis = self._activate_evm(node0)
        roots1_genesis = self._activate_evm(node1)
        assert_equal(
            roots0_genesis["stateRoot"],
            "0x075da8e0c813cc02020ddeb67dfc8d59a31ddff575f9e6e6624f80b7b59fa85b",
        )
        assert_equal(
            roots0_genesis["transactionsRoot"],
            "0x36307ed7ae2a456d58bfcd575e01607e74c1541e7b1422985dd1129b6296f453",
        )
        assert_equal(
            roots0_genesis["transactionsRoot"], roots1_genesis["transactionsRoot"]
        )

        for n in (node0, node1):
            assert_equal(
                n.w3.to_hex(
                    n.w3.eth.get_code(
                        n.w3.to_checksum_address(
                            "0xdf00000000000000000000000000000000000001"
                        )
                    )
                ),
                legacy_td,
            )
            assert_equal(
                n.w3.to_hex(
                    n.w3.eth.get_code(
                        n.w3.to_checksum_address(
                            "0xdf00000000000000000000000000000000000004"
                        )
                    )
                ),
                legacy_dst20_v1,
            )
            assert_equal(
                n.w3.to_hex(
                    n.w3.eth.get_code(
                        n.w3.to_checksum_address(
                            "0xff00000000000000000000000000000000000001"
                        )
                    )
                ),
                legacy_reserved,
            )

        # 2. Advance through DF23 (115) and DF24 (120) on both regimes:
        #    fixed-address DST20_v2 / DST20_v3 stay frozen 0.8.20 and transactionsRoots match
        roots0_df23 = self._advance_to(node0, 115)
        roots1_df23 = self._advance_to(node1, 115)
        assert_equal(roots0_df23["transactionsRoot"], roots1_df23["transactionsRoot"])

        roots0_df24 = self._advance_to(node0, 120)
        roots1_df24 = self._advance_to(node1, 120)
        assert_equal(roots0_df24["transactionsRoot"], roots1_df24["transactionsRoot"])

        for n in (node0, node1):
            assert_equal(
                n.w3.to_hex(
                    n.w3.eth.get_code(
                        n.w3.to_checksum_address(
                            "0xdf00000000000000000000000000000000000005"
                        )
                    )
                ),
                legacy_dst20_v2,
            )
            assert_equal(
                n.w3.to_hex(
                    n.w3.eth.get_code(
                        n.w3.to_checksum_address(
                            "0xdf00000000000000000000000000000000000006"
                        )
                    )
                ),
                legacy_dst20_v3,
            )

        # 3. Negative control (M5): create identical token at height 121 on node0 (< DF25)
        #    vs node1 (>= DF25). node0 deploys 0.8.20 facade, node1 deploys 0.8.28 facade,
        #    and their stateRoots MUST differ (proving root sensitivity to facade bytecode).
        addr0 = node0.get_genesis_keys().ownerAuthAddress
        addr1 = node1.get_genesis_keys().ownerAuthAddress
        for n, addr in ((node0, addr0), (node1, addr1)):
            n.createtoken(
                {
                    "symbol": "PRE",
                    "name": "Pre DF25 Token",
                    "isDAT": True,
                    "collateralAddress": addr,
                }
            )
            n.generate(1)

        pre_addr = node0.w3.to_checksum_address(
            "0xff00000000000000000000000000000000000001"
        )
        assert_equal(node0.w3.to_hex(node0.w3.eth.get_code(pre_addr)), legacy_dst20)
        assert_equal(node1.w3.to_hex(node1.w3.eth.get_code(pre_addr)), df25_dst20)
        root0_pre = node0.w3.to_hex(node0.w3.eth.get_block("latest")["stateRoot"])
        root1_pre = node1.w3.to_hex(node1.w3.eth.get_block("latest")["stateRoot"])
        assert (
            root0_pre != root1_pre
        ), "Negative control failed: 0.8.20 vs 0.8.28 facade must change stateRoot"

        # 4. On node0 (< DF25Height=130): mint PRE and bridge in & out below DF25Height
        evm_key = EvmKeyPair.from_node(node0)
        node0.minttokens("10@PRE")
        node0.generate(1)
        node0.utxostoaccount({addr0: "10@DFI"})
        node0.generate(1)

        node0.transferdomain(
            [
                {
                    "src": {"address": addr0, "amount": "5@PRE", "domain": 2},
                    "dst": {"address": evm_key.address, "amount": "5@PRE", "domain": 3},
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)

        dst20_abi = open(
            get_solc_artifact_path("dst20_v3", "abi.json"), "r", encoding="utf8"
        ).read()
        pre_contract = node0.w3.eth.contract(address=pre_addr, abi=dst20_abi)
        assert_equal(pre_contract.functions.name().call(), "Pre DF25 Token")
        assert_equal(pre_contract.functions.symbol().call(), "PRE")
        assert_equal(pre_contract.functions.totalSupply().call(), 5 * 10**18)
        assert_equal(
            pre_contract.functions.balanceOf(evm_key.address).call(), 5 * 10**18
        )

        # Bridge PRE back out below DF25Height
        node0.transferdomain(
            [
                {
                    "src": {"address": evm_key.address, "amount": "2@PRE", "domain": 3},
                    "dst": {"address": addr0, "amount": "2@PRE", "domain": 2},
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)
        assert_equal(
            pre_contract.functions.balanceOf(evm_key.address).call(), 3 * 10**18
        )

        # 5. Advance node0 to/above DF25Height (130)
        self._advance_to(node0, 130)

        # 5a. Pre-DF25 token (0.8.20 facade) still bridges in and out cleanly at/above DF25Height
        node0.transferdomain(
            [
                {
                    "src": {"address": addr0, "amount": "2@PRE", "domain": 2},
                    "dst": {"address": evm_key.address, "amount": "2@PRE", "domain": 3},
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)
        assert_equal(
            pre_contract.functions.balanceOf(evm_key.address).call(), 5 * 10**18
        )

        node0.transferdomain(
            [
                {
                    "src": {"address": evm_key.address, "amount": "1@PRE", "domain": 3},
                    "dst": {"address": addr0, "amount": "1@PRE", "domain": 2},
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)
        assert_equal(
            pre_contract.functions.balanceOf(evm_key.address).call(), 4 * 10**18
        )

        # 5b. Create a post-DF25 token (POST): DFIReserved reuse check succeeds (B2),
        #     deploys 0.8.28 facade bytecode, delegates to frozen DST20_v3 (M6 parity),
        #     and bridges in/out cleanly.
        node0.createtoken(
            {
                "symbol": "POST",
                "name": "Post DF25 Token",
                "isDAT": True,
                "collateralAddress": addr0,
            }
        )
        node0.generate(1)

        post_addr = node0.w3.to_checksum_address(
            "0xff00000000000000000000000000000000000002"
        )
        assert_equal(node0.w3.to_hex(node0.w3.eth.get_code(post_addr)), df25_dst20)

        node0.minttokens("10@POST")
        node0.generate(1)
        node0.transferdomain(
            [
                {
                    "src": {"address": addr0, "amount": "6@POST", "domain": 2},
                    "dst": {
                        "address": evm_key.address,
                        "amount": "6@POST",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)

        post_contract = node0.w3.eth.contract(address=post_addr, abi=dst20_abi)
        assert_equal(post_contract.functions.name().call(), "Post DF25 Token")
        assert_equal(post_contract.functions.symbol().call(), "POST")
        assert_equal(post_contract.functions.totalSupply().call(), 6 * 10**18)
        assert_equal(
            post_contract.functions.balanceOf(evm_key.address).call(), 6 * 10**18
        )
        assert_equal(
            post_contract.functions.allowance(
                evm_key.address,
                node0.w3.to_checksum_address(
                    "0xdf00000000000000000000000000000000000001"
                ),
            ).call(),
            0,
        )

        # Bridge POST back out at/above DF25Height
        node0.transferdomain(
            [
                {
                    "src": {
                        "address": evm_key.address,
                        "amount": "4@POST",
                        "domain": 3,
                    },
                    "dst": {"address": addr0, "amount": "4@POST", "domain": 2},
                    "singlekeycheck": False,
                }
            ]
        )
        node0.generate(1)
        assert_equal(
            post_contract.functions.balanceOf(evm_key.address).call(), 2 * 10**18
        )
        assert_equal(post_contract.functions.totalSupply().call(), 2 * 10**18)


if __name__ == "__main__":
    EvmDf25SolcUpgradeTest().main()
