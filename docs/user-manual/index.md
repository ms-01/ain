# User & Protocol Manual

The **User & Protocol Manual** explains how the DeFiChain protocol works and how
to use it. It is the entry point of the canonical DeFiChain documentation suite
defined in [ADR-0002](../developer-manual/adr/ADR-0002-documentation-structure.md).

The manual is written for **users of the protocol**: token holders, liquidity
providers, vault operators, masternode operators, governance participants and
developers who build on top of DeFiChain. If you want to *run* a node, read the
[Node Software Manual](../node-manual/index.md) instead. If you want to work on
the node software itself, read the
[Developer & Architecture Manual](../developer-manual/index.md).

## Sections

| Section | What it covers |
| --- | --- |
| [Getting Started](getting-started/index.md) | Core protocol concepts, wallets and addresses, the UTXO and the account model, and the DFI tokenomics overview. |
| [DVM](dvm/index.md) | The native DeFi VM: custom transactions (DfTx), the token system, and how value moves between the UTXO set and accounts. |
| [EVM](evm/index.md) | The MetaChain and its EVM layer, gas and fees, and cross-layer transfers between the DVM and the EVM. |
| [Staking & Masternodes](staking-masternodes/index.md) | Masternode registration, collateral, timelocks, minting and reward distribution. |
| [DEX & Liquidity](dex-liquidity/index.md) | Liquidity pools, swapping, commissions, slippage and ICX atomic swaps. |
| [Loans & dTokens](loans-dtokens/index.md) | Vaults, collateral, loans, dTokens, price oracles and liquidation auctions. |
| [Governance](governance/index.md) | On-chain governance, CFPs, DFIPs and VOC proposals, voting and quorum rules. |

## Reading paths

The manual is organised by protocol domain, so the fastest way in depends on
what you want to achieve.

### "I am new to DeFiChain"

1. [Core concepts](getting-started/core-concepts.md) — the two chain layers,
   blocks, masternodes and fees.
2. [Wallets and addresses](getting-started/wallets-and-addresses.md) — address
   formats, key types and how authorisation works.
3. [UTXO and accounts](getting-started/utxo-and-accounts.md) — where your
   balances live and how they move.
4. [DFI tokenomics](getting-started/dfi-tokenomics.md) — what DFI is worth and
   what it is used for.

### "I want to use a DeFi feature"

Read [Core concepts](getting-started/core-concepts.md) first, then go directly
to the domain section: [DEX & Liquidity](dex-liquidity/index.md),
[Loans & dTokens](loans-dtokens/index.md),
[Staking & Masternodes](staking-masternodes/index.md) or
[Governance](governance/index.md).

### "I want to move funds or build on the DVM"

1. [Custom transactions](dvm/custom-transactions.md) — the transaction types
   the DVM understands.
2. [Tokens and DAT](dvm/tokens-and-dat.md) — how tokens are defined and which
   token is the DAT.
3. [Account transfers](dvm/account-transfers.md) — the exact rules for
   UTXO ↔ account conversions and account-to-account transfers.

### "I already know DeFiChain and need the exact rule"

Go straight to the reference-style pages of the relevant domain section. Every
protocol rule in this manual is stated together with the source file in
`src/dfi/` that implements it, so it can be verified against the node.

## Conventions

* Amounts are written in the decimal unit of the token. DFI has 8 decimals,
  so `0.00000001 DFI` is the smallest representable DFI amount.
* Token identifiers are written as `DCT_ID`, the on-chain token id. DFI is
  `DCT_ID 0`; user-created tokens start at `DCT_ID 128`.
* Type codes such as `'U'` refer to the one-byte type identifier of a custom
  transaction, as defined in `src/dfi/customtx.h`.
* Code references such as `src/dfi/customtx.cpp` point at the node
  implementation in this repository.

## Related manuals

* [Node Software Manual](../node-manual/index.md) — installation,
  configuration, operations, RPC/CLI reference and troubleshooting.
* [Developer & Architecture Manual](../developer-manual/index.md) —
  architecture, codebase map, consensus rules and build & test workflow.
