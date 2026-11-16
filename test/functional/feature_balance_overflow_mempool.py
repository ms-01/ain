#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Mempool revalidation at the activation height of the transfer sum overflow.

A transaction whose transfer sum overflows is only rejected from
nBalanceOverflowHeight on. Below that height the historical behaviour is kept
(the overflow error is swallowed and the sum stays truncated), so such a
transaction can be accepted into the mempool.

The mempool evaluates transactions with the height of the *next* block. When
that height reaches the activation height, the rules change and the
transactions that were accepted under the old rules have to be re-evaluated:
the overflowing one is not valid anymore and has to be dropped from the
mempool, a transaction without an overflow has to survive.

Scenarios:
  S1  below the activation height the overflowing transaction is accepted and
      mined with the historical behaviour
  S2  it is dropped from the mempool when the next block reaches the
      activation height
  S3  the revalidation happens even if the connected blocks do not contain
      any transaction of the mempool
  S4  above the activation height the overflowing transaction is rejected on
      submission
  S5  a transaction without an overflow survives the same transition and is
      mined
  S6  a child of the dropped transaction leaves the mempool as well: it
      spends an output of the dropped transaction and can never be mined
      without it
  S7  a child of a surviving transaction stays in the mempool
"""

import struct
from decimal import Decimal

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, wait_until

COIN = 100000000
INT64_MAX = 2**63 - 1

# Amounts used by the attack: 8.00000001 DFI and INT64_MAX - 8 DFI. The sum
# of both exceeds INT64_MAX.
SMALL = 800000001
BIG = INT64_MAX - 800000000

# The amount the source account is funded with and the size of the change.
FUNDING = 100
UTXO_VALUE = 20
CHANGE = 10000

ACTIVATION_HEIGHT = 250


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
            marker = b"\x00\x01"  # segwit marker and flag
            raw += varint(len(marker)) + marker
        else:
            raw += varint(0)
        raw += struct.pack("<I", sequence)
    raw += varint(len(outputs))
    for satoshi, script in outputs:
        raw += struct.pack("<q", satoshi) + varint(len(script)) + script
    raw += struct.pack("<I", locktime)
    return raw


class BalanceOverflowMempoolTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # Activate all softforks at height 1, otherwise utxostoaccount and
        # accounttoaccount are rejected with "called before AMK height".
        self.extra_args = [
            [
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
                "-df25height=-1",
                f"-balanceoverflowheight={ACTIVATION_HEIGHT}",
            ],
            [
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
                "-df25height=-1",
                f"-balanceoverflowheight={ACTIVATION_HEIGHT}",
            ],
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
                    return Decimal(raw.split("@")[0])
        return Decimal(0)

    def utxos_of(self, node, address):
        """All unspent outputs of an address, in a stable order."""
        utxos = [u for u in node.listunspent() if u.get("address") == address]
        utxos.sort(key=lambda u: (u["txid"], u["vout"]))
        return utxos

    def fund_attacker(self, node, utxo_count):
        """Fund an account and return it together with its unspent outputs.

        The account is funded with FUNDING DFI, the unspent outputs carry
        UTXO_VALUE DFI each and are used as the inputs of the crafted
        transactions.
        """
        attacker = node.getnewaddress("", "bech32")
        node.utxostoaccount({attacker: str(FUNDING)})
        for _ in range(utxo_count):
            node.sendtoaddress(attacker, UTXO_VALUE)
        node.generate(1)
        utxos = self.utxos_of(node, attacker)
        assert len(utxos) >= utxo_count, "not enough utxos"
        return attacker, utxos[:utxo_count]

    def recipients(self, node):
        """Two fresh recipient addresses and their scripts.

        The recipients must be fresh for every transaction: a recipient that
        already holds INT64_MAX - 8 DFI would overflow when it is credited
        with the same amount again.
        """
        recv_a = node.getnewaddress("", "bech32")
        recv_b = node.getnewaddress("", "bech32")
        script_a = self.p2wpkh_script(node, recv_a)
        script_b = self.p2wpkh_script(node, recv_b)
        # CAccounts is a std::map<CScript, CBalances>: the iteration follows
        # the script bytes. The smaller script has to carry the smaller
        # amount, otherwise the truncated sum is too big to pass the balance
        # check.
        if script_a <= script_b:
            return script_a, recv_a, script_b, recv_b
        return script_b, recv_b, script_a, recv_a

    def build_transfer(self, node, from_address, to_entries, utxo):
        """Build and sign an AccountToAccount transaction."""
        from_script = self.p2wpkh_script(node, from_address)
        payload = build_dftx_payload(from_script, to_entries)
        op_return = b"\x6a\x4c" + bytes([len(payload)]) + payload

        in_amount = int(round(float(utxo["amount"]) * COIN))
        raw = build_raw_tx(
            [(bytes.fromhex(utxo["txid"]), utxo["vout"], 0xFFFFFFFF)],
            [
                (0, op_return),
                (in_amount - CHANGE, self.p2wpkh_script(node, from_address)),
            ],
        )
        signed = node.signrawtransactionwithwallet(raw.hex())
        assert signed.get("complete"), f"signing failed: {signed}"
        return signed["hex"]

    def build_child(self, node, to_address, parent_txid):
        """Build, sign and submit a transaction that spends the change output
        of another transaction in the mempool.

        The change output of a crafted transaction is the second output, the
        first one carries the metadata.
        """
        utxo = node.gettxout(parent_txid, 1)
        assert utxo is not None, f"the parent {parent_txid} has no change output"
        in_amount = int(round(float(utxo["value"]) * COIN))

        raw = build_raw_tx(
            [(bytes.fromhex(parent_txid), 1, 0xFFFFFFFF)],
            [
                (in_amount - CHANGE, self.p2wpkh_script(node, to_address)),
            ],
        )
        signed = node.signrawtransactionwithwallet(raw.hex())
        assert signed.get("complete"), f"signing failed: {signed}"
        return node.sendrawtransaction(signed["hex"])

    def run_test(self):
        holder, producer = self.nodes

        self.log.info(f"The chain starts at height {holder.getblockcount()}")
        assert (
            holder.getblockcount() < ACTIVATION_HEIGHT
        ), "the chain must start below the activation height"

        # ------------------------------------------------------------------
        # S1: below the activation height the historical behaviour applies.
        # ------------------------------------------------------------------
        self.log.info(
            "S1: the overflowing transaction is accepted and mined below the activation height"
        )

        attacker, utxos = self.fund_attacker(holder, 3)
        small_script, small_addr, big_script, big_addr = self.recipients(holder)

        overflow_tx = self.build_transfer(
            holder,
            attacker,
            [
                (small_script, [(0, SMALL)]),
                (big_script, [(0, BIG)]),
            ],
            utxos[0],
        )
        txid = holder.sendrawtransaction(overflow_tx)
        assert txid, "the transaction must be accepted below the activation height"
        assert txid in holder.getrawmempool()
        holder.generate(1)

        # The historical behaviour debits the source with the truncated sum
        # (8.00000001 DFI) and credits the destinations with their full
        # amounts.
        assert_equal(self.account_balance(holder, small_addr), Decimal(SMALL) / COIN)
        assert_equal(self.account_balance(holder, big_addr), Decimal(BIG) / COIN)

        # The producer node follows the chain of the holder.
        self.sync_blocks()

        # ------------------------------------------------------------------
        # The pending transactions must not reach the producer, otherwise
        # they would be mined before the activation height. Both nodes are
        # disconnected from the network, the blocks are handed over
        # manually.
        # ------------------------------------------------------------------
        self.log.info("Prepare the pending transactions on the holding node")
        holder.setnetworkactive(False)
        producer.setnetworkactive(False)
        wait_until(lambda: holder.getconnectioncount() == 0, timeout=30)
        wait_until(lambda: producer.getconnectioncount() == 0, timeout=30)

        pending_small, _, pending_big, _ = self.recipients(holder)
        pending_overflow = self.build_transfer(
            holder,
            attacker,
            [
                (pending_small, [(0, SMALL)]),
                (pending_big, [(0, BIG)]),
            ],
            utxos[1],
        )
        pending_txid = holder.sendrawtransaction(pending_overflow)
        assert pending_txid in holder.getrawmempool()

        # A child of the pending transaction: it spends the change output of
        # its parent. Without the parent it can never be mined, so it has to
        # leave the mempool together with the parent.
        pending_child_txid = self.build_child(holder, attacker, pending_txid)
        assert pending_child_txid in holder.getrawmempool()

        # A control transaction without an overflow, spending another utxo.
        control_receiver = holder.getnewaddress("", "bech32")
        control_tx = self.build_transfer(
            holder,
            attacker,
            [(self.p2wpkh_script(holder, control_receiver), [(0, COIN)])],
            utxos[2],
        )
        control_txid = holder.sendrawtransaction(control_tx)
        assert control_txid in holder.getrawmempool()

        # A child of the control transaction: it stays in the mempool together
        # with its parent.
        control_child_txid = self.build_child(holder, attacker, control_txid)
        assert control_child_txid in holder.getrawmempool()

        # ------------------------------------------------------------------
        # S2/S3: the revalidation at the activation height.
        # ------------------------------------------------------------------
        self.log.info("Mine up to the activation height - 1 and hand the blocks over")
        while producer.getblockcount() < ACTIVATION_HEIGHT - 1:
            producer.generate(1)
        assert producer.getblockcount() == ACTIVATION_HEIGHT - 1

        for height in range(holder.getblockcount() + 1, ACTIVATION_HEIGHT):
            raw = producer.getblock(producer.getblockhash(height), 0)
            assert holder.submitblock(raw) is None

        assert holder.getblockcount() == ACTIVATION_HEIGHT - 1

        # The next block is at the activation height: the rules change and the
        # transactions have to be re-evaluated. The overflowing transaction is
        # dropped, the control transaction survives. The connected blocks do
        # not contain any transaction of the mempool, so this only works if
        # the rebuild is enforced at the activation height.
        self.log.info("S2/S3: the overflowing transaction is dropped from the mempool")
        assert (
            pending_txid not in holder.getrawmempool()
        ), "the overflowing transaction must be dropped from the mempool"
        self.log.info("S6: the child of the dropped transaction is dropped as well")
        assert (
            pending_child_txid not in holder.getrawmempool()
        ), "the child of the dropped transaction must be dropped from the mempool"
        self.log.info("S5: the transaction without an overflow survives")
        assert (
            control_txid in holder.getrawmempool()
        ), "the control transaction must stay in the mempool"
        self.log.info("S7: the child of the surviving transaction survives as well")
        assert (
            control_child_txid in holder.getrawmempool()
        ), "the child of the surviving transaction must stay in the mempool"

        # ------------------------------------------------------------------
        # S5: the control transaction is mined with the activation block.
        # ------------------------------------------------------------------
        self.log.info("S5: the control transaction is mined")
        holder.generate(1)
        assert holder.getblockcount() == ACTIVATION_HEIGHT
        assert (
            control_txid
            in holder.getblock(holder.getblockhash(holder.getblockcount()))["tx"]
        )
        assert (
            pending_txid
            not in holder.getblock(holder.getblockhash(holder.getblockcount()))["tx"]
        )

        # ------------------------------------------------------------------
        # S4: above the activation height the transaction is rejected right
        # away.
        # ------------------------------------------------------------------
        self.log.info("S4: above the activation height the transaction is rejected")
        attacker2, utxos2 = self.fund_attacker(holder, 1)
        overflow_small, _, overflow_big, _ = self.recipients(holder)
        overflow_tx2 = self.build_transfer(
            holder,
            attacker2,
            [
                (overflow_small, [(0, SMALL)]),
                (overflow_big, [(0, BIG)]),
            ],
            utxos2[0],
        )
        assert_raises_rpc_error(
            -26, "overflow", holder.sendrawtransaction, overflow_tx2
        )


if __name__ == "__main__":
    BalanceOverflowMempoolTest().main()
