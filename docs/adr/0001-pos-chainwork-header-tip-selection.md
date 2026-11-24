# ADR-0001: PoS Chainwork Soundness and Anchored Header-Tip Selection

## Status

Accepted

## Context

On DeFiChain's Proof-of-Stake (PoS) chain, header-level validation (`pos::CheckHeaderSignature()`, `ContextualCheckBlockHeader()`, `pos::CheckStakeModifier()`) does not verify stake ownership, masternode registration, or PoS kernel validity (`pos::ContextualCheckProofOfStake()` only executes on full blocks).

Meanwhile, `pos::CalculateNextWorkRequired()` (`src/pos.cpp`) clamps `nActualTimespan` to `[nTargetTimespan/4, 4 * nTargetTimespan]`. A peer with a single keypair can fork from any historical block, space forged headers 1 second apart, and inflate claimed per-block work (`GetBlockProof()`, `src/chain.cpp`) by up to 4x per retarget period, exceeding mainnet `consensus.nMinimumChainWork` in a few thousand headers.

### Audit of `nChainWork` Consumers

1. **`pindexBestHeader` selection** (`BlockManager::AddToBlockIndex()`, `BlockManager::LoadBlockIndex()` in `src/validation.cpp`): Directly exploitable. A forged high-work fork hijacks `pindexBestHeader`, bypassing the unanchored side-chain header budget (`fExtendsBestHeaderChain` in `src/net_processing.cpp`), redirecting `getheaders` locators onto the forged chain, and distorting `GetBlockProofEquivalentTime(*pindexBestHeader, ...)` (`src/chain.cpp`), which suppresses relay of honest recent blocks.
2. **`CChainState::IsInitialBlockDownload()`** (`src/validation.cpp`): Compares `m_chain.Tip()->nChainWork < nMinimumChainWork`. Because `m_chain.Tip()` is the validated active chain, this is not forgeable by header-only attacks.
3. **`CanSkipScriptChecks()` / assumevalid** (`src/validation.cpp`): Requires `m_assumed_valid` to be an ancestor of `pindexBestHeader` and checks `pindexBestHeader->nChainWork >= nMinimumChainWork`. Hardening `pindexBestHeader` protects this path as well.
4. **`FindNextBlocksToDownload()`** (`src/net_processing.cpp`): Evaluates `state->pindexBestKnownBlock->nChainWork` per peer, so honest peers still deliver blocks independently of `pindexBestHeader`.

## Decision

Treat `nChainWork` as **advisory only** for unvalidated headers on PoS and select `pindexBestHeader` using a single deterministic comparator `IsBetterHeaderTip()` across all update sites (`AddToBlockIndex`, `LoadBlockIndex`, `ConnectTip`, `RecomputeBestHeader`):

1. **Anchored height (default `anchored` mode):** Compute `GetAnchoredHeight(pindex, active_tip)` lazily from live `CBlockIndex::nStatus`:
   - Return `0` for `nullptr` or `BLOCK_FAILED_MASK`.
   - Return `pindex->nHeight` in O(1) if `pindex->IsValid(BLOCK_VALID_SCRIPTS)`.
   - Use an O(log N) `pskip` active-chain fast path (`pindex->GetAncestor(active_tip->nHeight) == active_tip`) so honest headers-first IBD works arbitrarily far ahead of `active_tip`.
   - Fall back to a bounded `pprev` walk of at most `MAX_ANCHORED_WALK_DEPTH = 64` hops (worst-case <= 128 pointer dereferences per comparison).
2. **Tie-break (and sole criterion in `chainwork` mode):** Higher `nChainWork`, then lower `GetBlockHash()` for running-vs-restarted determinism.
3. **Rollback handling:** Guarded `BlockManager::RecomputeBestHeader()` on `InvalidateBlock()` (only when `pindexBestHeader` has `BLOCK_FAILED_MASK` or descends from the invalidated block) and `RewindBlockIndex()` (only when an ancestor of `pindexBestHeader` is rewound).
4. **Operator override:** Non-consensus CLI option `-headertipmode=<anchored|chainwork>` (default: `anchored`), validated in `AppInitParameterInteraction()`.

## Consequences

- **Non-consensus:** No consensus rules, activation heights, or on-disk formats change.
- **Closed threats:** Historical forged forks (`fork_height < active_tip->nHeight`) can no longer hijack `pindexBestHeader` (at runtime or after a restart, because a validated block exists above the fork base) and remain bounded by the unanchored side-chain header budget. At runtime, permanent `pindexBestHeader` lockup from an at-tip fork is eliminated because `ConnectTip()` re-evaluates `pindexBestHeader` whenever a new honest block validates.
- **Accepted residual risks:**
  - *Live-tip re-fork window:* A peer that forks directly *at* the current validated tip `H` (or inside the unvalidated region above `H`) shares `anchored_height == H` and can temporarily win the `nChainWork` tie-break until honest block `H+1` validates (~30s on mainnet). During that window, `fExtendsBestHeaderChain == true` and up to `MAX_HEADERS_RESULTS` (2,000) headers per `headers` message are not counted by the insertion-time side-chain budget. Closing this live-tip re-fork accounting window is tracked separately to keep this change single-PR scoped.
  - *Post-restart reconstruction window (> `MAX_ANCHORED_WALK_DEPTH` unvalidated headers above `H`):* Because `LoadBlockIndex()` runs before `active_tip` is known, an unvalidated header chain extending more than `MAX_ANCHORED_WALK_DEPTH` (64) blocks above the validated tip `H` that is persisted to disk and survives a restart before `H+1` validates can remain `pindexBestHeader` immediately after restart until a newer honest block validates above the fork base. Once a validated block exists above the fork base, post-restart reconstruction keeps `pindexBestHeader` on the validated chain.
