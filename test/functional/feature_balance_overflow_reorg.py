#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Chain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Reorg across the activation height of the transfer sum overflow check.

From consensus.nBalanceOverflowHeight on, a transfer whose amounts sum up to
more than INT64_MAX is a consensus error. Below that height the historical
behaviour is kept: the overflow error is swallowed, the sum stays truncated
and the destinations are credited with their full amounts.

A reorganization has to decide per block which behaviour applies. A block
below the activation height keeps the historical behaviour, even if it is
replayed while the tip is already above the activation height. This test
covers that:

  * a block below the activation height that contains an overflowing transfer
    is accepted (historical behaviour),
  * a competing chain without the overflowing transfer wins the
    reorganization, the balances follow the valid chain,
  * the historical chain wins again afterwards: the block below the
    activation height is replayed while the tip is above the activation
    height and the historical behaviour has to be applied again,
  * a rebuild of the node (-reindex-chainstate and -reindex) reproduces the
    same tip, balances and unspent outputs.

The three nodes are not connected to each other, the blocks are handed over
with submitblock. That makes the competing chains deterministic: the node
under test follows the chain with more work.

The overflowing transaction has to be built raw: ParseFixedPoint() rejects
amounts with more than 18 significant digits, so the exploit amount cannot be
expressed through the RPC interface.
"""

import struct

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, disconnect_nodes, wait_until

COIN = 100000000

# The activation height of the overflow check. The historical block is
# placed one height below it, the competing chain is built above it.
ACTIVATION_HEIGHT = 105
# The last height both chains share before they diverge.
PARENT_HEIGHT = ACTIVATION_HEIGHT - 2

# The amounts of the historical transaction: the sum is INT64_MAX + 1, so
# the sum overflows. The destination with the smaller script has to carry the
# smaller amount, otherwise the truncated sum is too big to pass the balance
# check.
HISTORICAL_AMOUNTS = [1, (1 << 63) - 1]
# The amounts of the valid transaction: 1 DFI for each destination.
VALID_AMOUNTS = [100000000, 100000000]

# The amount the source account is funded with and the value of the UTXO
# that pays the fee of the crafted transactions.
FUNDING = 10
UTXO_VALUE = 20
# The fee of the crafted transactions, paid from the UTXO and not from the
# account balance.
FEE = 10000


def varint(n):
    """Bitcoin variable length integer."""
    if n < 0xFD:
        return bytes([n])
    if n <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


def build_dftx_payload(from_script, to_entries):
    """AccountToAccountMessage: from (CScript) + to (CAccounts)."""
    out = b"DfTx" + bytes([0x42])  # 0x42 == 'B' == AccountToAccount
    out += varint(len(from_script)) + from_script
    out += varint(len(to_entries))
    for script, amounts in to_entries:
        out += varint(len(script)) + script
        out += varint(len(amounts))
        for token_id, amount in amounts:
            out += struct.pack("<I", token_id) + struct.pack("<q", amount)
    return out


def build_raw_tx(inputs, outputs, locktime=0):
    """Serialize a witness transaction (version 4)."""
    raw = struct.pack("<i", 4)
    raw += varint(len(inputs))
    for idx, (txid, vout, sequence) in enumerate(inputs):
        raw += txid[::-1] + struct.pack("<I", vout)
        if idx == 0:
            marker = b"\x00\x01"  # segwit marker + flag
            raw += varint(len(marker)) + marker
        else:
            raw += varint(0)
        raw += struct.pack("<I", sequence)
    raw += varint(len(outputs))
    for satoshi, script in outputs:
        raw += struct.pack("<q", satoshi) + varint(len(script)) + script
    raw += struct.pack("<I", locktime)
    return raw


class BalanceOverflowReorgTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        # Activate all softforks at height 1, otherwise utxostoaccount and
        # accounttoaccount are rejected with "called before AMK height".
        forks = [
            "-amkheight=1",
            "-bayfrontheight=1",
            "-bayfrontmarinaheight=1",
            "-bayfrontgardensheight=1",
            "-clarkequayheight=1",
            "-dakotaheight=1",
            "-dakotacrescentheight=1",
            "-eunosheight=1",
            "-eunospayaheight=1",
            "-fortcanningheight=1",
            "-fortcanningmuseumheight=1",
            "-fortcanningparkheight=1",
            "-fortcanninghillheight=1",
            "-fortcanningroadheight=1",
            "-fortcanningcrunchheight=1",
            "-fortcanningspringheight=1",
            "-fortcanninggreatworldheight=1",
            "-fortcanningepilogueheight=1",
            "-grandcentralheight=1",
            "-grandcentralepilogueheight=1",
            "-metachainheight=1",
            "-df23height=1",
            "-df24height=1",
        ]
        self.extra_args = [
            forks + [f"-balanceoverflowheight={ACTIVATION_HEIGHT}"]
            for _ in range(self.num_nodes)
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def p2wpkh_script(self, node, address):
        """Return a P2WPKH script (CScript) of an address."""
        script = bytes.fromhex(node.getaddressinfo(address)["scriptPubKey"])
        assert len(script) == 22 and script[0] == 0x00 and script[1] == 0x14
        return script

    def account_balance(self, node, address):
        """Account balance (not utxo balance) of an address in DFI."""
        for entry in node.listaccounts():
            owner = entry.get("owner") or {}
            if address in (owner.get("addresses") or []):
                raw = entry.get("amount") or ""
                if raw.endswith("@DFI"):
                    return float(raw.split("@")[0])
        return 0.0

    def submit_chain(self, source, target, first_height, last_height):
        """Hand the blocks of a chain over from one node to another one."""
        for height in range(first_height, last_height + 1):
            raw = source.getblock(source.getblockhash(height), 0)
            result = target.submitblock(raw)
            self.log.info("Submit block height=%s result=%s", height, result)
            assert_equal(result in (None, "inconclusive"), True)

    def build_transfer(self, node, source, scripts, amounts, utxo):
        """Build and sign an AccountToAccount transaction.

        The transaction is built raw: the amount that overflows cannot be
        expressed with the RPC interface.
        """
        from_script = self.p2wpkh_script(node, source)
        payload = build_dftx_payload(
            from_script, list(zip(scripts, [[(0, amount)] for amount in amounts]))
        )
        op_return = b"\x6a\x4c" + bytes([len(payload)]) + payload

        in_amount = int(round(float(utxo["amount"]) * COIN))
        raw = build_raw_tx(
            [(bytes.fromhex(utxo["txid"]), utxo["vout"], 0xFFFFFFFF)],
            [
                (0, op_return),
                (in_amount - FEE, self.p2wpkh_script(node, source)),
            ],
        )
        signed = node.signrawtransactionwithwallet(raw.hex())
        assert signed.get("complete"), f"signing failed: {signed}"
        return signed["hex"]

    def run_test(self):
        producer, alternate, receiver = self.nodes

        # Isolate all nodes; the blocks are exchanged explicitly. The nodes
        # are connected as a chain by default, so both directions have to be
        # disconnected before the first block is mined.
        for i in range(self.num_nodes - 1):
            disconnect_nodes(self.nodes[i], i + 1)
            disconnect_nodes(self.nodes[i + 1], i)
        for node in self.nodes:
            node.setnetworkactive(False)
        wait_until(
            lambda: all(node.getconnectioncount() == 0 for node in self.nodes),
            timeout=30,
        )

        self.log.info("Building the shared chain with the funded accounts")
        source = producer.getnewaddress("", "bech32")
        recipients = [
            producer.getnewaddress("", "bech32"),
            producer.getnewaddress("", "bech32"),
        ]
        producer.generate(PARENT_HEIGHT - 2)
        producer.utxostoaccount({source: f"{FUNDING}@0"})
        producer.generate(1)
        producer.sendtoaddress(source, UTXO_VALUE)
        producer.generate(1)
        assert_equal(producer.getblockcount(), PARENT_HEIGHT)

        self.log.info("Submitting the shared chain to the other two nodes")
        self.submit_chain(producer, receiver, 1, PARENT_HEIGHT)
        self.submit_chain(producer, alternate, 1, PARENT_HEIGHT)
        parent_hash = producer.getbestblockhash()
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), parent_hash)
            assert_equal(self.account_balance(node, source), float(FUNDING))

        # CAccounts is a std::map<CScript, CBalances>: the iteration follows
        # the script bytes, so the recipients are sorted by their script.
        scripts = sorted(
            self.p2wpkh_script(producer, address) for address in recipients
        )
        script_addresses = {
            self.p2wpkh_script(producer, address): address
            for address in recipients
        }
        small_address, large_address = [script_addresses[s] for s in scripts]
        addresses = [source, small_address, large_address]

        utxo = None
        for entry in producer.listunspent():
            if entry.get("address") == source:
                utxo = entry
                break
        assert utxo is not None, "no utxo for the source address"

        # Both transactions spend the same UTXO: they are alternatives, only
        # one of them ends up in a block. The competing chains therefore
        # conflict, and a transaction of the discarded chain is dropped from
        # the mempool again.
        historical_tx = self.build_transfer(
            producer, source, scripts, HISTORICAL_AMOUNTS, utxo
        )
        valid_tx = self.build_transfer(producer, source, scripts, VALID_AMOUNTS, utxo)

        def check_state(historical):
            """The balances and the selected outputs of the current chain."""
            assert_equal(
                self.account_balance(receiver, source),
                9.99999999 if historical else 8.0,
            )
            assert_equal(
                self.account_balance(receiver, small_address),
                0.00000001 if historical else 1.0,
            )
            assert_equal(
                self.account_balance(receiver, large_address),
                92233720368.54775807 if historical else 1.0,
            )
            # The transactions of the discarded chain are dropped again, they
            # conflict with the transaction of the chain that is active.
            assert_equal(receiver.getrawmempool(), [])

        def require_stronger(node):
            assert_equal(
                int(node.getblockchaininfo()["chainwork"], 16)
                > int(receiver.getblockchaininfo()["chainwork"], 16),
                True,
            )

        self.log.info("Fork A: the historical chain with the overflowing transfer")
        txid = producer.sendrawtransaction(historical_tx)
        self.log.info("The overflowing transaction was accepted below the activation height: %s", txid)
        producer.generate(3)
        assert_equal(producer.getblockcount(), ACTIVATION_HEIGHT + 1)
        self.submit_chain(producer, receiver, PARENT_HEIGHT + 1, producer.getblockcount())
        check_state(True)
        self.log.info("The historical block below the activation height was applied")

        self.log.info("Fork B: the competing chain with the valid transfer")
        alternate.sendrawtransaction(valid_tx)
        alternate.generate(4)
        assert_equal(alternate.getblockcount(), ACTIVATION_HEIGHT + 2)
        require_stronger(alternate)
        self.submit_chain(alternate, receiver, PARENT_HEIGHT + 1, alternate.getblockcount())
        check_state(False)
        self.log.info("The reorganization restored the balances of the valid chain")

        self.log.info("Back to fork A: the historical block is replayed above the activation height")
        producer.generate(2)
        assert_equal(producer.getblockcount(), ACTIVATION_HEIGHT + 3)
        require_stronger(producer)
        self.submit_chain(producer, receiver, ACTIVATION_HEIGHT + 2, producer.getblockcount())
        check_state(True)
        self.log.info(
            "The reorganization replayed the historical rule of the block below the activation height"
        )

        def replay_snapshot():
            return {
                "tip": receiver.getbestblockhash(),
                "height": receiver.getblockcount(),
                "chainwork": receiver.getblockchaininfo()["chainwork"],
                "accounts": [receiver.getaccount(addr) for addr in addresses],
                "mempool": sorted(receiver.getrawmempool()),
            }

        # The historical branch is active and contains the historical block
        # below the activation height. A rebuild has to replay it with the
        # rules of its own height, not with the rules of the tip.
        expected_replay = replay_snapshot()
        assert_equal(expected_replay["height"], ACTIVATION_HEIGHT + 3)
        for mode in ["-reindex-chainstate", "-reindex"]:
            self.log.info("Rebuilding the historical branch with %s", mode)
            self.restart_node(
                2, extra_args=self.extra_args[2] + [mode, "-persistmempool=0"]
            )
            receiver.setnetworkactive(False)
            wait_until(lambda: receiver.getconnectioncount() == 0, timeout=30)
            wait_until(
                lambda: receiver.getbestblockhash() == expected_replay["tip"],
                timeout=120,
            )
            check_state(True)
            assert_equal(replay_snapshot(), expected_replay)
            self.log.info("%s preserved the tip and all checked balances", mode)

        self.log.info("Switching to the competing chain once more")
        alternate.generate(2)
        assert_equal(alternate.getblockcount(), ACTIVATION_HEIGHT + 4)
        require_stronger(alternate)
        self.submit_chain(alternate, receiver, ACTIVATION_HEIGHT + 3, alternate.getblockcount())
        check_state(False)

        # The node under test has to match the independently maintained
        # reference chain.
        for address in addresses:
            assert_equal(receiver.getaccount(address), alternate.getaccount(address))
        assert_equal(
            receiver.getblockchaininfo()["chainwork"],
            alternate.getblockchaininfo()["chainwork"],
        )
        self.log.info("Competing chain activation reorganization checks passed")


if __name__ == "__main__":
    BalanceOverflowReorgTest().main()
