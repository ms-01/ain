# Custom transactions

A **custom transaction** (DfTx) is an ordinary DeFiChain transaction whose
first output carries a payload for the DeFi VM. This page explains how that
payload is structured, which types exist and how to inspect a DfTx.

## Structure of a DfTx

A transaction is a DfTx if its **first output** (`vout[0]`) is a provably
non-spendable `OP_RETURN` output whose data starts with the marker `DfTx`
followed by a one-byte type identifier and the type-specific metadata:

```text
vout[0].scriptPubKey = OP_RETURN <push "DfTx"> <type byte> <metadata…>
                        ^^^^^^^^^^^^^^^ ^^^^^^^^^ ^^^^^^^^^^^^^^^^^
                        marker           type       payload
```

The rules are enforced by `GuessCustomTxType()` in `src/dfi/customtx.cpp` and
by `ParseScriptByMarker()` in `src/consensus/tx_check.cpp`:

* The marker must be in the **first** output. A marker in any later output
  makes the whole transaction invalid.
* Exactly **one** DfTx per transaction: a second marker anywhere in the
  transaction is rejected.
* Additional opcodes after the marker in the same output are rejected.
* The first metadata byte is the type. An unknown type is rejected.

Everything else about the transaction stays ordinary: it has inputs, it pays a
fee in DFI, it is signed by its inputs and it is added to the mempool like any
other transaction. Many DfTx types use additional outputs — `UtxosToAccount`
burns value in `OP_RETURN` outputs, `AccountToUtxos` mints value in regular
outputs — and those outputs are part of the ordinary value flow of the
transaction.

## Type identifiers

The type identifiers are defined in `src/dfi/customtx.h`
(`enum class CustomTxType`). The table below lists all of them, grouped by
the protocol area they belong to.

### Masternodes

| Code | Type | Purpose |
| --- | --- | --- |
| `C` | `CreateMasternode` | Register a masternode and lock its collateral. |
| `R` | `ResignMasternode` | Start the resignation of a masternode. |
| `m` | `UpdateMasternode` | Change masternode metadata such as the reward address. |
| `A` | `AutoAuthPrep` | Create a dedicated authorisation output for an address that shall authorise masternode actions. |

### Tokens

| Code | Type | Purpose |
| --- | --- | --- |
| `T` | `CreateToken` | Create a new token (DGT). |
| `M` | `MintToken` | Mint additional units of a mintable token. |
| `F` | `BurnToken` | Burn tokens and remove them from the supply. |
| `N` | `UpdateToken` | Legacy token update, only the DAT flag is honoured. |
| `n` | `UpdateTokenAny` | Token update with arbitrary flags and fields. |

### Liquidity pools

| Code | Type | Purpose |
| --- | --- | --- |
| `p` | `CreatePoolPair` | Create a liquidity pool for a token pair. |
| `u` | `UpdatePoolPair` | Change pool parameters such as the commission. |
| `s` | `PoolSwap` | Swap tokens in a pool. |
| `i` | `PoolSwapV2` | Composite swap that routes a swap through a list of pools (`poolIDs`). |
| `l` | `AddPoolLiquidity` | Add liquidity and receive pool shares. |
| `r` | `RemovePoolLiquidity` | Remove liquidity and burn pool shares. |

### Accounts and balances

| Code | Type | Purpose |
| --- | --- | --- |
| `U` | `UtxosToAccount` | Convert UTXO value into account balances. |
| `b` | `AccountToUtxos` | Convert account balances back into UTXO value. |
| `B` | `AccountToAccount` | Transfer tokens between two accounts. |
| `a` | `AnyAccountsToAccounts` | Transfer tokens between any number of accounts. |
| `K` | `SmartContract` | Execute a built-in smart contract, for example DFIP2201. |
| `Q` | `FutureSwap` | Place a future swap order (DFIP2203 / DFIP2206F). |

### Governance variables

| Code | Type | Purpose |
| --- | --- | --- |
| `G` | `SetGovVariable` | Set a governance variable, for example a pool commission. |
| `j` | `SetGovVariableHeight` | Schedule a governance variable for a future height. |
| `Z` | `UnsetGovVariable` | Remove a governance variable. |
| `-` | `UnsetGovHeightVariable` | Remove a scheduled governance variable. |
| `+` | `ClearGovHeights` | Clear all scheduled governance heights. |

### Oracles

| Code | Type | Purpose |
| --- | --- | --- |
| `o` | `AppointOracle` | Appoint an oracle. |
| `h` | `RemoveOracleAppoint` | Remove an appointed oracle. |
| `t` | `UpdateOracleAppoint` | Update the appointment of an oracle. |
| `y` | `SetOracleData` | Publish a price for a currency or a token. |

### ICX (atomic swaps)

