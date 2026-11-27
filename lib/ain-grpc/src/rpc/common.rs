use std::{
    cmp::max,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use ain_evm::{
    storage::{traits::BlockStorage, Storage},
    transaction::SignedTx,
};
use ethereum::{Block, BlockAny, PartialHeader};
use ethereum_types::U256;
use jsonrpsee::core::RpcResult;

use crate::{block::BlockNumber, errors::RPCError};

/// The mempool entry type of a raw EVM transaction (`SystemTxType::EVMTx`).
///
/// The mempool also holds transfer domain transactions, they are not part of
/// the EVM pending block.
const POOL_TX_TYPE_EVM: u8 = 0;

/// A mempool entry as reported by the node.
#[derive(Clone, Debug)]
pub struct PoolEntry {
    /// The type of the entry (`0` = EVM transaction, `1` = transfer domain).
    pub tx_type: u8,
    /// The hex encoded raw transaction (without `0x` prefix).
    pub data: String,
}

/// The kind of block a block selector refers to.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SelectorKind {
    /// A block of the committed EVM chain.
    Committed,
    /// The synthetic pending block.
    Pending,
}

/// Maps a block selector to the kind of block it refers to.
///
/// The match is exhaustive on purpose: a new [`BlockNumber`] variant has to be
/// mapped explicitly, it can never fall back to the latest block silently.
#[must_use]
pub fn selector_kind(selector: &BlockNumber) -> SelectorKind {
    match selector {
        BlockNumber::Hash { .. }
        | BlockNumber::Num(_)
        | BlockNumber::Latest
        | BlockNumber::Earliest
        | BlockNumber::Safe
        | BlockNumber::Finalized => SelectorKind::Committed,
        BlockNumber::Pending => SelectorKind::Pending,
    }
}

/// The resolution of a block selector.
///
/// A block selector and a state selector are two different concepts: the
/// `pending` selector refers to a block that does not exist yet (the synthetic
/// pending block), while the *state* of the `pending` selector is the state of
/// the latest committed block. The resolution keeps both apart, so that no
/// caller can silently substitute one for the other.
pub enum ResolvedBlock {
    /// A block of the committed EVM chain.
    Committed(Box<BlockAny>),
    /// The synthetic pending block.
    Pending {
        /// The synthetic pending block: the latest committed block plus one,
        /// carrying the transactions that are currently in the mempool.
        block: Box<BlockAny>,
        /// The latest committed block the pending block is built on.
        committed: Box<BlockAny>,
    },
}

impl ResolvedBlock {
    /// The committed block the selector resolves to.
    ///
    /// For the `pending` selector this is the latest committed block. The node
    /// cannot compute a faithful pending state: the mempool transactions are
    /// executed on the C++ side during block production (including the DVM
    /// side effects), so replaying them in the RPC layer would produce a state
    /// that differs from the state the node would actually produce. The
    /// limitation is documented in the methods that resolve a state.
    #[must_use]
    pub fn committed(&self) -> &BlockAny {
        match self {
            ResolvedBlock::Committed(block) => block,
            ResolvedBlock::Pending { committed, .. } => committed,
        }
    }

    /// The synthetic pending block, if the selector resolved to it.
    #[must_use]
    pub fn pending(&self) -> Option<&BlockAny> {
        match self {
            ResolvedBlock::Committed(_) => None,
            ResolvedBlock::Pending { block, .. } => Some(block),
        }
    }

    /// Whether the selector resolved to the synthetic pending block.
    #[must_use]
    pub fn is_pending(&self) -> bool {
        matches!(self, ResolvedBlock::Pending { .. })
    }
}

