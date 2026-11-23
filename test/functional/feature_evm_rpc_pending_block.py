#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Functional tests for the ``pending`` block selector of the ``eth_*`` RPCs.

The node used to resolve the ``pending`` block selector to the latest
committed block: the ``BlockNumber::Pending`` arm was missing in the block
resolver of ``lib/ain-grpc/src/rpc/common.rs`` and the catch-all arm fell
back to the latest block. A client that asked for the pending block therefore
received a valid, but wrong, block.

The pending block is now the synthetic block on top of the latest committed
block, it carries the transactions that are currently in the mempool. The
state reads resolve explicitly to the latest committed state, the node cannot
compute a faithful pending state: the mempool transactions are executed on the
C++ side during block production, including the DVM side effects.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.evm_key_pair import EvmKeyPair

# Gas price (10 gwei) and gas limit used for the regular transactions.
GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000

# The address the transactions are sent to.
TO_ADDRESS = "0x00000000000000000000000000000000000000ee"


class EVMRpcPendingBlockTest(DefiTestFramework):
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
                "-evmestimategaserrorratio=0",
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

        self.w3 = self.nodes[0].w3
        self.evm_key_pair = EvmKeyPair.from_node(self.nodes[0])
        self.sender = self.evm_key_pair.address
        self.privkey = self.evm_key_pair.privkey

        self.nodes[0].transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "50@DFI", "domain": 2},
                    "dst": {
                        "address": self.sender,
                        "amount": "50@DFI",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        self.nodes[0].generate(1)

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    def sign_and_send(self):
        """Sign a transaction and send it to the mempool without mining it."""
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "gas": TX_GAS,
            "gasPrice": GAS_PRICE,
            "to": self.w3.to_checksum_address(TO_ADDRESS),
        }
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        return self.w3.eth.send_raw_transaction(signed.rawTransaction).hex()

    # ------------------------------------------------------------------
    # scenarios
    # ------------------------------------------------------------------

    def test_pending_block_is_not_the_latest_block(self):
        """The pending block is the latest block plus one.

        The pending block used to be an alias of the latest block, the client
        received a valid, but wrong, block.
        """
        latest = self.nodes[0].eth_getBlockByNumber("latest", False)
        pending = self.nodes[0].eth_getBlockByNumber("pending", False)

        assert pending is not None
        assert_equal(int(pending["number"], 16), int(latest["number"], 16) + 1)
        assert_equal(pending["parentHash"], latest["hash"])
        assert pending["hash"] != latest["hash"]

        # The pending block carries no transaction as long as the mempool is
        # empty.
        assert_equal(pending["transactions"], [])

    def test_pending_block_carries_the_mempool_transactions(self):
        """A transaction that is not mined is part of the pending block."""
        txid = self.sign_and_send()

        pending = self.nodes[0].eth_getBlockByNumber("pending", False)
        assert_equal(pending["transactions"], [txid])

        # The full transaction objects are available as well.
        pending_full = self.nodes[0].eth_getBlockByNumber("pending", True)
        assert_equal(len(pending_full["transactions"]), 1)
        assert_equal(pending_full["transactions"][0]["hash"], txid)
        assert_equal(
            pending_full["transactions"][0]["from"].lower(), self.sender.lower()
        )

        # The pending transaction is cross checked with the pending
        # transactions of the node.
        pending_transactions = self.nodes[0].eth_pendingTransactions()
        assert_equal(len(pending_transactions), 1)
        assert_equal(pending_transactions[0]["hash"], txid)

        # The pending transaction count is the number of the transactions in the
        # mempool.
        assert_equal(self.nodes[0].eth_getBlockTransactionCountByNumber("pending"), 1)

        # After the transaction is mined it is not pending anymore.
        self.nodes[0].generate(1)
        assert_equal(self.nodes[0].eth_pendingTransactions(), [])
        assert_equal(self.nodes[0].eth_getBlockTransactionCountByNumber("pending"), 0)
        pending = self.nodes[0].eth_getBlockByNumber("pending", False)
        latest = self.nodes[0].eth_getBlockByNumber("latest", False)
        assert_equal(int(pending["number"], 16), int(latest["number"], 16) + 1)
        assert_equal(pending["parentHash"], latest["hash"])
        assert_equal(pending["transactions"], [])

    def test_pending_transaction_count(self):
        """The pending nonce is the committed nonce plus the mempool."""
        committed = self.nodes[0].eth_getTransactionCount(self.sender, "latest")
        assert_equal(
            self.nodes[0].eth_getTransactionCount(self.sender, "pending"),
            committed,
        )

        self.sign_and_send()

        committed = self.nodes[0].eth_getTransactionCount(self.sender, "latest")
        pending = self.nodes[0].eth_getTransactionCount(self.sender, "pending")
        assert_equal(int(pending, 16), int(committed, 16) + 1)
        assert_equal(len(self.nodes[0].eth_pendingTransactions()), 1)

        # The nonce of the transaction that is in the mempool is the nonce of
        # the pending state.
        pending_transactions = self.nodes[0].eth_pendingTransactions()
        assert_equal(len(pending_transactions), 1)
        # The nonce of the transaction in the mempool is the committed nonce.
        assert_equal(pending_transactions[0]["nonce"], committed)

        self.nodes[0].generate(1)

        # The transaction is mined, the pending nonce is the committed nonce
        # again.
        committed = self.nodes[0].eth_getTransactionCount(self.sender, "latest")
        pending = self.nodes[0].eth_getTransactionCount(self.sender, "pending")
        assert_equal(pending, committed)

    def test_pending_ranges_resolve_to_the_latest_block(self):
        """The pending block carries neither logs nor fee history.

        The range selectors resolve explicitly to the latest committed block,
        their result is the one of the ``latest`` selector.
        """
        assert_equal(
            self.nodes[0].eth_getLogs({"fromBlock": "0x0", "toBlock": "pending"}),
            self.nodes[0].eth_getLogs({"fromBlock": "0x0", "toBlock": "latest"}),
        )
        assert_equal(
            self.nodes[0].eth_feeHistory("0x1", "pending", []),
            self.nodes[0].eth_feeHistory("0x1", "latest", []),
        )

    def test_pending_block_cannot_be_traced(self):
        """The synthetic pending block cannot be traced."""
        assert_raises_rpc_error(
            -32001,
            "pending block cannot be traced",
            self.nodes[0].debug_traceBlockByNumber,
            "pending",
        )

    def test_other_selectors_are_unchanged(self):
        """The other block selectors keep their behaviour."""
        latest = self.nodes[0].eth_getBlockByNumber("latest", False)
        block_number = int(latest["number"], 16)

        assert_equal(
            self.nodes[0].eth_getBlockByNumber(hex(block_number), False)["hash"],
            latest["hash"],
        )
        assert_equal(
            self.nodes[0].eth_getBlockByHash(latest["hash"], False)["hash"],
            latest["hash"],
        )
        assert_equal(
            self.nodes[0].eth_getBlockByNumber("earliest", False)["number"], hex(0)
        )
        assert_equal(
            self.nodes[0].eth_getBlockTransactionCountByNumber(hex(block_number)),
            len(latest["transactions"]),
        )

    def run_test(self):
        self.setup()

        self.test_pending_block_is_not_the_latest_block()
        self.test_pending_block_carries_the_mempool_transactions()
        self.test_pending_transaction_count()
        self.test_pending_ranges_resolve_to_the_latest_block()
        self.test_pending_block_cannot_be_traced()
        self.test_other_selectors_are_unchanged()


if __name__ == "__main__":
    EVMRpcPendingBlockTest().main()