| Code | Type | Purpose |
| --- | --- | --- |
| `1` | `ICXCreateOrder` | Create an order for an atomic swap. |
| `2` | `ICXMakeOffer` | Make an offer for an existing order. |
| `3` | `ICXSubmitDFCHTLC` | Submit the DeFiChain side of the HTLC. |
| `4` | `ICXSubmitEXTHTLC` | Submit the external side of the HTLC. |
| `5` | `ICXClaimDFCHTLC` | Claim the DeFiChain side of the HTLC. |
| `6` | `ICXCloseOrder` | Close an order. |
| `7` | `ICXCloseOffer` | Close an offer. |

### Loans and vaults

| Code | Type | Purpose |
| --- | --- | --- |
| `c` | `SetLoanCollateralToken` | Declare a token as acceptable collateral. |
| `g` | `SetLoanToken` | Create a loan token (dToken). |
| `x` | `UpdateLoanToken` | Update a loan token. |
| `L` | `LoanScheme` | Create a loan scheme. |
| `d` | `DefaultLoanScheme` | Set the default loan scheme. |
| `D` | `DestroyLoanScheme` | Destroy a loan scheme. |
| `V` | `Vault` | Create a vault. |
| `e` | `CloseVault` | Close a vault. |
| `v` | `UpdateVault` | Update the parameters of a vault. |
| `S` | `DepositToVault` | Deposit collateral into a vault. |
| `J` | `WithdrawFromVault` | Withdraw collateral from a vault. |
| `W` | `PaybackWithCollateral` | Pay back a loan with collateral. |
| `X` | `TakeLoan` | Take a loan from a vault. |
| `H` | `PaybackLoan` | Pay back a loan. |
| `k` | `PaybackLoanV2` | Pay back a loan, variant with an explicit token list. |
| `I` | `AuctionBid` | Bid on a liquidation auction. |

### On-chain governance

| Code | Type | Purpose |
| --- | --- | --- |
| `z` | `CreateCfp` | Submit a community proposal (CFP). |
| `E` | `CreateVoc` | Submit a vote of confidence (VOC). |
| `O` | `Vote` | Cast a masternode vote on a proposal. |
| `Y` | `ProposalFeeRedistribution` | Redistribute the fees of a rejected proposal. |

### Marker transactions

| Code | Type | Purpose |
| --- | --- | --- |
| `q` | `FutureSwapExecution` | Settlement of an expired future swap. |
| `w` | `FutureSwapRefund` | Refund of an expired future swap. |
| `P` | `TokenSplit` | Split a token. |
| `?` | `TokenLock` | Lock tokens in the token lock contract. |
| `!` | `TokenLockRelease` | Release locked tokens. |

### EVM

| Code | Type | Purpose |
| --- | --- | --- |
| `8` | `TransferDomain` | Transfer assets between the DVM and the EVM. |
| `9` | `EvmTx` | An EVM transaction executed on the MetaChain. |

!!! note "Type codes are not unique across protocols"

    The one-byte codes are reused between protocol areas. For example `V` is
    `Vault` and `m` is `UpdateMasternode`; the code alone never identifies a
    transaction outside the context of the surrounding protocol rules. Two
    examples of historical collisions are noted in the source itself: `Vote`
    (`O`) overlaps with the ICX order code and `CreateVoc` (`E`) overlaps with
    the ICX offer code.

## Inspecting a DfTx

* `getcustomtx <txid>` decodes the payload of a DfTx and returns its fields
  in JSON (`src/dfi/rpc_tokens.cpp`).
* `getrawtransaction <txid>` shows the raw transaction as hex, including the
  `OP_RETURN` payload of the first output.
* `getrawtransaction <txid> true` shows the decoded transaction, including the
  script of the first output, which is where the `DfTx` marker is encoded.

## Rejected transactions

When a DfTx does not satisfy its consensus rules, the node rejects it with an
error message that names the violated rule, for example:

* *transfer tokens mismatch burnt tokens* — a `UtxosToAccount` that burns less
  than it credits.
* *amount of minted tokens in UTXOs and metadata do not match* — an
  `AccountToUtxos` whose minted outputs do not match its payload.
* *only available for DFI transactions* — an `AccountToUtxos` for a token
  other than DFI.
* *sum of inputs (from) != sum of outputs (to)* — an
  `AnyAccountsToAccounts` with unbalanced amounts.
* *tx must have at least one input from account owner* — a missing
  authorisation (`DeFiErrors::InvalidAuth()` in `src/dfi/errors.h`).

The RPC reference is part of the
[Node Software Manual](../../node-manual/rpc-cli/index.md).

## Reference

* `src/dfi/customtx.h` — the `CustomTxType` enumeration with all type codes.
* `src/dfi/customtx.cpp` — `GuessCustomTxType()`, the type dispatch and the
  human readable names.
* `src/consensus/tx_check.cpp` — the `DfTx` marker parsing.
* `src/dfi/consensus/txvisitor.cpp` — dispatch of a decoded transaction to
  the consensus modules.
