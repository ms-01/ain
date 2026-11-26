# Tokens and DAT

The token system of DeFiChain is the foundation of every DeFi feature: a
liquidity pool holds two tokens, a vault accepts collateral tokens, a loan
mints a loan token. This page explains how tokens are defined, which kinds
exist and who is allowed to change them.

## Identity

A token is identified by its numeric id, `DCT_ID`:

| Range | Owner | Notes |
| --- | --- | --- |
| `0` | Protocol | DFI, the only DAT. |
| `1` … `127` | Foundation | Reserved for foundation-issued tokens. |
| `128` and above | Users and governance | Every token created after the Bayfront hardfork. |

`CTokensView::DCT_ID_START` is 128 (`src/dfi/tokens.cpp`). A token id is
never reused, not even after the token is destroyed.

Tokens are addressed by symbol in the user interface, but a symbol is not
unique: a DAT is addressed by its symbol alone (`DFI`), every other token by
symbol and id (`BTC#144`). This is the format the node uses internally
(`CToken::CreateSymbolKey()` in `src/dfi/tokens.h`).

## Flags

Every token carries a set of flags (`CToken::TokenFlags` in
`src/dfi/tokens.h`):

| Flag | Value | Meaning |
| --- | --- | --- |
| `Mintable` | `0x01` | The owner may mint additional units. |
| `Tradeable` | `0x02` | The token may be traded on the DEX. |
| `DAT` | `0x04` | The token is a DeFi Asset Token (unique, protocol level). |
| `LPS` | `0x08` | The token is a liquidity pool share. |
| `Finalized` | `0x10` | The token can never be changed again. |
| `LoanToken` | `0x20` | The token is a loan token (dToken). |
| `Deprecated` | `0x40` | The token is deprecated and no longer supported. |

A token that is created without explicit flags gets the default
`Mintable | Tradeable`.

## DAT versus DGT

* A **DAT** (DeFi Asset Token) is unique in the whole protocol. There is
  exactly one: DFI. Creating a DAT requires foundation or governance
  authorisation (`HasGovOrFoundationAuth()` in
  `src/dfi/consensus/tokens.cpp`), and a DAT can also be issued on the EVM
  side as a DST20 token.
* A **DGT** (DeFi Governance Token) is a user-issued token. Any address may
  create one, subject to the fee and the collateral.

## Creating a token

A token creation transaction (`CreateToken`, type `T`) has to carry two
outputs (`CheckTokenCreationTx()` in `src/dfi/consensus/tokens.cpp`):

| Output | Content | Purpose |
| --- | --- | --- |
| `vout[0]` | at least 100 DFI (`consensus.token.creationFee`) | The creation fee, paid to the protocol. |
| `vout[1]` | exactly 1 DFI (`consensus.token.collateralAmount`) | The collateral that authorises future minting. |

The collateral output is locked to the **collateral address** of the token.
Whoever controls that address may mint the token, as long as the token is
`Mintable`.

Further rules for the token itself:

* The symbol must start with a letter, must not be empty, must not contain
  `#`, and — since the Fort Canning Crunch hardfork — must not contain `/`
  (`CToken::IsValidSymbol()` in `src/dfi/tokens.cpp`).
* The symbol is truncated to 8 characters, the name to 128 characters
  (`CToken::MAX_TOKEN_SYMBOL_LENGTH`, `CToken::MAX_TOKEN_NAME_LENGTH`).
* The number of decimals is fixed at 8 and is currently not validated
  (`decimal` in `src/dfi/rpc_tokens.cpp`).
* A liquidity pool share cannot be created manually; it is created by the pool
  logic when liquidity is added.

## Minting and burning

Minting (`MintToken`, type `M`) requires:

* the token to exist and to have the `Mintable` flag,
* the token not to be a liquidity pool share and not to be DFI,
* on mainnet, the token not to be a loan token,
* the transaction to authorise the collateral address — the address of the
  creation transaction or, if it was changed, of the newer collateral
  transaction.

Burning (`BurnToken`, type `F`) moves tokens from an account to the burn
address of the protocol, which removes them from the circulating supply. It
requires the authorisation of the account that holds the tokens
(`CAccountsConsensus`/`CTokensConsensus` in `src/dfi/consensus/tokens.cpp`).
A separate mechanism, the token split, replaces the units of a token without
changing its id.

## Updating a token

`UpdateTokenAny` (type `n`) changes the flags and the metadata of a token.
The rules depend on the height and on who authorises the transaction:

* DFI itself can never be altered (*Can't alter DFI token!*).
* Liquidity pool shares can never be altered.
* A token that is `Finalized` can never be updated.
* A token cannot be updated while it is locked.
* The owner of the token — the holder of the collateral address — may update
  its own token.
* Governance or the foundation may only **deprecate** a token that it does not
  own; changing its name or symbol is rejected with *Only token deprecation
  toggle is allowed by governance*.
* Since the Grand Central hardfork, the collateral address of a token can be
  moved to a new address with a new collateral transaction.
* The `DAT` flag can no longer be changed since the MetaChain hardfork
  (*Cannot change isDAT flag after DF23Height*), because a token cannot be
  migrated to the EVM side on the fly.

A deprecated token keeps its id but receives the `eol/` prefix in its symbol
(`CToken::DeprecationPrefix()`).

## Token splits

A token split (`TokenSplit`, type `P`) changes the number of units of a token
without changing its id: the node stores a **split multiplier** for the token
and applies it to all balances (`CTokensView::SetTokenSplitMultiplier()`).
A split token is addressed with the id suffix, for example `BTC#144`.

## Protocol-owned tokens

Two token kinds are created by protocol modules and not by users:

* **Liquidity pool shares** (`LPS`) are minted when liquidity is added to a
  pool and burnt when it is removed. They carry the pool share of the owner
  and are the unit of the liquidity accounting.
* **Loan tokens** (dTokens, flag `LoanToken`) are created by the loan system
  (`SetLoanToken`, type `g`) and represent debt in a vault. They accrue
  interest over time. See
  [Loans & dTokens](../loans-dtokens/index.md).

## Querying tokens

* `gettoken <symbol-or-id>` returns the properties of a token, including the
  `minted` amount.
* `listtokens` iterates over all tokens, including the liquidity pool shares
  and the loan tokens.

Both RPCs are documented in the
[Node Software Manual](../../node-manual/rpc-cli/index.md).

## Reference

* `src/dfi/tokens.h` — token flags, token limits and the token store.
* `src/dfi/tokens.cpp` — token creation, symbol validation, split multipliers.
* `src/dfi/consensus/tokens.cpp` — the consensus rules of creation, minting,
  burning and updating.
* `src/dfi/consensus/poolpairs.cpp` — liquidity pool shares.
* `src/dfi/consensus/loans.cpp` — loan tokens.
