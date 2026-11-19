#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Reject transfers whose amounts do not fit into the maximum amount.

The sum of the transfer amounts is the basis of the account consensus: the
source account is debited with the sum while every destination is credited
with its full amount. An amount that exceeds the maximum has to be rejected,
on the RPC level as well - otherwise the caller gets a transaction with an
amount that is not the one it asked for.

The amounts of a single recipient are aggregated before the transfer is
built. The aggregation has to report an overflow too: without the check the
sum would silently keep the amount of the first entry and the caller would
transfer the truncated amount.

Scenarios:

  S1  utxostoaccount with several recipients whose amounts sum up to more
      than the maximum is rejected
  S2  accounttoaccount with the same amounts is rejected
  S3  the amounts of a single recipient are aggregated first: an array that
      sums up to more than the maximum is rejected as well
  S4  a negative amount is rejected by the amount parser
  S5  a valid transfer still succeeds

No account may change in any of the rejected cases.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# A single amount that the amount parser still accepts (18 significant
# digits), but that is far above the maximum money of the chain.
BIG = "9000000000.00000000@0"
# Eleven of them exceed the maximum amount of a single token.
OVERFLOWING = [BIG] * 12


class BalanceOverflowRpcTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-txnotokens=0", "-amkheight=50", "-eunosheight=101"],
            ["-txnotokens=0", "-amkheight=50", "-eunosheight=101"],
        ]

    def snapshot(self, addresses):
        return [
            {
                "tip": node.getbestblockhash(),
                "mempool": sorted(node.getrawmempool()),
                "accounts": [node.getaccount(addr) for addr in addresses],
            }
            for node in self.nodes
        ]

    def run_test(self):
        node = self.nodes[0]
        node.generate(101)
        self.sync_blocks()

        source = node.getnewaddress()
        destination = node.getnewaddress()
        recipients = [node.getnewaddress() for _ in range(12)]
        addresses = [source, destination] + recipients

        node.utxostoaccount({source: "10@0"})
        node.generate(1)
        self.sync_blocks()
        assert_equal(node.getaccount(source), ["10.00000000@DFI"])
        assert_equal(node.getrawmempool(), [])

        # S1: the sum of the recipients exceeds the maximum amount
        oversized_total = {address: BIG for address in recipients}
        before = self.snapshot(addresses)
        assert_raises_rpc_error(-8, "overflow", node.utxostoaccount, oversized_total)
        assert_equal(self.snapshot(addresses), before)

        # S2: the same over accounttoaccount
        before = self.snapshot(addresses)
        assert_raises_rpc_error(
            -8, "overflow", node.accounttoaccount, source, oversized_total
        )
        assert_equal(self.snapshot(addresses), before)

        # S3: the amounts of one recipient are aggregated first, the overflow
        # has to be reported by the aggregation and not by the transfer sum
        before = self.snapshot(addresses)
        assert_raises_rpc_error(
            -8, "overflow", node.utxostoaccount, {destination: OVERFLOWING}
        )
        assert_equal(self.snapshot(addresses), before)

        # S4: a negative amount is rejected by the amount parser, that is not
        # the transfer sum
        before = self.snapshot(addresses)
        assert_raises_rpc_error(
            -3,
            "Amount out of range",
            node.accounttoaccount,
            source,
            {destination: "-1@0"},
        )
        assert_equal(self.snapshot(addresses), before)

        # S5: a valid transfer still succeeds
        node.accounttoaccount(source, {destination: "1@0"})
        node.generate(1)
        self.sync_all()
        for peer in self.nodes:
            assert_equal(peer.getaccount(source), ["9.00000000@DFI"])
            assert_equal(peer.getaccount(destination), ["1.00000000@DFI"])


if __name__ == "__main__":
    BalanceOverflowRpcTest().main()
