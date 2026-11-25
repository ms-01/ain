# P2P networking and IBD

This page describes the peer-to-peer layer of the node: how connections are
established, how blocks and headers are synchronised, and how the initial
block download (IBD) is detected. It also documents the anchored header-tip
selection that protects the PoS chain against forged chainwork.

Part of the [Subsystems section](index.md) of the
[Developer & Architecture Manual](../index.md).

!!! info "Source of truth"

    The connection handling lives in `src/net.{h,cpp}`, the message
    processing in `src/net_processing.{h,cpp}`, the header-tip selection in
    `src/validation.cpp` and the peer limits in `src/validation.h`.

## The layers

| Layer | Files | Responsibility |
| --- | --- | --- |
| Sockets and connections | `src/net.h`, `src/net.cpp` | `CConnman` (the connection manager), `CNode` (one peer), the socket handler thread, the outbound connection thread, the message threads. |
| Transport | `src/bip324.{h,cpp}` | The v2 (encrypted) transport, negotiated per connection. |
| Addresses | `src/addrman.{h,cpp}`, `src/addrdb.{h,cpp}` | The address manager and its `peers.dat` persistence. |
| Bans | `src/banman.{h,cpp}` | The ban list (`banlist` in the data directory) and the ban scores of the peers. |
| Permissions | `src/net_permissions.{h,cpp}` | `-whitelist`, `-whitelistrelay`, `-whitelistforcerelay`, `-noban`. |
| Groups | `src/netgroup.{h,cpp}` | The `-netgroup`/`-whitelist` group rules. |
| Message processing | `src/net_processing.{h,cpp}` | `PeerLogicValidation`: the per-peer state machine and the message handlers. |
| Block requests | `src/txrequest.{h,cpp}` | `TxRequestTracker`, the parallel download tracker for the announced blocks. |
| Chain state | `src/validation.{h,cpp}` | The block index, `pindexBestHeader`, `IsInitialBlockDownload()`. |

## Connections

`CConnman::Start()` (`src/net.cpp`) starts the threads that drive the
network:

* the socket handler thread (`ThreadSocketHandler()`) accepts the incoming
  connections and reads and writes the messages,
* the outbound connection thread opens the connections to the addresses of
  the address manager (`OpenNetworkConnection()`),
* the message handler threads call `ProcessMessage()` of the peer logic.

The number of inbound peers is limited by `-maxconnections`
(`DEFAULT_MAX_PEER_CONNECTIONS` = 125, `src/net.h`); the number of
outbound connections is derived from it. `-connect` pins the node to a fixed
set of peers, `-connect=0` disables the outbound connections.

Every peer has a `CNodeState` in the peer logic. It holds the download
progress (the best known block, the last common block, the best header sent
and the number of blocks in flight) and the misbehaviour score
(`nMisbehavior`). The announced block inventory is kept separately in
`CNode::vInventoryBlockToSend`, and the transaction request tracker is a
single global instance (`g_txrequest`) that is keyed by peer id.

## Message processing

`PeerLogicValidation::ProcessMessage()` (`src/net_processing.cpp`) dispatches
the messages of the Bitcoin protocol (`version`, `verack`, `ping`, `pong`,
`addr`, `inv`, `getdata`, `tx`, `block`, `headers`, `getheaders`, `getblocks`,
`mempool`, `feefilter`, `sendheaders`, `sendcmpct`, …) and the DeFiChain
specific anchor messages (`getauths`, `anchorauth`, `anchorconf`,
`src/protocol.cpp`).

Misbehaviour is scored (`Misbehaving()`), and a peer that exceeds the
threshold is banned or disconnected.

## Header synchronisation

`ProcessHeadersMessage()` (`src/net_processing.cpp`) is the entry point for
the `headers` message and for the header of a compact block
(`cmpctblock`) when the node falls back to header processing
(`via_compact_block=true`):

* A `headers` message carries at most `MAX_HEADERS_RESULTS` (2 000,
  `src/validation.h`) headers.
* The headers are inserted into the block index by
  `BlockManager::AddToBlockIndex()` (`src/validation.cpp`). A header that
  does not extend the current best header chain is *side chain* material and
  is counted against the unanchored header budget:

  | Setting | Meaning |
  | --- | --- |
  | `-maxunanchoredheaders` | Global budget of unanchored headers (`DEFAULT_MAX_UNANCHORED_HEADERS` = 200 000). |
  | `-maxunanchoredheadersperpeer` | Per-peer budget (`DEFAULT_MAX_UNANCHORED_HEADERS_PER_PEER` = 20 000). |

  The budget is released when the headers become anchored, i.e. when a
  validated block is connected below them (`UnanchorHeaders()`).
* When the peer stops sending headers because it reached the tip, the node
  switches from the header sync to the block download.

## Anchored header-tip selection

On a PoS chain the header-level validation does not prove stake ownership,
so a peer that controls a single key pair can forge a side chain with a high
claimed chainwork and hijack `pindexBestHeader`. The node therefore selects
the header tip with a single deterministic comparator.

