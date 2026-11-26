# DFI tokenomics

This page describes the DFI token: what it is technically, how it is issued,
how it is distributed and what it is used for inside the protocol.

## What DFI is

DFI is the native token of DeFiChain.

| Property | Value | Source |
| --- | --- | --- |
| Token id | `DCT_ID 0` | `CTokensView::CreateDFIToken()` in `src/dfi/tokens.cpp` |
| Symbol | `DFI` | `CURRENCY_UNIT` |
| Decimals | 8 | `CToken::decimal` |
| Flags | `DAT`, `Tradeable`, `Finalized` | `src/dfi/tokens.cpp` |
| Creation height | 0 | `creationHeight = 0` |

DFI is the only **DAT** (DeFi Asset Token) of the protocol. DATs are unique:
there is exactly one, it has the lowest token id, and it can never be
replaced or split. Every other token is a DGT (DeFi Governance Token) with an
id of 128 or higher. See [Tokens and DAT](../dvm/tokens-and-dat.md).

Because DFI is `Finalized`, no update transaction can change its properties.
Because it is not `Mintable`, no user can mint DFI directly; new DFI only
enters the system through the block subsidy.

## Issuance: the block subsidy

New DFI is created by the block subsidy. The subsidy is a function of the
block height:

* Before the Eunos hardfork the subsidy is 200 DFI per block
  (`consensus.baseBlockSubsidy`).
* From the Eunos hardfork onwards (`consensus.DF8EunosHeight`) the subsidy
  starts at **405.04 DFI per block** (`consensus.newBaseBlockSubsidy`) and is
  reduced by **1.658 % every 32,690 blocks**, i.e. roughly every two weeks
  (`consensus.emissionReductionPeriod` and `consensus.emissionReductionAmount`
  in `src/chainparams.cpp`).

The subsidy is paid in the coinbase transaction of the block. From the Grand
Central hardfork onwards (`consensus.DF20GrandCentralHeight`) it is split
between the community accounts:

| Share | Destination |
| --- | --- |
| 33.33 % | Masternodes (`consensus.dist.masternode`) |
| 25.45 % | Liquidity providers, via the incentive funding account |
| 24.68 % | Loan system rewards, via the loan account |
| 9.88 % | Options account |
| 4.91 % | Community development funds |
| 1.73 % | Unallocated account |
| 0.02 % | Anchor (SPV) rewards |

The exact reward mechanics, including the timelock multipliers that apply to
masternode owners, are described in the
[Staking & Masternodes section](../staking-masternodes/index.md).

## The lifetime cap

The whitepaper states that at most **1.3 billion DFI** will ever be created.
The node does not hard-code that number as a supply limit. Instead it
enforces a consensus sanity bound of **1.2 billion DFI** on any single amount
or account balance (`MAX_MONEY` in `src/amount.h`), which is a safety check
against overflow and not a supply figure.

## What DFI is used for

DFI is the settlement and collateral currency of the protocol:

| Use | Amount | Source |
| --- | --- | --- |
| Transaction fees | at least 0.00001 DFI per kB | `DEFAULT_MIN_RELAY_TX_FEE` in `src/validation.h` |
| Masternode collateral | 20,000 DFI (1,000,000 DFI before the Dakota hardfork) | `GetMnCollateralAmount()` in `src/dfi/masternodes.cpp` |
| Masternode creation fee | 10 DFI | `consensus.mn.creationFee` |
| Token creation fee | 100 DFI | `consensus.token.creationFee` |
| Token creation collateral | 1 DFI | `consensus.token.collateralAmount` |
| Vault collateral | user defined, typically DFI and dUSD | [Loans & dTokens](../loans-dtokens/index.md) |
| Liquidity provision | user defined | [DEX & Liquidity](../dex-liquidity/index.md) |
| Governance proposals | fee in DFI, see the governance section | [Governance](../governance/cfps-and-dfips.md) |

The stablecoin dUSD and the dTokens are *credit-based*: they are minted against
vault collateral and are not backed one-to-one by DFI. DFI is the asset that
carries the price risk of the loan system.

## Reading supply figures from a node

The protocol does not store a single "total supply" number that can be read
off directly. The figures that matter are derived from the chain state:

* `gettoken DFI` and `listtokens` report the **minted** amount of each token
  (`src/dfi/rpc_tokens.cpp`).
* `getaccount <address>` and `getbalances` report the account balances of an
  address, per token.
* `getburninfo` reports the amounts burnt in token splits, locks and
  conversions (`src/dfi/rpc_accounts.cpp`).
* `listaccounts` iterates over all accounts, which is how the total amount
  held in accounts is measured.

Because the balance of an address can be split between the UTXO set and the
account layer, a correct supply measurement has to add both parts. The
tokenomics review of the DeFiChain workspace repository keeps the current
measurements and the open questions; it is the reference for the figures that
are not derivable from the protocol rules alone.

!!! warning "Measure, do not assume"

    Supply figures change with every block that is mined and differ between
    the UTXO layer, the account layer and the EVM layer. Always state the
    block height and the measurement method next to a number.

### Snapshot from the workspace tokenomics review

The DeFiChain workspace keeps a tokenomics review. Its latest snapshot
combines the **review of 16./17. September 2026** (taken at block `6,431,125`)
with the **API snapshot of 13. November 2026**; the two dates are separate
measurements and are not a single reading of one block. It records the
following figures. They are **measurements, not protocol constants**, and they
have to be re-measured on a full node after a chain restart:

| Figure | Value |
| --- | --- |
| Community Fund | 19.4 mio. DFI and about 131,000 dUSD |
| Proposal fee | 187,500 DFI per on-chain proposal |
| Gross minted dUSD | 315.2 mio. dUSD, including locked and burnt amounts |
| Lock ratio of dUSD and dTokens | 0.9, with none of the 100 planned tranches released |
| Minimum vault collateralisation | 150 %, of which at least 50 % in DFI for dUSD loans |
| Active oracles | 4 of 9 |

The same review lists the DFI related questions that are still open — the
total, burnt and circulating DFI supply, the split between the UTXO layer,
the account layer and the EVM layer, the amount of staked DFI and the number
of active masternodes. Answering them requires a full node, which is why the
figures above are dated.

## Reference

* `src/dfi/tokens.cpp` — DFI token definition and token creation.
* `src/chainparams.cpp` — subsidy, reduction schedule and reward split.
* `src/validation.cpp` — `GetBlockSubsidy()` and coinbase validation.
* `src/amount.h` — `MAX_MONEY` sanity bound.
* `src/dfi/masternodes.cpp` — masternode collateral and fees.