/// Resolves a block selector into the block it refers to.
///
/// The match is exhaustive on purpose: a new [`BlockNumber`] variant has to be
/// mapped explicitly, it can never fall back to the latest block silently.
pub fn resolve_block(
    storage: &Arc<Storage>,
    block_number: Option<BlockNumber>,
) -> RpcResult<ResolvedBlock> {
    match block_number.unwrap_or(BlockNumber::Latest) {
        BlockNumber::Hash { hash, .. } => {
            let block = storage
                .get_block_by_hash(&hash)
                .map_err(RPCError::EvmError)?;
            committed_block(block)
        }
        BlockNumber::Num(n) => {
            let block = storage
                .get_block_by_number(&U256::from(n))
                .map_err(RPCError::EvmError)?;
            committed_block(block)
        }
        BlockNumber::Earliest => {
            let block = storage
                .get_block_by_number(&U256::zero())
                .map_err(RPCError::EvmError)?;
            committed_block(block)
        }
        BlockNumber::Latest => {
            let block = storage.get_latest_block().map_err(RPCError::EvmError)?;
            committed_block(block)
        }
        BlockNumber::Safe | BlockNumber::Finalized => {
            let latest = storage.get_latest_block().map_err(RPCError::EvmError)?;
            let Some(latest) = latest else {
                return Err(RPCError::BlockNotFound.into());
            };
            let finality_count = ain_cpp_imports::get_attribute_values(None).finality_count;
            let Some(safe_block_number) =
                latest.header.number.checked_sub(U256::from(finality_count))
            else {
                return Err(RPCError::BlockNotFound.into());
            };
            let block = storage
                .get_block_by_number(&safe_block_number)
                .map_err(RPCError::EvmError)?;
            committed_block(block)
        }
        BlockNumber::Pending => {
            let latest = storage.get_latest_block().map_err(RPCError::EvmError)?;
            let Some(latest) = latest else {
                return Err(RPCError::BlockNotFound.into());
            };
            let transactions = pending_transactions();
            let block = build_pending_block(&latest, &transactions);
            Ok(ResolvedBlock::Pending {
                block: Box::new(block),
                committed: Box::new(latest),
            })
        }
    }
}

/// Resolves a selector to the number of the committed block it refers to.
///
/// The `pending` selector resolves explicitly to the latest committed block
/// number: the pending block carries neither logs nor fee history data, and a
/// range that ends at the pending block number would be rejected.
pub fn committed_block_number(
    storage: &Arc<Storage>,
    block_number: Option<BlockNumber>,
) -> RpcResult<U256> {
    let resolved = resolve_block(storage, block_number)?;
    Ok(resolved.committed().header.number)
}

/// Wraps a committed block into a [`ResolvedBlock`].
fn committed_block(block: Option<BlockAny>) -> RpcResult<ResolvedBlock> {
    block
        .map(|block| ResolvedBlock::Committed(Box::new(block)))
        .ok_or_else(|| RPCError::BlockNotFound.into())
}

/// Returns the EVM transactions that are currently in the mempool.
///
/// The transactions are ordered by `(sender, nonce)`, the same order the block
/// template uses. Entries that are not EVM transactions and entries that
/// cannot be decoded are skipped, they never make the whole call fail.
#[must_use]
pub fn pending_transactions() -> Vec<SignedTx> {
    let Ok(entries) = ain_cpp_imports::get_pool_transactions() else {
        return Vec::new();
    };
    let entries: Vec<PoolEntry> = entries
        .into_iter()
        .map(|entry| PoolEntry {
            tx_type: entry.tx_type,
            data: entry.data,
        })
        .collect();
    decode_pool_transactions(&entries)
}

/// Decodes the EVM transactions of the given mempool entries.
///
/// Entries that do not carry an EVM transaction (e.g. transfer domain
/// transactions) and entries that cannot be decoded are skipped, they never
/// make the whole call fail.
#[must_use]
pub fn decode_pool_transactions(entries: &[PoolEntry]) -> Vec<SignedTx> {
    let mut transactions: Vec<SignedTx> = entries
        .iter()
        .filter(|entry| entry.tx_type == POOL_TX_TYPE_EVM)
        .filter_map(|entry| SignedTx::try_from(entry.data.as_str()).ok())
        .collect();
    transactions.sort_by(|a, b| {
        a.sender
            .cmp(&b.sender)
            .then_with(|| a.nonce().cmp(&b.nonce()))
    });
    transactions
}

