# Consensus

Consensus rules, hardforks and activation-height gating such as DF25Height.

Part of the [Developer & Architecture Manual](../index.md).

## Contents

| Document | What it covers |
| --- | --- |
| [Hardfork timeline](hardforks.md) | Every hardfork from `DF1AMKHeight` to `DF25Height` with the activation height of mainnet, testnet, Changi, DevNet and regtest. |
| [Upgrade gating](upgrade-gating.md) | How to add a height-gated consensus rule: consensus parameter, per-network heights, CLI overrides, mempool coupling, error codes and the required tests. |

## About this page

This section describes the consensus rules of the node: the hardfork
timeline with the activation height of every network, and the pattern that
every height-gated rule follows. The heights and code references are
transcribed from the source tree, so a change in `src/consensus/params.h` or
`src/chainparams.cpp` has to be mirrored here.

## Related pages

* [Codebase map](../codebase-map/index.md) — where the consensus code lives.
* [Subsystems](../subsystems/index.md) — mempool, validation and block
  production in detail.
