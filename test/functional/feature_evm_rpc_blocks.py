#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Functional tests for the block and transaction reference part of the
``eth_*`` RPC surface.

The gRPC service layer (``lib/ain-grpc/src/rpc/eth.rs``) registers 42
``eth_*`` methods.  This test covers the 13 methods that no other
functional test touches:

  * ``eth_getBlockTransactionCountByNumber`` and
    ``eth_getBlockTransactionCountByHash``,
  * ``eth_getUncleCountByBlockNumber``, ``eth_getUncleCountByBlockHash``,
    ``eth_getUncleByBlockNumberAndIndex`` and
    ``eth_getUncleByBlockHashAndIndex``,
  * ``eth_getTransactionByBlockNumberAndIndex`` and
    ``eth_getTransactionByBlockHashAndIndex``,
  * ``eth_newPendingTransactionFilter``,
  * ``eth_getWork``, ``eth_submitWork`` and ``eth_submitHashrate``,
  * ``eth_syncing``.

The expected results are the ones of the node implementation:

  * the block transaction counters return a JSON **number** (``RpcResult<usize>``)
    and not a hex quantity like Ethereum does,
  * DeFiChain has no uncle blocks, therefore the uncle counters are ``0x0``
    and the uncle lookups return ``null``,
  * the mining methods are stubs: ``eth_getWork`` returns three zero hashes,
    ``eth_submitWork`` and ``eth_submitHashrate`` always return ``false``,
  * ``eth_syncing`` returns ``false`` for a node that is in sync.

Only methods that the server really registers are called; ``eth_getProof``
and ``eth_coinbase`` are not implemented by the server.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal
from test_framework.evm_key_pair import EvmKeyPair

# Gas price (10 gwei) and gas limit used for the regular transactions.
GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000

# The hash of a block that does not exist.
UNKNOWN_HASH = "0x" + "11" * 32

# A block hash that is not the hash of a block.
INVALID_HASH = "0x" + "22" * 32


class EVMRpcBlocksTest(DefiTestFramework):
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

        # Mine a transaction so that a block with exactly one transaction
        # exists and can be looked up by its number and its hash.
        self.receipt = self.send_tx(to="0x00000000000000000000000000000000000000ee")
        assert_equal(self.receipt["status"], 1)
        self.block_number = int(self.receipt["blockNumber"])
        self.block_hash = self.to_hex(self.receipt["blockHash"])
        self.txid = self.to_hex(self.receipt["transactionHash"])

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    def to_hex(self, value) -> str:
        """Return the hex representation of a hash of the receipt."""
        if isinstance(value, str):
            return value
        return value.hex()

    def send_tx(self, to=None, data="0x", gas=TX_GAS, value=0):
        """Sign, send and mine a transaction, return its receipt."""
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "gas": gas,
            "gasPrice": GAS_PRICE,
        }
        if to is not None:
            tx["to"] = self.w3.to_checksum_address(to)
        if data and data != "0x":
            tx["data"] = data
        if value:
            tx["value"] = value
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        return self.w3.eth.wait_for_transaction_receipt(txid)

    # ------------------------------------------------------------------
    # scenarios
    # ------------------------------------------------------------------

    def test_syncing(self):
        """``eth_syncing`` reports ``false`` for a node that is in sync."""
        assert_equal(self.nodes[0].eth_syncing(), False)

    def test_block_transaction_count(self):
        """The block transaction counters return the number of transactions.

        The node returns a JSON number (``RpcResult<usize>``) and not a hex
        quantity like Ethereum does.
        """
        count = self.nodes[0].eth_getBlockTransactionCountByNumber(
            hex(self.block_number)
        )
        assert_equal(count, 1)
        # The result is a number and not a hex string.
        assert isinstance(count, int)

        count = self.nodes[0].eth_getBlockTransactionCountByHash(self.block_hash)
        assert_equal(count, 1)
        assert isinstance(count, int)

        # An unknown hash has no transactions.
        count = self.nodes[0].eth_getBlockTransactionCountByHash(UNKNOWN_HASH)
        assert_equal(count, 0)

    def test_uncle_methods(self):
        """DeFiChain has no uncle blocks.

        The uncle counters are zero and the uncle lookups return ``null``.
        ``eth_getUncleByBlockNumberAndIndex`` and
        ``eth_getUncleByBlockHashAndIndex`` are not part of the dispatch list
        of the test framework, they are called on the EVM RPC connection
        directly.
        """
        assert_equal(self.nodes[0].eth_getUncleCountByBlockNumber(), "0x0")
        assert_equal(self.nodes[0].eth_getUncleCountByBlockHash(), "0x0")

        assert_equal(self.nodes[0].evm_rpc.eth_getUncleByBlockNumberAndIndex(), None)
        assert_equal(self.nodes[0].evm_rpc.eth_getUncleByBlockHashAndIndex(), None)

    def test_transaction_by_block_and_index(self):
        """The transactions of a block can be looked up by number and index."""
        tx = self.nodes[0].eth_getTransactionByBlockNumberAndIndex(
            hex(self.block_number), "0x0"
        )
        assert tx is not None
        assert_equal(tx["hash"], self.txid)
        assert_equal(tx["blockNumber"], hex(self.block_number))
        assert_equal(tx["blockHash"], self.block_hash)
        assert_equal(tx["transactionIndex"], "0x0")

        tx = self.nodes[0].eth_getTransactionByBlockHashAndIndex(self.block_hash, "0x0")
        assert tx is not None
        assert_equal(tx["hash"], self.txid)
        assert_equal(tx["blockNumber"], hex(self.block_number))
        assert_equal(tx["blockHash"], self.block_hash)

        # An index that is out of range has no transaction.
        assert_equal(
            self.nodes[0].eth_getTransactionByBlockNumberAndIndex(
                hex(self.block_number), "0x5"
            ),
            None,
        )

        # An unknown block hash has no transaction.
        assert_equal(
            self.nodes[0].eth_getTransactionByBlockHashAndIndex(UNKNOWN_HASH, "0x0"),
            None,
        )

    def test_pending_transaction_filter(self):
        """The pending transaction filter returns an id and no changes."""
        filter_id = self.nodes[0].eth_newPendingTransactionFilter()
        assert filter_id is not None
        # The filter id is a hex quantity.
        assert isinstance(filter_id, str)
        assert filter_id.startswith("0x")

        assert_equal(self.nodes[0].eth_getFilterChanges(filter_id), [])
        assert_equal(self.nodes[0].eth_pendingTransactions(), [])

        assert_equal(self.nodes[0].eth_uninstallFilter(filter_id), True)

    def test_mining_stubs(self):
        """The mining methods are stubs on DeFiChain."""
        work = self.nodes[0].eth_getWork()
        assert_equal(len(work), 3)
        for entry in work:
            assert_equal(entry, "0x" + "00" * 32)

        assert_equal(self.nodes[0].eth_submitWork("0x0", "0x0", "0x0"), False)
        assert_equal(self.nodes[0].eth_submitHashrate("0x0", "0x0"), False)

    def run_test(self):
        self.setup()

        self.test_syncing()
        self.test_block_transaction_count()
        self.test_uncle_methods()
        self.test_transaction_by_block_and_index()
        self.test_pending_transaction_filter()
        self.test_mining_stubs()


if __name__ == "__main__":
    EVMRpcBlocksTest().main()