/// Builds the synthetic pending block on top of the given committed block.
///
/// The pending block is not a block of the chain: its number is the number of
/// the latest committed block plus one and its parent is the latest committed
/// block. The header fields that cannot be known before the block is produced
/// (`state_root`, `receipts_root`, `logs_bloom`, `gas_used`, `base_fee` and
/// `gas_limit`) are copied from the latest committed block and are therefore
/// provisional. The `transactions_root` is computed from the pending
/// transactions.
///
/// The block contains the wall clock time, so its hash is not stable across
/// calls. Clients must not use the hash of the pending block as an identity.
#[must_use]
pub fn build_pending_block(latest: &BlockAny, transactions: &[SignedTx]) -> BlockAny {
    let header = &latest.header;
    let partial_header = PartialHeader {
        parent_hash: header.hash(),
        beneficiary: header.beneficiary,
        state_root: header.state_root,
        receipts_root: header.receipts_root,
        logs_bloom: header.logs_bloom,
        difficulty: header.difficulty,
        number: header.number.saturating_add(U256::one()),
        gas_limit: header.gas_limit,
        gas_used: header.gas_used,
        timestamp: max(header.timestamp, current_timestamp()),
        extra_data: header.extra_data.clone(),
        mix_hash: header.mix_hash,
        nonce: header.nonce,
        base_fee: header.base_fee,
    };
    Block::new(
        partial_header,
        transactions
            .iter()
            .map(|transaction| transaction.transaction.clone())
            .collect(),
        Vec::new(),
    )
}

