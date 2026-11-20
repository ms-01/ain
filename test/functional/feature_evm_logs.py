#!/usr/bin/env python3
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test EVM contract"""

import asyncio
import json
import websockets

from test_framework.authproxy import JSONRPCException
from test_framework.util import (
    assert_equal,
    int_to_eth_u256,
    rpc_port,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.evm_contract import EVMContract
from test_framework.evm_key_pair import EvmKeyPair
from web3._utils.events import get_event_data


class EVMTestLogs(DefiTestFramework):
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

        # Generate chain
        self.nodes[0].generate(105)

        self.nodes[0].getbalance()
        self.nodes[0].utxostoaccount({self.address: "201@DFI"})
        self.nodes[0].setgov(
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
        self.nodes[0].generate(2)

    def should_create_contract(self):
        node = self.nodes[0]
        self.evm_key_pair = EvmKeyPair.from_node(node)

        node.transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "50@DFI", "domain": 2},
                    "dst": {
                        "address": self.evm_key_pair.address,
                        "amount": "50@DFI",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        node.generate(1)

        self.contract = EVMContract.from_file("Events.sol", "TestEvents")
        abi, bytecode, _ = self.contract.compile()
        compiled = node.w3.eth.contract(abi=abi, bytecode=bytecode)
        self.abi = abi
        self.event_abi = compiled._find_matching_event_abi("NumberStored")

        tx = compiled.constructor().build_transaction(
            {
                "chainId": node.w3.eth.chain_id,
                "nonce": node.w3.eth.get_transaction_count(self.evm_key_pair.address),
                "maxFeePerGas": 10_000_000_000,
                "maxPriorityFeePerGas": 1_500_000_000,
                "gas": 1_000_000,
            }
        )
        signed = node.w3.eth.account.sign_transaction(tx, self.evm_key_pair.privkey)
        hash = node.w3.eth.send_raw_transaction(signed.rawTransaction)

        node.generate(1)

        receipt = node.w3.eth.wait_for_transaction_receipt(hash)
        self.contract = node.w3.eth.contract(
            address=receipt["contractAddress"], abi=abi
        )

    def should_contract_store_and_emit_logs(self):
        # store
        node = self.nodes[0]
        tx = self.contract.functions.store(10).build_transaction(
            {
                "chainId": node.w3.eth.chain_id,
                "from": self.evm_key_pair.address,
                "nonce": node.w3.eth.get_transaction_count(self.evm_key_pair.address),
                "gasPrice": 10_000_000_000,
            }
        )
        signed = node.w3.eth.account.sign_transaction(tx, self.evm_key_pair.privkey)
        hash = node.w3.eth.send_raw_transaction(signed.rawTransaction)

        node.generate(1)

        node.w3.eth.wait_for_transaction_receipt(hash)

        # The `NumberStored` event carries three topics: the event signature, the
        # stored number and the caller.
        self.log_topics = [
            topic if isinstance(topic, str) else "0x" + bytes(topic).hex()
            for topic in node.w3.eth.wait_for_transaction_receipt(hash)["logs"][0][
                "topics"
            ]
        ]
        assert_equal(len(self.log_topics), 3)

        block = self.nodes[0].eth_getBlockByNumber("latest")
        receipt = self.nodes[0].eth_getTransactionReceipt(block["transactions"][0])
        assert_equal(len(receipt["logs"]), 1)

        logs = self.nodes[0].eth_getLogs({"fromBlock": "latest"})
        assert_equal(logs[0]["blockHash"], block["hash"])
        assert_equal(logs[0]["blockNumber"], block["number"])
        assert_equal(logs[0]["logIndex"], "0x0")
        assert_equal(logs[0]["removed"], False)
        assert_equal(logs[0]["transactionHash"], block["transactions"][0])
        assert_equal(logs[0]["transactionIndex"], "0x0")
        assert_equal(logs[0]["data"], "0x")
        assert_equal(len(logs[0]["topics"]), 3)

        logs = node.w3.eth.get_logs({"fromBlock": "latest"})
        event_data = get_event_data(node.w3.codec, self.event_abi, logs[0])
        assert_equal(event_data["event"], "NumberStored")

    def transferdomain_receipt_gas_used(self):
        dvmHash = self.nodes[0].transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "50@DFI", "domain": 2},
                    "dst": {
                        "address": self.evm_key_pair.address,
                        "amount": "50@DFI",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        self.nodes[0].generate(1)
        evmHash = self.nodes[0].vmmap(dvmHash, 5)["output"]
        receipt = self.nodes[0].eth_getTransactionReceipt(evmHash)
        assert_equal(receipt["gasUsed"], int_to_eth_u256(0))

    def should_emit_removed_logs_on_reorg(self):
        node = self.nodes[0]
        ws_url = f"ws://127.0.0.1:{rpc_port(node.index, 4)}"

        async def run_ws_reorg_check():
            async with websockets.connect(ws_url) as ws:
                await ws.send(
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": 1,
                            "method": "eth_subscribe",
                            "params": ["logs", {"address": self.contract.address}],
                        }
                    )
                )
                sub_resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                sub_id = sub_resp["result"]

                tx = self.contract.functions.store(20).build_transaction(
                    {
                        "chainId": node.w3.eth.chain_id,
                        "from": self.evm_key_pair.address,
                        "nonce": node.w3.eth.get_transaction_count(
                            self.evm_key_pair.address
                        ),
                        "gasPrice": 10_000_000_000,
                    }
                )
                signed = node.w3.eth.account.sign_transaction(
                    tx, self.evm_key_pair.privkey
                )
                node.w3.eth.send_raw_transaction(signed.rawTransaction)

                node.generate(1)
                dvm_block_hash = node.getbestblockhash()
                evm_block = node.eth_getBlockByNumber("latest")

                added_msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                assert_equal(added_msg["method"], "eth_subscription")
                assert_equal(added_msg["params"]["subscription"], sub_id)
                added_log = added_msg["params"]["result"]
                assert_equal(added_log["removed"], False)
                assert_equal(added_log["blockHash"], evm_block["hash"])
                assert_equal(added_log["blockNumber"], evm_block["number"])
                assert_equal(added_log["transactionHash"], evm_block["transactions"][0])

                # Trigger block disconnect / reorg; should emit log with removed=True
                node.invalidateblock(dvm_block_hash)

                removed_msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                assert_equal(removed_msg["method"], "eth_subscription")
                assert_equal(removed_msg["params"]["subscription"], sub_id)
                removed_log = removed_msg["params"]["result"]
                assert_equal(removed_log["removed"], True)
                assert_equal(removed_log["blockHash"], evm_block["hash"])
                assert_equal(removed_log["blockNumber"], evm_block["number"])
                assert_equal(
                    removed_log["transactionHash"], evm_block["transactions"][0]
                )
                assert_equal(removed_log["logIndex"], "0x0")

        asyncio.run(run_ws_reorg_check())

    def subscribe_logs(self, topics, timeout=5):
        """Subscribe to the logs of the contract and emit a new log.

        Returns the delivered log or `None` if no log was delivered within `timeout`
        seconds.
        """
        node = self.nodes[0]
        ws_url = f"ws://127.0.0.1:{rpc_port(node.index, 4)}"

        async def run_ws_check():
            async with websockets.connect(ws_url) as ws:
                await ws.send(
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": 1,
                            "method": "eth_subscribe",
                            "params": [
                                "logs",
                                {"address": self.contract.address, "topics": topics},
                            ],
                        }
                    )
                )
                sub_resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                sub_id = sub_resp["result"]

                tx = self.contract.functions.store(10).build_transaction(
                    {
                        "chainId": node.w3.eth.chain_id,
                        "from": self.evm_key_pair.address,
                        "nonce": node.w3.eth.get_transaction_count(
                            self.evm_key_pair.address
                        ),
                        "gasPrice": 10_000_000_000,
                    }
                )
                signed = node.w3.eth.account.sign_transaction(
                    tx, self.evm_key_pair.privkey
                )
                node.w3.eth.send_raw_transaction(signed.rawTransaction)

                node.generate(1)

                try:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout))
                except asyncio.TimeoutError:
                    return None

                assert_equal(msg["method"], "eth_subscription")
                assert_equal(msg["params"]["subscription"], sub_id)
                return msg["params"]["result"]

        return asyncio.run(run_ws_check())

    def should_subscribe_logs_with_null_topic_placeholder(self):
        # A `null` placeholder in the flat form is a positional wildcard: the
        # positions of the following topics must not be shifted.
        log = self.subscribe_logs([self.log_topics[0], None, self.log_topics[2]])
        assert log is not None, "log with a null topic placeholder was not delivered"
        assert_equal(log["topics"], self.log_topics)

        # A `null` inside an OR-list turns the whole position into a wildcard.
        log = self.subscribe_logs([[self.log_topics[1], None]])
        assert (
            log is not None
        ), "log with a nested null topic placeholder was not delivered"
        assert_equal(log["topics"], self.log_topics)

        # A position that does not match must not deliver any log.
        wrong_topic = "0x" + "ff" * 32
        log = self.subscribe_logs([self.log_topics[0], None, wrong_topic])
        assert log is None, "log was delivered although a topic position does not match"

    def subscribe_logs_response(self, topics=None, req_id=1):
        """Send an `eth_subscribe` request for the logs of the contract.

        Returns the JSON-RPC response of the subscription request, i.e. either the
        subscription id (`result`) or the error object (`error`).
        """
        node = self.nodes[0]
        ws_url = f"ws://127.0.0.1:{rpc_port(node.index, 4)}"

        async def run_ws_request():
            async with websockets.connect(ws_url) as ws:
                params = {"address": self.contract.address}
                if topics is not None:
                    params["topics"] = topics
                await ws.send(
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "method": "eth_subscribe",
                            "params": ["logs", params],
                        }
                    )
                )
                return json.loads(await asyncio.wait_for(ws.recv(), timeout=10))

        return asyncio.run(run_ws_request())

    def should_reject_log_subscription_with_too_many_topics(self):
        node = self.nodes[0]
        five_topics = [
            self.log_topics[0],
            self.log_topics[1],
            self.log_topics[2],
            "0x" + "ff" * 32,
            "0x" + "ee" * 32,
        ]

        # The reference error of the `eth_newFilter` surface for the same payload.
        # An explicit block range keeps the comparison independent of the block
        # range limit of the filter service.
        try:
            node.evm_rpc.eth_newFilter(
                {"topics": five_topics, "fromBlock": "0x0", "toBlock": "0x1"}
            )
            raise AssertionError(
                "eth_newFilter accepted more than four topic positions"
            )
        except JSONRPCException as e:
            new_filter_error = e.error

        assert_equal(new_filter_error["code"], -32001)
        assert_equal(
            new_filter_error["message"],
            'Custom error: error calling EVM : "exceed max topics"',
        )

        # The flat form with five positions is rejected with the very same error.
        response = self.subscribe_logs_response(five_topics)
        assert "error" in response, f"subscription was accepted: {response}"
        assert "result" not in response, f"subscription id was handed out: {response}"
        assert_equal(response["error"]["code"], new_filter_error["code"])
        assert_equal(response["error"]["message"], new_filter_error["message"])

        # The nested form with five positions is rejected as well.
        response = self.subscribe_logs_response([[t] for t in five_topics])
        assert "error" in response, f"subscription was accepted: {response}"
        assert "result" not in response, f"subscription id was handed out: {response}"
        assert_equal(response["error"]["code"], new_filter_error["code"])
        assert_equal(response["error"]["message"], new_filter_error["message"])

        # Exactly four positions are still accepted.
        response = self.subscribe_logs_response(five_topics[:4])
        assert "error" not in response, f"subscription was rejected: {response}"
        assert "result" in response, f"no subscription id was returned: {response}"

        # A subscription without any topic filter is still accepted.
        response = self.subscribe_logs_response()
        assert "error" not in response, f"subscription was rejected: {response}"
        assert "result" in response, f"no subscription id was returned: {response}"

    def run_test(self):
        self.setup()

        self.should_create_contract()

        self.should_contract_store_and_emit_logs()

        self.transferdomain_receipt_gas_used()

        self.should_subscribe_logs_with_null_topic_placeholder()

        self.should_reject_log_subscription_with_too_many_topics()

        self.should_emit_removed_logs_on_reorg()


if __name__ == "__main__":
    EVMTestLogs().main()
