#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test DF25 vault-owner authorization for PaybackLoanV2 collateral repayment."""

import calendar
import time

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
    disconnect_nodes,
)

DF25_HEIGHT = 600
SENTINEL_DUSD = "9999999999.99999999@DUSD"


class LoanPaybackCollateralAuthDF25Test(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common_args = [
            "-txnotokens=0",
            "-amkheight=1",
            "-bayfrontheight=1",
            "-eunosheight=1",
            "-fortcanningheight=1",
            "-fortcanningmuseumheight=1",
            "-fortcanninghillheight=1",
            "-fortcanningroadheight=1",
            "-fortcanningspringheight=1",
            "-fortcanningcrunchheight=1",
            "-fortcanninggreatworldheight=1",
            "-fortcanningepilogueheight=1",
            "-jellyfish_regtest=1",
            "-simulatemainnet=1",
            f"-df25height={DF25_HEIGHT}",
        ]
        self.extra_args = [common_args, common_args]

    def setup_chain_state(self):
        self.nodes[0].generate(120)

        # Appoint and feed two oracles
        price_feeds = [
            {"currency": "USD", "token": "DFI"},
            {"currency": "USD", "token": "DUSD"},
        ]
        oracle_prices = [
            {"currency": "USD", "tokenAmount": "1@DUSD"},
            {"currency": "USD", "tokenAmount": "10@DFI"},
        ]

        oracle_addr1 = self.nodes[0].getnewaddress("", "legacy")
        oracle_id1 = self.nodes[0].appointoracle(oracle_addr1, price_feeds, 10)
        self.nodes[0].generate(1)
        self.nodes[0].setoracledata(
            oracle_id1, calendar.timegm(time.gmtime()), oracle_prices
        )

        oracle_addr2 = self.nodes[0].getnewaddress("", "legacy")
        oracle_id2 = self.nodes[0].appointoracle(oracle_addr2, price_feeds, 10)
        self.nodes[0].generate(1)
        self.nodes[0].setoracledata(
            oracle_id2, calendar.timegm(time.gmtime()), oracle_prices
        )
        self.nodes[0].generate(120)

        self.mn_address = self.nodes[0].get_genesis_keys().ownerAuthAddress
        self.owner_addr = self.nodes[0].getnewaddress()
        self.attacker_addr = self.nodes[1].getnewaddress()

        # Create DUSD loan token
        self.nodes[0].setloantoken(
            {
                "symbol": "DUSD",
                "name": "DUSD",
                "fixedIntervalPriceId": "DUSD/USD",
                "mintable": True,
                "interest": 0,
            }
        )
        self.nodes[0].generate(120)

        self.id_dusd = list(self.nodes[0].gettoken("DUSD").keys())[0]

        # Mint DUSD and create DFI account balance
        self.nodes[0].minttokens("100000@DUSD")
        self.nodes[0].generate(1)
        self.nodes[0].utxostoaccount({self.mn_address: "100000@DFI"})
        self.nodes[0].generate(1)

        # Create DFI-DUSD pool pair and add liquidity
        self.nodes[0].createpoolpair(
            {
                "tokenA": "DFI",
                "tokenB": "DUSD",
                "commission": 0,
                "status": True,
                "ownerAddress": self.mn_address,
            }
        )
        self.nodes[0].generate(1)
        self.nodes[0].addpoolliquidity(
            {self.mn_address: ["10000@DFI", "8000@DUSD"]}, self.mn_address
        )
        self.nodes[0].generate(1)

        # Create loan scheme and collateral tokens
        self.nodes[0].createloanscheme(150, 1, "LOAN001")
        self.nodes[0].generate(1)
        self.nodes[0].setcollateraltoken(
            {"token": "DFI", "factor": 1, "fixedIntervalPriceId": "DFI/USD"}
        )
        self.nodes[0].setcollateraltoken(
            {"token": "DUSD", "factor": 1, "fixedIntervalPriceId": "DUSD/USD"}
        )
        self.nodes[0].generate(120)

        # Enable payback with collateral for DUSD
        self.nodes[0].setgov(
            {
                "ATTRIBUTES": {
                    f"v0/token/{self.id_dusd}/loan_payback_collateral": "true"
                }
            }
        )
        self.nodes[0].generate(1)

        # Fund owner (Node 0) and attacker (Node 1) with UTXOs and DFI/DUSD tokens
        self.nodes[0].sendtoaddress(self.owner_addr, 10)
        self.nodes[0].sendtoaddress(self.attacker_addr, 10)
        self.nodes[0].accounttoaccount(
            self.mn_address,
            {
                self.owner_addr: ["5000@DFI", "5000@DUSD"],
                self.attacker_addr: "500@DUSD",
            },
        )
        self.nodes[0].generate(1)

        # Create vault owned by owner_addr on Node 0
        self.vault_id = self.nodes[0].createvault(self.owner_addr, "LOAN001")
        self.nodes[0].generate(1)
        self.nodes[0].deposittovault(self.vault_id, self.owner_addr, "2000@DFI")
        self.nodes[0].deposittovault(self.vault_id, self.owner_addr, "2000@DUSD")
        self.nodes[0].generate(1)
        self.nodes[0].takeloan({"vaultId": self.vault_id, "amounts": "500@DUSD"})
        self.nodes[0].generate(1)
        self.sync_blocks()

    def run_test(self):
        self.setup_chain_state()

        # Ensure we are well below DF25_HEIGHT for pre-activation checks
        height = self.nodes[0].getblockcount()
        assert height < DF25_HEIGHT - 10, f"Expected height < {DF25_HEIGHT - 10}, got {height}"

        # ------------------------------------------------------------------
        # 1. Pre-DF25Height: reproduce vulnerability & verify historical compat
        # ------------------------------------------------------------------
        # Negative control: paybackwithcollateral already enforces owner auth
        assert_raises_rpc_error(
            -5,
            "Incorrect authorization for",
            self.nodes[1].paybackwithcollateral,
            self.vault_id,
        )

        # Pre-DF25: unrelated attacker B can trigger collateral repayment on A's vault
        # via PaybackLoanV2 sentinel amount without A's signature
        vault_before = self.nodes[0].getvault(self.vault_id)
        assert len(vault_before["loanAmounts"]) == 1

        self.nodes[1].paybackloan(
            {
                "vaultId": self.vault_id,
                "from": self.attacker_addr,
                "loans": [{"dToken": "DUSD", "amounts": SENTINEL_DUSD}],
            }
        )
        self.nodes[1].generate(1)
        self.sync_blocks()

        vault_after_pre_df25 = self.nodes[0].getvault(self.vault_id)
        assert_equal(vault_after_pre_df25["loanAmounts"], [])
        assert vault_after_pre_df25["collateralAmounts"] != vault_before["collateralAmounts"]

        # ------------------------------------------------------------------
        # 2. Mempool boundary check across DF25Height
        # ------------------------------------------------------------------
        # Take a fresh DUSD loan on A's vault while still below DF25Height
        self.nodes[0].takeloan({"vaultId": self.vault_id, "amounts": "500@DUSD"})
        self.nodes[0].generate(1)
        self.sync_blocks()

        # Advance to DF25_HEIGHT - 2 so mempool validation (tip + 1 = DF25_HEIGHT - 1)
        # still accepts the unauthorized sentinel tx
        blocks_to_mine = (DF25_HEIGHT - 2) - self.nodes[0].getblockcount()
        assert blocks_to_mine > 0
        self.nodes[0].generate(blocks_to_mine)
        self.sync_blocks()
        assert_equal(self.nodes[0].getblockcount(), DF25_HEIGHT - 2)

        # Disconnect nodes so Node 1 queues the unauthorized sentinel tx in its mempool
        # while Node 0 advances past DF25_HEIGHT
        disconnect_nodes(self.nodes[0], 1)

        mempool_txid = self.nodes[1].paybackloan(
            {
                "vaultId": self.vault_id,
                "from": self.attacker_addr,
                "loans": [{"dToken": "DUSD", "amounts": SENTINEL_DUSD}],
            }
        )
        assert mempool_txid in self.nodes[1].getrawmempool()

        # Node 0 crosses DF25_HEIGHT
        self.nodes[0].generate(2)
        assert_equal(self.nodes[0].getblockcount(), DF25_HEIGHT)

        # Reconnect and sync: Node 1 crosses DF25_HEIGHT and must not mine the
        # unauthorized sentinel tx in the next block
        vault_before_mine = self.nodes[0].getvault(self.vault_id)
        connect_nodes_bi(self.nodes, 0, 1)
        self.sync_blocks()

        mined_hashes = self.nodes[1].generate(1)
        self.sync_blocks()
        mined_block = self.nodes[1].getblock(mined_hashes[0])
        assert mempool_txid not in mined_block["tx"]
        vault_after_mine = self.nodes[0].getvault(self.vault_id)
        assert_equal(
            vault_after_mine["collateralAmounts"],
            vault_before_mine["collateralAmounts"],
        )
        assert_equal(len(vault_after_mine["loanAmounts"]), 1)

        # ------------------------------------------------------------------
        # 3. Post-DF25Height: exploit prevention (both loans V2 and amounts V1)
        # ------------------------------------------------------------------
        assert self.nodes[0].getblockcount() >= DF25_HEIGHT
        vault_snapshot = self.nodes[0].getvault(self.vault_id)

        # PaybackLoanV2 (loans parameter) must be rejected
        assert_raises_rpc_error(
            -32600,
            "tx must have at least one input from token owner",
            self.nodes[1].paybackloan,
            {
                "vaultId": self.vault_id,
                "from": self.attacker_addr,
                "loans": [{"dToken": "DUSD", "amounts": SENTINEL_DUSD}],
            },
        )

        # Legacy PaybackLoan (amounts parameter) must also be rejected
        assert_raises_rpc_error(
            -32600,
            "tx must have at least one input from token owner",
            self.nodes[1].paybackloan,
            {
                "vaultId": self.vault_id,
                "from": self.attacker_addr,
                "amounts": SENTINEL_DUSD,
            },
        )

        # Negative control: paybackwithcollateral remains rejected
        assert_raises_rpc_error(
            -5,
            "Incorrect authorization for",
            self.nodes[1].paybackwithcollateral,
            self.vault_id,
        )

        # Verify vault collateral and loan balances are completely unchanged
        vault_after_rejects = self.nodes[0].getvault(self.vault_id)
        assert_equal(
            vault_after_rejects["collateralAmounts"],
            vault_snapshot["collateralAmounts"],
        )
        assert_equal(
            vault_after_rejects["loanAmounts"],
            vault_snapshot["loanAmounts"],
        )

        # ------------------------------------------------------------------
        # 4. Positive controls (post-DF25Height): no regressions
        # ------------------------------------------------------------------
        # Third-party normal (non-sentinel) paybackloan still succeeds
        self.nodes[1].paybackloan(
            {
                "vaultId": self.vault_id,
                "from": self.attacker_addr,
                "amounts": "10@DUSD",
            }
        )
        self.nodes[1].generate(1)
        self.sync_blocks()
        vault_after_normal = self.nodes[0].getvault(self.vault_id)
        assert vault_after_normal["loanAmounts"] != vault_snapshot["loanAmounts"]
        assert len(vault_after_normal["loanAmounts"]) == 1

        # Vault owner A can still execute sentinel PaybackLoanV2 collateral repayment
        self.nodes[0].paybackloan(
            {
                "vaultId": self.vault_id,
                "from": self.owner_addr,
                "loans": [{"dToken": "DUSD", "amounts": SENTINEL_DUSD}],
            }
        )
        self.nodes[0].generate(1)
        self.sync_blocks()
        assert_equal(self.nodes[0].getvault(self.vault_id)["loanAmounts"], [])

        # Vault owner A can still execute paybackwithcollateral directly
        self.nodes[0].takeloan({"vaultId": self.vault_id, "amounts": "100@DUSD"})
        self.nodes[0].generate(1)
        self.nodes[0].paybackwithcollateral(self.vault_id)
        self.nodes[0].generate(1)
        self.sync_blocks()
        assert_equal(self.nodes[0].getvault(self.vault_id)["loanAmounts"], [])


if __name__ == "__main__":
    LoanPaybackCollateralAuthDF25Test().main()
