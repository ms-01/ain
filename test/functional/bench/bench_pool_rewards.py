#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Benchmark of the pool reward catch-up of a stale liquidity provider.

The pool rewards of an account which has not been touched for a long time are
re-calculated when the account is touched the next time. The cost of this
catch-up depends on the number of the missed heights and on the number of the
pools the account holds shares in.

The benchmark measures the `ApplyCustomTx` time of a transfer which touches a
stale liquidity provider. The measurement is taken from the node's own
`-debug=bench` log entry:

    - ApplyCustomTx: <txid> Type: AccountToAccount Time: <x>ms

Usage:

    POOL_REWARD_BENCH_POOLS=10 POOL_REWARD_BENCH_STALE_BLOCKS=1000 \
        python3 test/functional/bench/bench_pool_rewards.py

"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from test_framework.test_framework import DefiTestFramework

BENCH_RE = re.compile(r"ApplyCustomTx: (\w+) Type: (\S+) Time: ([0-9.]+)ms")


class PoolRewardsBenchmark(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=50",
                "-bayfrontgardensheight=50",
                "-clarkequayheight=50",
                "-grandcentralheight=50",
                "-debug=bench",
            ]
        ]

    def debug_log_path(self):
        return "%s/regtest/debug.log" % self.nodes[0].datadir

    def bench_entries(self, offset):
        entries = {}
        with open(self.debug_log_path(), "r", errors="replace") as log:
            log.seek(offset)
            for line in log:
                match = BENCH_RE.search(line)
                if match:
                    entries[match.group(1)] = (match.group(2), float(match.group(3)))
        return entries

    def log_size(self):
        with open(self.debug_log_path(), "rb") as log:
            return log.tell()

    def run_test(self):
        node = self.nodes[0]
        pools = int(os.environ.get("POOL_REWARD_BENCH_POOLS", "10"))
        stale_blocks = int(os.environ.get("POOL_REWARD_BENCH_STALE_BLOCKS", "1000"))

        # leave the initial block download and accumulate enough DFI for the
        # collateral of the tokens
        node.generate(600)

        # the tokens of the pools
        collateral = node.getnewaddress("", "legacy")
        symbols = ["T%03d" % i for i in range(pools)]
        for symbol in symbols:
            node.createtoken(
                {"symbol": symbol, "name": symbol, "collateralAddress": collateral}
            )
            node.generate(1)
        tokens = node.listtokens()
        token_ids = [idx for idx, token in tokens.items() if token["symbol"] in symbols]
        for token_id in token_ids:
            node.minttokens(["1000000@" + token_id])
        node.generate(1)

        # an account with DFI to fund the liquidity providers
        funder = node.getnewaddress("", "legacy")
        node.utxostoaccount({funder: "100@DFI"})
        node.generate(1)

        # the pools and the liquidity provider, which holds shares in all the
        # pools
        provider = node.getnewaddress("", "legacy")
        node.accounttoaccount(funder, {provider: "2@DFI"})
        node.generate(1)
        for token_id in token_ids:
            pool_owner = node.getnewaddress("", "legacy")
            node.createpoolpair(
                {
                    "tokenA": "DFI",
                    "tokenB": token_id,
                    "commission": 0.01,
                    "status": True,
                    "ownerAddress": pool_owner,
                }
            )
            node.generate(1)
            node.accounttoaccount(collateral, {provider: "100@" + token_id})
            node.generate(1)
            node.addpoolliquidity({"*": ["1@DFI", "100@" + token_id]}, provider)
            node.generate(1)

        # NOTE: the pools do not pay any reward, so the liquidity providers are
        # not touched by the reward distribution and their pool rewards have to
        # be re-calculated when they are touched the next time

        # a receiver for the transfers
        receiver = node.getnewaddress("", "legacy")
        node.accounttoaccount(funder, {receiver: "2@DFI"})
        node.generate(1)

        # let the liquidity providers become stale
        node.generate(stale_blocks)

        # the transfer touches the stale liquidity provider and settles its
        # pool rewards
        offset = self.log_size()
        txid = node.accounttoaccount(provider, {receiver: "0.1@DFI"})
        node.generate(1)
        time.sleep(0.5)
        entries = self.bench_entries(offset)
        if txid not in entries:
            raise RuntimeError("no bench entry for %s" % txid)
        print(
            "[pool-rewards] pools=%d stale_blocks=%d ApplyCustomTx=%.3f ms"
            % (pools, stale_blocks, entries[txid][1])
        )


if __name__ == "__main__":
    PoolRewardsBenchmark().main()