The decision is recorded in
[ADR-0001: PoS Chainwork Soundness and Anchored Header-Tip Selection](../adr/ADR-0001-pos-chainwork-header-tip-selection.md).
The implementation is in `src/validation.cpp`:

| Symbol | Role |
| --- | --- |
| `GetAnchoredHeight()` | The height of the validated block the header chain is anchored to (0 for an unknown or failed chain). |
| `IsBetterHeaderTip()` | The comparator: a higher anchored height wins, the chainwork is only the tie-break. |
| `BlockManager::AddToBlockIndex()`, `LoadBlockIndex()`, `ConnectTip()`, `RecomputeBestHeader()` | The update sites that use the comparator. |

The policy is selected with the non-consensus option
`-headertipmode=<anchored|chainwork>` (`src/init.cpp`):

* `anchored` (the default) prefers the chain that is backed by validated PoS
  blocks and is therefore not vulnerable to forged chainwork.
* `chainwork` restores the legacy behaviour of comparing the raw
  `nChainWork` values. It is only kept for operators who need the old
  behaviour and it is explicitly documented as vulnerable on PoS.

## Block download

`FindNextBlocksToDownload()` (`src/net_processing.cpp`) selects the next
blocks to request from a peer. At most `MAX_BLOCKS_IN_TRANSIT_PER_PEER`
(16, `src/validation.h`) blocks are in flight per peer, and the parallel
requests are tracked by `TxRequestTracker` (`src/txrequest.{h,cpp}`).

A peer that stalls the block download is disconnected. The timeout logic
lives in `src/net_processing_stalling.h`:

* the default timeout is `BLOCK_STALLING_TIMEOUT_DEFAULT` (2 s),
* the timeout is doubled on every stall, capped at
  `BLOCK_STALLING_TIMEOUT_MAX` (64 s),
* it is reduced slowly (15 % per connected block) after a block was
  connected,
* a manual peer that stalls is put on a
  `MANUAL_PEER_BLOCK_DOWNLOAD_COOLDOWN` (2 min) cooldown.

With `-blocksonly` the node rejects the transactions of the peers (the
wallet, the RPC and the whitelisted peers are not affected).

## Initial block download

`CChainState::IsInitialBlockDownload()` (`src/validation.cpp`) decides
whether the node is still syncing. The node is in IBD when

* a reindex or an import is running (`fImporting`, `fReindex`), or
* there is no tip yet, or
* the tip's chainwork is below `consensus.nMinimumChainWork`, or
* the tip is older than `nMaxTipAge`.

The result is latched: once IBD has finished, the node does not re-enter it
unless it is restarted or reindexed. The state is used in many places, for
example to decide whether transactions are relayed
(`src/net_processing.cpp`) or whether the fee filter is announced.

The masternode version gate adds a second stall check:
`PeerLogicValidation::CheckForMasternodeVersionGateStall()`
(`src/net_processing.cpp`) disconnects the peers that do not signal the
`VERSION_BIT_BALANCE_OVERFLOW` block version after
`nMNVersionGateHeight` (see the
[hardfork timeline](../consensus/hardforks.md)).

## Transaction relay

* A transaction is announced with an `inv` and requested with `getdata`.
  The relay honours the minimum fee (`-minrelaytxfee`), the `-feefilter`
  announcement and the `-blocksonly` mode.
* Transactions whose parents are unknown are kept in the orphan pool
  (`src/node/txorphanage.cpp`); see the
  [mempool page](mempool.md) for the acceptance rules.
* The EVM transactions of the MetaChain layer are not relayed through the
  Bitcoin P2P protocol; they are part of the EVM block template and are
  handled by the Rust layer (see the
  [C++ ↔ Rust bridge](../architecture/cxx-bridge.md)).

## Operator settings

| Option | Effect |
| --- | --- |
| `-maxconnections=<n>` | Maximum number of inbound peers (default 125). |
| `-connect=<addr>` | Connect only to the given peers; `-connect=0` disables the outbound connections. |
| `-blocksonly` | Do not accept transactions from the peers. |
| `-maxuploadtarget=<MB>` | Keep the outbound traffic below the given target per 24 h. |
| `-whitelist`, `-whitelistrelay`, `-whitelistforcerelay`, `-noban` | The peer permissions. |
| `-maxunanchoredheaders`, `-maxunanchoredheadersperpeer` | The side-chain header budget. |
| `-headertipmode=<anchored\|chainwork>` | The header-tip selection policy (see ADR-0001). |
| `-netgroup`, `-whitelist` | The peer group rules. |

## Related pages

* [Mempool and transaction acceptance](mempool.md) — what happens to a
  transaction after it arrives.
* [PoS validation and masternodes](pos-validation.md) — the validation of
  the blocks that are downloaded.
* [State storage](state-storage.md) — where the block index is stored.
* [Performance and IBD](../../node-manual/configuration/performance-ibd.md) —
  the operator-facing tuning of the synchronisation.
* [Sync and reindex](../../node-manual/troubleshooting/sync-and-reindex.md) —
  the operator-facing recovery procedures.