/// The current wall clock time in seconds since the Unix epoch.
fn current_timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use ain_evm::transaction::LegacyUnsignedTransaction;
    use ethereum::{TransactionAction, TransactionSignature};
    use ethereum_types::{H160, H256, U256};

    use super::*;

    /// The chain id used to sign the test transactions.
    const TEST_CHAIN_ID: u64 = 1127;
    /// The secret key used to sign the test transactions.
    const TEST_SECRET_KEY: [u8; 32] = [0x01; 32];

    /// Builds the hex encoded raw transaction of a signed legacy transaction.
    fn signed_tx_hex(nonce: u64) -> String {
        let unsigned = LegacyUnsignedTransaction {
            nonce: U256::from(nonce),
            gas_price: U256::from(10_000_000_000u64),
            gas_limit: U256::from(21_000u64),
            action: TransactionAction::Call(H160::from_low_u64_be(0xee)),
            value: U256::from(1u64),
            input: Vec::new(),
            sig: TransactionSignature::new(27, H256::from_low_u64_be(1), H256::from_low_u64_be(1))
                .unwrap(),
        };
        let signed = unsigned
            .sign_with_chain_id(&TEST_SECRET_KEY, TEST_CHAIN_ID)
            .unwrap();
        hex::encode(rlp::encode(&signed))
    }

    /// Builds a committed block with the given number.
    fn committed_block(number: u64) -> BlockAny {
        let partial_header = PartialHeader {
            parent_hash: H256::from_low_u64_be(0xaa),
            beneficiary: H160::zero(),
            state_root: H256::from_low_u64_be(0xbb),
            receipts_root: H256::from_low_u64_be(0xcc),
            logs_bloom: Default::default(),
            difficulty: U256::from(2u64),
            number: U256::from(number),
            gas_limit: U256::from(30_000_000u64),
            gas_used: U256::from(21_000u64),
            timestamp: 1_700_000_000,
            extra_data: Default::default(),
            mix_hash: H256::from_low_u64_be(0xdd),
            nonce: Default::default(),
            base_fee: U256::from(1_000_000_000u64),
        };
        Block::new(partial_header, Vec::new(), Vec::new())
    }

    /// The selector kind mapping is exhaustive, no selector falls back to the
    /// latest block.
    #[test]
    fn selector_kinds_are_mapped_explicitly() {
        assert_eq!(selector_kind(&BlockNumber::Latest), SelectorKind::Committed);
        assert_eq!(
            selector_kind(&BlockNumber::Earliest),
            SelectorKind::Committed
        );
        assert_eq!(selector_kind(&BlockNumber::Safe), SelectorKind::Committed);
        assert_eq!(
            selector_kind(&BlockNumber::Finalized),
            SelectorKind::Committed
        );
        assert_eq!(selector_kind(&BlockNumber::Num(1)), SelectorKind::Committed);
        assert_eq!(
            selector_kind(&BlockNumber::Hash {
                hash: H256::zero(),
                require_canonical: false
            }),
            SelectorKind::Committed
        );
        assert_eq!(selector_kind(&BlockNumber::Pending), SelectorKind::Pending);
    }

    /// The pending block is the latest committed block plus one and its parent
    /// is the latest committed block.
    #[test]
    fn pending_block_is_built_on_the_latest_block() {
        let latest = committed_block(42);
        let transactions = vec![SignedTx::try_from(signed_tx_hex(0).as_str()).unwrap()];
        let pending = build_pending_block(&latest, &transactions);

        assert_eq!(pending.header.number, U256::from(43u64));
        assert_eq!(pending.header.parent_hash, latest.header.hash());
        assert!(pending.header.timestamp >= latest.header.timestamp);
        assert!(pending.ommers.is_empty());
        assert_eq!(pending.transactions.len(), 1);
        // The transactions root is computed from the pending transactions.
        assert_ne!(
            pending.header.transactions_root,
            latest.header.transactions_root
        );
    }

    /// The provisional header fields are copied from the latest block.
    #[test]
    fn pending_block_copies_the_provisional_fields() {
        let latest = committed_block(42);
        let pending = build_pending_block(&latest, &[]);

        assert_eq!(pending.header.state_root, latest.header.state_root);
        assert_eq!(pending.header.receipts_root, latest.header.receipts_root);
        assert_eq!(pending.header.gas_limit, latest.header.gas_limit);
        assert_eq!(pending.header.gas_used, latest.header.gas_used);
        assert_eq!(pending.header.base_fee, latest.header.base_fee);
        assert_eq!(pending.header.difficulty, latest.header.difficulty);
        assert_eq!(pending.header.beneficiary, latest.header.beneficiary);
    }

    /// The pending block of an empty mempool is still distinct from the latest
    /// block.
    #[test]
    fn pending_block_of_an_empty_mempool_differs_from_the_latest_block() {
        let latest = committed_block(42);
        let pending = build_pending_block(&latest, &[]);

        assert_ne!(pending.header.number, latest.header.number);
        assert_ne!(pending.header.hash(), latest.header.hash());
        assert!(pending.transactions.is_empty());
    }

    /// Only the EVM entries of the mempool are decoded, the transfer domain
    /// entries and the malformed entries are skipped.
    #[test]
    fn pool_transactions_skip_non_evm_and_malformed_entries() {
        let raw = signed_tx_hex(0);
        let entries = vec![
            PoolEntry {
                tx_type: POOL_TX_TYPE_EVM,
                data: raw.clone(),
            },
            PoolEntry {
                tx_type: 1,
                data: raw.clone(),
            },
            PoolEntry {
                tx_type: POOL_TX_TYPE_EVM,
                data: "not-a-hex-transaction".to_string(),
            },
        ];

        let transactions = decode_pool_transactions(&entries);
        assert_eq!(transactions.len(), 1);
        assert_eq!(transactions[0].nonce(), U256::zero());
    }

    #[test]
    fn resolved_block_accessors_and_committed_block_helper() {
        let block = committed_block(10);
        let resolved = super::committed_block(Some(block.clone())).unwrap();
        assert!(!resolved.is_pending());
        assert!(resolved.pending().is_none());
        assert_eq!(resolved.committed().header.number, U256::from(10));

        assert!(super::committed_block(None).is_err());

        let pending_block = build_pending_block(&block, &[]);
        let resolved_pending = ResolvedBlock::Pending {
            block: Box::new(pending_block.clone()),
            committed: Box::new(block.clone()),
        };
        assert!(resolved_pending.is_pending());
        assert_eq!(
            resolved_pending.pending().unwrap().header.number,
            U256::from(11)
        );
        assert_eq!(resolved_pending.committed().header.number, U256::from(10));
    }

    #[test]
    fn pool_transactions_are_sorted_by_sender_and_nonce() {
        let entries = vec![
            PoolEntry {
                tx_type: POOL_TX_TYPE_EVM,
                data: signed_tx_hex(2),
            },
            PoolEntry {
                tx_type: POOL_TX_TYPE_EVM,
                data: signed_tx_hex(0),
            },
            PoolEntry {
                tx_type: POOL_TX_TYPE_EVM,
                data: signed_tx_hex(1),
            },
        ];

        let transactions = decode_pool_transactions(&entries);
        assert_eq!(transactions.len(), 3);
        assert_eq!(transactions[0].nonce(), U256::from(0));
        assert_eq!(transactions[1].nonce(), U256::from(1));
        assert_eq!(transactions[2].nonce(), U256::from(2));
    }
}
