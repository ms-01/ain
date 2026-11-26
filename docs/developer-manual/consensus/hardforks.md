# Hardfork timeline

This page lists every DeFiChain hardfork from `DF1AMKHeight` to `DF25Height`,
with the activation height of each network and the consensus parameter that
carries it.

Part of the [Consensus section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The heights in this page are transcribed from `src/chainparams.cpp` and
    the parameter declarations in `src/consensus/params.h`. When a height in
    the code changes, this page has to change with it.

    Legacy reference: [`doc/hardforks.md`](https://github.com/DeFiCh/ain/blob/master/doc/hardforks.md)
    still lists the user-facing feature summaries of the named releases.

## How an activation height works

* Every hardfork is a plain `int` member of `Consensus::Params`
  (`src/consensus/params.h`), named `DF<n><Name>Height`.
* Every network class in `src/chainparams.cpp` sets **all** of them, so a
  network can never inherit a stale value from another network.
* `std::numeric_limits<int>::max()` means "not scheduled on this network" —
  the rule is never active. It is the default for all not-yet-scheduled
  networks.
* A rule is active when `height >= <Height>`. There is no separate "enabled"
  flag and no on-chain signalling: the height in the chain parameters *is* the
  switch.
* The Rust side reads the same values through the FFI (`getDF23Height()`,
  `getDF24Height()`, `getDF25Height()` in `src/ffi/ffiexports.cpp`), so the
  EVM layer always agrees with the C++ layer about which rules apply.

## Mainnet timeline

| Parameter | Height | Release | Summary |
| --- | --- | --- | --- |
| `DF1AMKHeight` | 356500 | Ang Mo Kio (v1.2.0) | Tokens (DST/DCT/DAT), minting and distribution, Bitcoin anchor rewards, 25% incentive funding. |
| `DF2BayfrontHeight` | 405000 | Bayfront (v1.3.0) | DEX and liquidity pools, token swaps, trading-fee liquidity incentives. |
| `DF3BayfrontMarinaHeight` | 465150 | Bayfront Marina (v1.3.x) | Follow-up hardfork to Bayfront; the release features are not summarised in the legacy `doc/hardforks.md`. |
| `DF4BayfrontGardensHeight` | 488300 | Bayfront Gardens (v1.3.8) | Reward distribution fix for pool shares below 0.01%, poolswap performance, new RPC calls. |
| `DF5ClarkeQuayHeight` | 595738 | Clarke Quay (v1.4.0) | Custom pool-pair rewards, automatic balance selection, no staking while syncing. |
| `DF6DakotaHeight` | 678000 | Dakota (v1.5.0) | Masternode collateral reduced to 20,000 DFI, anchoring system refactor, swap slippage fix, staking performance. |
| `DF7DakotaCrescentHeight` | 733000 | Dakota Crescent (v1.6.0) | Consensus efficiency improvements. |
| `DF8EunosHeight` | 894000 | Eunos (v1.7.0) | Interchain Exchange, new block-reward scheme, foundation burn, price oracles, difficulty adjustment period raised to 1008 blocks, masternode registration/resignation delays. |
| `DF9EunosKampungHeight` | 895743 | Eunos Kampung (v1.7.x) | Follow-up hardfork to Eunos, activated one day later than Eunos on mainnet; the release features are not summarised in the legacy `doc/hardforks.md`. |
| `DF10EunosPayaHeight` | 1072000 | Eunos Paya (v1.8.0) | 5- and 10-year masternode locks, pagination for oracle and price RPCs, subnode staking with consistent locked rewards. |
| `DF11FortCanningHeight` | 1367000 | Fort Canning (v2.0.0) | Decentralised loans (vaults, loans, liquidations and auctions), composite swaps, `spv_refundhtlcall`, anchoring cost reduced to BTC dust, transaction fees returned to block rewards. |
| `DF12FortCanningMuseumHeight` | 1430640 | Fort Canning Museum (v2.1.x) | Follow-up hardfork to Fort Canning; the release features are not summarised in the legacy `doc/hardforks.md`. |
| `DF13FortCanningParkHeight` | 1503143 | Fort Canning Park (v2.1.x) | Follow-up hardfork to Fort Canning Museum; the release features are not summarised in the legacy `doc/hardforks.md`. |
| `DF14FortCanningHillHeight` | 1604999 | Fort Canning Hill (v2.1.x) | Follow-up hardfork to Fort Canning Park; the release features are not summarised in the legacy `doc/hardforks.md`. |
| `DF15FortCanningRoadHeight` | 1786000 | Fort Canning Road (v2.7.0) | Futures contracts, vault payback in any token via swaps, per-pool/per-token DEX fees. |
| `DF16FortCanningCrunchHeight` | 1936000 | Fort Canning Crunch (v2.8.0) | Token splits. |
| `DF17FortCanningSpringHeight` | 2033000 | Fort Canning Spring (v2.9.0) | DFI-DUSD future swaps, liquidation penalties converted to DUSD, DUSD swap fee, `-dexstats`, token split info in account history. |
| `DF18FortCanningGreatWorldHeight` | 2212000 | Fort Canning Great World (v2.10.0) | Negative interest rates. |
| `DF19FortCanningEpilogueHeight` | 2257500 | Fort Canning Epilogue (v2.11.0) | Vault collateral requirement rules, DUSD payback with vault collateral, higher collateral factor. |
| `DF20GrandCentralHeight` | 2479000 | Grand Central (v3.0.1) | On-chain governance, token consortium framework, masternode parameter updates, pool commission and reward fixes. |
| `DF21GrandCentralEpilogueHeight` | 2574000 | Grand Central Epilogue (v3.2.0) | Mint tokens to any address. |
| `DF22MetachainHeight` | 3462000 | Metachain (v4.0.0) | EVM support, DUSD-looped vaults. |
| `DF23Height` | 4006000 | DF23 | Token split transactions are added to the EVM block template (`src/miner.cpp`). |
| `DF24Height` | 4463000 | DF24 | Token lock creation transactions are added to the EVM block template (`src/miner.cpp`). |
| `DF25Height` | not scheduled (`INT_MAX`) | DF25 (v4.2.7+) | Bundled consensus hardening: account balance `MAX_MONEY` enforcement, stricter pool-pair liquidity arithmetic, stricter loan, vault, smart-contract and token-lock accounting. |

!!! note "DF25 is not scheduled on any public network"

    `DF25Height` is `std::numeric_limits<int>::max()` on mainnet, testnet,
    Changi and DevNet, and `1` on regtest. `Consensus::IsDF25Active()`
    (`src/consensus/params.h`) therefore returns `false` for every public
    network until a release sets a concrete mainnet height. The activation
    mechanism is documented in
    [Upgrade gating](upgrade-gating.md).

## All networks side by side

| Parameter | Mainnet | Testnet | Changi | DevNet | Regtest |
| --- | --- | --- | --- | --- | --- |
| `DF1AMKHeight` | 356500 | 150 | 150 | 150 | 10000000 |
| `DF2BayfrontHeight` | 405000 | 3000 | 3000 | 3000 | 10000000 |
| `DF3BayfrontMarinaHeight` | 465150 | 90470 | 90470 | 90470 | 10000000 |
| `DF4BayfrontGardensHeight` | 488300 | 101342 | 101342 | 101342 | 10000000 |
| `DF5ClarkeQuayHeight` | 595738 | 155000 | 155000 | 155000 | 10000000 |
| `DF6DakotaHeight` | 678000 | 220680 | 220680 | 220680 | 10000000 |
| `DF7DakotaCrescentHeight` | 733000 | 287700 | 287700 | 287700 | 10000000 |
| `DF8EunosHeight` | 894000 | 354950 | 354950 | 354950 | 10000000 |
| `DF9EunosKampungHeight` | 895743 | = `DF8EunosHeight` | = `DF8EunosHeight` | = `DF8EunosHeight` | 10000000 |
| `DF10EunosPayaHeight` | 1072000 | 463300 | 463300 | 463300 | 10000000 |
| `DF11FortCanningHeight` | 1367000 | 686200 | 686200 | 686200 | 10000000 |
| `DF12FortCanningMuseumHeight` | 1430640 | 724000 | 724000 | 724000 | 10000000 |
| `DF13FortCanningParkHeight` | 1503143 | 828800 | 828800 | 828800 | 10000000 |
| `DF14FortCanningHillHeight` | 1604999 | 828900 | 828900 | 828900 | 10000000 |
| `DF15FortCanningRoadHeight` | 1786000 | 893700 | 893700 | 893700 | 10000000 |
| `DF16FortCanningCrunchHeight` | 1936000 | 1011600 | 1011600 | 1011600 | 10000000 |
| `DF17FortCanningSpringHeight` | 2033000 | 1086000 | 1086000 | 1086000 | 10000000 |
| `DF18FortCanningGreatWorldHeight` | 2212000 | 1150000 | 1223000 | 1223000 | 10000000 |
| `DF19FortCanningEpilogueHeight` | 2257500 | 1150010 | 1244000 | 1244000 | 10000000 |
| `DF20GrandCentralHeight` | 2479000 | 1150020 | 1366000 | 1366000 | 10000000 |
| `DF21GrandCentralEpilogueHeight` | 2574000 | 1150030 | 1438200 | 1438200 | 10000000 |
| `DF22MetachainHeight` | 3462000 | 1150040 | 1586750 | 1586750 | 10000000 |
| `DF23Height` | 4006000 | 1507200 | 1985600 | `INT_MAX` | 10000000 |
| `DF24Height` | 4463000 | 1810800 | 2241000 | `INT_MAX` | 10000000 |
| `DF25Height` | `INT_MAX` | `INT_MAX` | `INT_MAX` | `INT_MAX` | 1 |

`INT_MAX` is `std::numeric_limits<int>::max()`, i.e. "not scheduled".

Two details are worth remembering:

* On testnet, Changi and DevNet `DF9EunosKampungHeight` is **assigned from**
  `DF8EunosHeight` (`consensus.DF9EunosKampungHeight = consensus.DF8EunosHeight;`),
  so both forks activate at the same block. Only mainnet separates them.
* On regtest every historic fork is parked at `10000000`, so a test chain
  runs the *current* rule set unless the fork is triggered explicitly with a
  CLI override (see [Upgrade gating](upgrade-gating.md)).

## Non-DF activation heights

Two consensus rules of the v4.2.7 hardening are **not** part of the `DF<n>`
sequence. They are separate consensus constants in `src/consensus/params.h`
because they gate existing behaviour instead of introducing a new fork:

| Parameter | Mainnet | Other networks | Rule |
| --- | --- | --- | --- |
| `nBalanceOverflowHeight` | 6564035 | `INT_MAX` (regtest: `1`) | From this height on, an overflowing sum of transfer amounts is rejected instead of silently wrapping. Implemented in `src/dfi/consensus/txvisitor.cpp` (`CCustomTxVisitor::SumTransfers()`). |
| `nMNVersionGateHeight` | 6564035 | `INT_MAX` (regtest: `10000000`) | From this height on, blocks must signal the overflow fix in `block.nVersion` via `VERSION_BIT_BALANCE_OVERFLOW` (`1 << 24`); blocks without the bit are rejected with `bad-version`. Implemented in `src/validation.cpp` (`ComputeBlockVersion()`, `ContextualCheckBlockHeader()`). |

Both heights are consensus constants: every node must agree on them, so the
CLI overrides are only evaluated on regtest and mocknet (see
[Upgrade gating](upgrade-gating.md#3-add-the-cli-override)).

## Where the rules are implemented

| Area | Location |
| --- | --- |
| Parameter declarations | `src/consensus/params.h` |
| Per-network heights | `src/chainparams.cpp` (`CMainParams`, `CTestNetParams`, `CChangiParams`, `CDevNetParams`, `CRegTestParams`) |
| CLI overrides | `src/chainparams.cpp` (`SetupCommonArgActivationParams()`, `UpdateHeightValidation()`), flags declared in `src/init.cpp` |
| DF23/DF24 block template changes | `src/miner.cpp` |
| DF25 rule switches | `Consensus::IsDF25Active()` in `src/consensus/params.h`, used in `src/dfi/poolpairs.cpp`, `src/dfi/consensus/loans.cpp`, `src/dfi/consensus/vaults.cpp`, `src/dfi/consensus/smartcontracts.cpp`, `src/dfi/consensus/tokenlock.cpp`, `src/dfi/validation.cpp`, `src/mempool_accept.cpp` |
| DF25 account `MAX_MONEY` enforcement | `src/dfi/accounts.cpp` (`CAccountsView::AddBalance()` → `CustomTxErrCodes::AccountBalanceOutOfRange`), `src/dfi/masternodes.cpp` (pool reward balances), switched on in `src/validation.cpp` (`view->SetEnforceAccountMaxMoney(Consensus::IsDF25Active(height, consensus))` in `BlockContext::BlockContext()` and `BlockContext::GetView()`) |
| DF25 block time base | `src/chain.cpp` (`GetFutureBlockTimeLimit()` — from the activation height on the legacy peer-adjusted clock is no longer added to the base time) |
| Transfer sum overflow (`nBalanceOverflowHeight`) | `src/dfi/consensus/txvisitor.cpp` (`CCustomTxVisitor::SumTransfers()` → `CustomTxErrCodes::TransferSumOverflow`), called from `src/dfi/consensus/accounts.cpp` and `src/dfi/consensus/poolpairs.cpp` |
| Masternode version gate (`nMNVersionGateHeight`) | `src/validation.cpp` (`ComputeBlockVersion()`, `ContextualCheckBlockHeader()`), `src/net_processing.cpp` (`MaybePunishNode()`, `PeerLogicValidation::CheckForMasternodeVersionGateStall()`), `src/rpc/blockchain.cpp` (`getblockchaininfo`) |
| Exposure to the Rust layer | `src/ffi/ffiexports.cpp` (`getDF23Height()`, `getDF24Height()`, `getDF25Height()`) |

## Related pages

* [Upgrade gating](upgrade-gating.md) — how to add a height-gated rule.
* [Consensus section index](index.md).
* [Codebase map](../codebase-map/index.md) — where the files above live.
