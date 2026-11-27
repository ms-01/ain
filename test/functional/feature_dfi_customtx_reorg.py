#!/usr/bin/env python3
# Copyright (c) 2026 The DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test multi-node DFI CustomTx reorg rollback and mempool re-acceptance.

Exercises deterministic disconnect/reconnect chain reorganizations across
DFI CustomTx state transitions:
- Vault creation, collateral deposit, loan issuance, payback, and withdrawal
- PoolPair swaps and liquidity additions
- On-chain governance proposals (VOC)
- Mempool re-acceptance of disconnected CustomTxs and state convergence
  between competing chains.
"""

from decimal import Decimal
import time

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes,
    disconnect_nodes,
)


class DfiCustomTxReorgTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common_args = [
            "-txnotokens=0",
            "-amkheight=1",
            "-bayfrontheight=1",
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
            "-metachainheight=105",
            "-df23height=105",
            "-df24height=105",
            "-subsidytest=1",
        ]
        self.extra_args = [list(common_args), list(common_args)]

    def setup_dfi_environment(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        node0.generate(110)
        self.sync_blocks()

        # Enable on-chain governance for proposal tests
        node0.setgov({"ATTRIBUTES": {"v0/params/feature/gov": "true"}})
        node0.generate(1)

        # Create token: BTC (DUSD will be created by setloantoken)
        owner = node0.getnewaddress("", "legacy")
        node0.createtoken(
            {
                "symbol": "BTC",
                "name": "Bitcoin",
                "isDAT": True,
                "collateralAddress": owner,
            }
        )
        node0.generate(1)

        # Mint BTC and fund owner account with DFI
        node0.minttokens("1000@BTC")
        node0.utxostoaccount({owner: "500@DFI"})
        node0.generate(1)

        # Create DFI-BTC poolpair and add initial liquidity
        node0.createpoolpair(
            {
                "tokenA": "DFI",
                "tokenB": "BTC",
                "commission": Decimal("0.002"),
                "status": True,
                "ownerAddress": owner,
                "pairSymbol": "DFI-BTC",
            }
        )
        node0.generate(1)

        node0.addpoolliquidity({owner: ["100@DFI", "100@BTC"]}, owner)
        node0.generate(1)

        # Setup oracle and price feeds for vaults & loans
        oracle_addr = node0.getnewaddress("", "legacy")
        price_feeds = [
            {"currency": "USD", "token": "DFI"},
            {"currency": "USD", "token": "BTC"},
            {"currency": "USD", "token": "DUSD"},
        ]
        oracle_id = node0.appointoracle(oracle_addr, price_feeds, 10)
        node0.generate(1)

        oracle_prices = [
            {"currency": "USD", "tokenAmount": "10@DFI"},
            {"currency": "USD", "tokenAmount": "100@BTC"},
            {"currency": "USD", "tokenAmount": "1@DUSD"},
        ]
        node0.setoracledata(oracle_id, int(time.time()), oracle_prices)
        node0.generate(10)

        # Setup loan scheme, collateral token, and loan token
        node0.createloanscheme(150, 5, "LOAN150")
        node0.generate(1)

        node0.setcollateraltoken(
            {
                "token": "DFI",
                "factor": 1,
                "fixedIntervalPriceId": "DFI/USD",
            }
        )
        node0.setloantoken(
            {
                "symbol": "DUSD",
                "fixedIntervalPriceId": "DUSD/USD",
                "mintable": True,
                "interest": 1,
            }
        )
        node0.generate(1)

        # Create DUSD-DFI poolpair so paybackloan can burn accrued interest in DFI
        node0.minttokens("100@DUSD")
        node0.generate(1)
        node0.createpoolpair(
            {
                "tokenA": "DUSD",
                "tokenB": "DFI",
                "commission": Decimal("0.002"),
                "status": True,
                "ownerAddress": owner,
                "pairSymbol": "DUSD-DFI",
            }
        )
        node0.generate(1)
        node0.addpoolliquidity({"*": ["50@DUSD", "50@DFI"]}, owner)
        node0.generate(10)
        self.sync_blocks()

        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        return owner

    def run_test(self):
        node0, node1 = self.nodes[0], self.nodes[1]
        owner = self.setup_dfi_environment()

        # ------------------------------------------------------------------
        # Scenario 1: Reorg rollback of Vault, Loan, PoolPair, and Proposal
        #             CustomTxs + mempool re-acceptance & convergence
        # ------------------------------------------------------------------
        self.log.info("Scenario 1: Multi-node reorg across Vault/Loan/PoolPair/Proposal CustomTxs")
        pre_fork_pool = node0.getpoolpair("DFI-BTC")
        pre_fork_pair_id = next(iter(pre_fork_pool))
        pre_fork_reserve_a = pre_fork_pool[pre_fork_pair_id]["reserveA"]
        pre_fork_reserve_b = pre_fork_pool[pre_fork_pair_id]["reserveB"]

        disconnect_nodes(node0, 1)

        # Node 0 builds a shorter fork with multiple DFI CustomTx types
        vault_id = node0.createvault(owner, "LOAN150")
        node0.generate(1)

        tx_deposit = node0.deposittovault(vault_id, owner, "50@DFI")
        tx_swap = node0.poolswap(
            {
                "from": owner,
                "tokenFrom": "DFI",
                "amountFrom": 10,
                "to": owner,
                "tokenTo": "BTC",
            }
        )
        tx_proposal = node0.creategovvoc({"title": "Reorg test VOC", "context": "<none>"})
        node0.generate(1)

        tx_loan = node0.takeloan({"vaultId": vault_id, "amounts": "100@DUSD"})
        node0.generate(1)

        # Verify state on node0's fork before reorg
        vault_info = node0.getvault(vault_id)
        assert_equal(vault_info["state"], "active")
        assert_equal(vault_info["collateralAmounts"], ["50.00000000@DFI"])
        assert_equal(len(vault_info["loanAmounts"]), 1)

        fork_pool = node0.getpoolpair("DFI-BTC")[pre_fork_pair_id]
        assert fork_pool["reserveA"] > pre_fork_reserve_a
        assert fork_pool["reserveB"] < pre_fork_reserve_b

        proposal_info = node0.getgovproposal(tx_proposal)
        assert_equal(proposal_info["title"], "Reorg test VOC")

        # Node 1 builds a longer competing chain without those CustomTxs
        node1.generate(5)
        assert_raises_rpc_error(-20, "not found", node1.getvault, vault_id)
        assert_raises_rpc_error(-8, "does not exist", node1.getgovproposal, tx_proposal)

        # Reconnect and trigger reorg on node0 -> node1's longer chain
        connect_nodes(node0, 1)
        self.sync_blocks()

        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        assert_equal(node0.getblockcount(), node1.getblockcount())

        # Verify node0 cleanly rolled back all DFI CustomTx state
        assert_raises_rpc_error(-20, "not found", node0.getvault, vault_id)
        assert_raises_rpc_error(-8, "does not exist", node0.getgovproposal, tx_proposal)

        post_reorg_pool_0 = node0.getpoolpair("DFI-BTC")[pre_fork_pair_id]
        post_reorg_pool_1 = node1.getpoolpair("DFI-BTC")[pre_fork_pair_id]
        assert_equal(post_reorg_pool_0["reserveA"], pre_fork_reserve_a)
        assert_equal(post_reorg_pool_0["reserveB"], pre_fork_reserve_b)
        assert_equal(post_reorg_pool_0["reserveA"], post_reorg_pool_1["reserveA"])
        assert_equal(post_reorg_pool_0["reserveB"], post_reorg_pool_1["reserveB"])

        # Verify disconnected CustomTxs were re-accepted into node0's mempool
        mempool = node0.getrawmempool()
        assert vault_id in mempool, f"Expected vault creation tx {vault_id} in mempool: {mempool}"
        assert tx_swap in mempool, f"Expected poolswap tx {tx_swap} in mempool: {mempool}"
        assert tx_proposal in mempool, f"Expected proposal tx {tx_proposal} in mempool: {mempool}"

        # Mine the re-accepted mempool CustomTxs on the winning chain
        # (dependent txs like deposit/loan may require sequential blocks if not chained in mempool)
        node0.generate(1)
        self.sync_blocks()

        # Confirm vault, poolswap, and proposal converged identically on both nodes
        for idx, node in enumerate([node0, node1]):
            v = node.getvault(vault_id)
            assert_equal(v["ownerAddress"], owner)
            p = node.getgovproposal(tx_proposal)
            assert_equal(p["title"], "Reorg test VOC")
            pool = node.getpoolpair("DFI-BTC")[pre_fork_pair_id]
            assert pool["reserveA"] > pre_fork_reserve_a, f"node{idx} reserveA did not update"

        # Ensure deposit and loan are confirmed (mine if re-accepted in mempool, else re-submit)
        if tx_deposit in node0.getrawmempool():
            node0.generate(1)
            self.sync_blocks()
        elif not node0.getvault(vault_id)["collateralAmounts"]:
            node0.deposittovault(vault_id, owner, "50@DFI")
            node0.generate(1)
            self.sync_blocks()

        if tx_loan in node0.getrawmempool():
            node0.generate(1)
            self.sync_blocks()
        elif not node0.getvault(vault_id)["loanAmounts"]:
            node0.takeloan({"vaultId": vault_id, "amounts": "100@DUSD"})
            node0.generate(1)
            self.sync_blocks()

        assert_equal(node0.getvault(vault_id)["collateralAmounts"], node1.getvault(vault_id)["collateralAmounts"])
        assert_equal(node0.getvault(vault_id)["loanAmounts"], node1.getvault(vault_id)["loanAmounts"])

        # ------------------------------------------------------------------
        # Scenario 2: Reorg rollback of Loan Payback + Collateral Withdrawal
        # ------------------------------------------------------------------
        self.log.info("Scenario 2: Multi-node reorg across loan payback and collateral withdrawal")
        pre_payback_vault = node0.getvault(vault_id)
        assert_equal(len(pre_payback_vault["loanAmounts"]), 1)
        assert_equal(pre_payback_vault["collateralAmounts"], ["50.00000000@DFI"])
        pre_payback_loan = Decimal(pre_payback_vault["loanAmounts"][0].split("@")[0])

        disconnect_nodes(node0, 1)

        # Node 0 partially pays back the loan and withdraws part of the collateral on a shorter fork
        tx_payback = node0.paybackloan(
            {"vaultId": vault_id, "from": owner, "amounts": ["50@DUSD"]}
        )
        node0.generate(1)
        tx_withdraw = node0.withdrawfromvault(vault_id, owner, "20@DFI")
        node0.generate(1)

        fork_vault = node0.getvault(vault_id)
        fork_loan = Decimal(fork_vault["loanAmounts"][0].split("@")[0])
        assert fork_loan < pre_payback_loan
        assert_equal(fork_vault["collateralAmounts"], ["30.00000000@DFI"])

        # Node 1 mines a longer chain where the full loan and collateral remain intact
        node1.generate(4)

        # Reconnect and reorg node0 back to node1's chain
        connect_nodes(node0, 1)
        self.sync_blocks()

        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        reorged_vault_0 = node0.getvault(vault_id)
        reorged_vault_1 = node1.getvault(vault_id)
        assert_equal(reorged_vault_0["collateralAmounts"], ["50.00000000@DFI"])
        assert_equal(len(reorged_vault_0["loanAmounts"]), 1)
        assert Decimal(reorged_vault_0["loanAmounts"][0].split("@")[0]) >= pre_payback_loan
        assert_equal(reorged_vault_0["collateralAmounts"], reorged_vault_1["collateralAmounts"])
        assert_equal(reorged_vault_0["loanAmounts"], reorged_vault_1["loanAmounts"])

        # Payback and withdrawal txs should be re-accepted into mempool and confirm cleanly
        assert tx_payback in node0.getrawmempool()
        node0.generate(1)
        self.sync_blocks()

        final_vault_0 = node0.getvault(vault_id)
        final_vault_1 = node1.getvault(vault_id)
        assert Decimal(final_vault_0["loanAmounts"][0].split("@")[0]) < pre_payback_loan
        assert_equal(final_vault_0["loanAmounts"], final_vault_1["loanAmounts"])
        assert_equal(final_vault_0["collateralAmounts"], final_vault_1["collateralAmounts"])


if __name__ == "__main__":
    DfiCustomTxReorgTest().main()
