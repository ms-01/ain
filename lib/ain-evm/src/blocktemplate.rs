use ethereum::{Block, ReceiptV3, TransactionV2};
use ethereum_types::{Bloom, H160, U256};

use crate::{
    backend::{EVMBackend, Vicinity},
    core::XHash,
    evm::{BlockContext, ExecTxState},
    receipt::Receipt,
    transaction::SignedTx,
};

type Result<T> = std::result::Result<T, BlockTemplateError>;

pub type ReceiptAndOptionalContractAddress = (ReceiptV3, Option<H160>);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TemplateTxItem {
    pub tx: Box<SignedTx>,
    pub tx_hash: XHash,
    pub gas_fees: U256,
    pub gas_used: U256,
    pub logs_bloom: Bloom,
    pub receipt_v3: ReceiptAndOptionalContractAddress,
}

impl TemplateTxItem {
    pub fn new_system_tx(
        tx: Box<SignedTx>,
        receipt_v3: ReceiptAndOptionalContractAddress,
        logs_bloom: Bloom,
    ) -> Self {
        TemplateTxItem {
            tx,
            tx_hash: Default::default(),
            gas_fees: U256::zero(),
            gas_used: U256::zero(),
            logs_bloom,
            receipt_v3,
        }
    }
}

#[derive(Clone, Debug)]
pub struct BlockData {
    pub block: Block<TransactionV2>,
    pub receipts: Vec<Receipt>,
}

/// The `BlockTemplateData` contains:
/// 1. Validated transactions in the block template
/// 2. Block data
/// 3. Total gas used by all in the block template
/// 4. Backend vicinity
/// 5. Block template timestamp
/// 6. DVM block number
/// 7. EVM backend
///
/// The template is used to construct a valid EVM block.
///
pub struct BlockTemplate {
    pub transactions: Vec<TemplateTxItem>,
    pub block_data: Option<BlockData>,
    pub total_gas_used: U256,
    pub vicinity: Vicinity,
    pub ctx: BlockContext,
    pub backend: EVMBackend,
}

impl BlockTemplate {
    pub fn new(vicinity: Vicinity, ctx: BlockContext, backend: EVMBackend) -> Self {
        Self {
            transactions: Vec::new(),
            block_data: None,
            total_gas_used: U256::zero(),
            vicinity,
            ctx,
            backend,
        }
    }

    pub fn add_tx(&mut self, tx_update: ExecTxState, tx_hash: XHash) -> Result<()> {
        self.total_gas_used = self
            .total_gas_used
            .checked_add(tx_update.gas_used)
            .ok_or(BlockTemplateError::ValueOverflow)?;

        self.transactions.push(TemplateTxItem {
            tx: tx_update.tx,
            tx_hash,
            gas_fees: tx_update.gas_fees,
            gas_used: tx_update.gas_used,
            logs_bloom: tx_update.logs_bloom,
            receipt_v3: tx_update.receipt,
        });
        Ok(())
    }

    pub fn remove_txs_above_hash(&mut self, target_hash: XHash) -> Result<Vec<XHash>> {
        let mut removed_txs = Vec::new();

        if let Some(index) = self
            .transactions
            .iter()
            .position(|item| item.tx_hash == target_hash)
        {
            removed_txs = self
                .transactions
                .drain(index..)
                .map(|tx_item| tx_item.tx_hash)
                .collect();

            self.total_gas_used = self.transactions.iter().try_fold(U256::zero(), |acc, tx| {
                acc.checked_add(tx.gas_used)
                    .ok_or(BlockTemplateError::ValueOverflow)
            })?;
            self.backend.reset_from_tx(index);
        }

        Ok(removed_txs)
    }

    pub fn get_latest_logs_bloom(&self) -> Bloom {
        self.transactions
            .last()
            .map_or(Bloom::default(), |tx_item| tx_item.logs_bloom)
    }

    pub fn get_block_base_fee_per_gas(&self) -> U256 {
        self.vicinity.block_base_fee_per_gas
    }

    pub fn get_block_number(&self) -> U256 {
        self.vicinity.block_number
    }

    pub fn is_genesis_block(&self) -> bool {
        self.get_block_number() == U256::zero()
    }
}

#[derive(Debug)]
pub enum BlockTemplateError {
    NoSuchTemplate,
    ValueOverflow,
}

impl std::fmt::Display for BlockTemplateError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            BlockTemplateError::NoSuchTemplate => write!(f, "No block template for this id"),
            BlockTemplateError::ValueOverflow => {
                write!(f, "Value overflow when updating block template")
            }
        }
    }
}

impl std::error::Error for BlockTemplateError {}

#[cfg(test)]
mod tests {
    use std::{
        path::PathBuf,
        sync::{
            atomic::{AtomicU64, Ordering},
            Arc,
        },
    };

    use ethereum::{
        EIP658ReceiptData, LegacyTransaction, ReceiptV3, TransactionAction, TransactionSignature,
        TransactionV2,
    };
    use ethereum_types::{Bloom, H160, U256};

    use super::*;
    use crate::{
        backend::{EVMBackend, Vicinity},
        evm::{BlockContext, ExecTxState},
        storage::Storage,
        transaction::{SignedTx, LOWER_H256},
        trie::GENESIS_STATE_ROOT,
    };

    static COUNTER: AtomicU64 = AtomicU64::new(0);

    fn make_backend() -> (parking_lot::MutexGuard<'static, ()>, EVMBackend, PathBuf) {
        let guard = crate::evm_unit_tests::vsdb_lock();
        let trie_store = crate::evm_unit_tests::test_trie_store();
        let id = COUNTER.fetch_add(1, Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!(
            "ain_evm_bt_tests_storage_{}_{}",
            std::process::id(),
            id
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let storage = Arc::new(Storage::new(&dir).unwrap());
        let backend = EVMBackend::from_root(
            GENESIS_STATE_ROOT,
            trie_store,
            storage,
            Vicinity::default(),
            None,
        )
        .unwrap();
        (guard, backend, dir)
    }

    fn make_signed_tx(nonce: u64) -> Box<SignedTx> {
        let tx = TransactionV2::Legacy(LegacyTransaction {
            nonce: U256::from(nonce),
            gas_price: U256::from(10_000_000_000u64),
            gas_limit: U256::from(21_000u64),
            action: TransactionAction::Call(H160::from_low_u64_be(0x1234)),
            value: U256::zero(),
            input: Vec::new(),
            signature: TransactionSignature::new(27, LOWER_H256, LOWER_H256).unwrap(),
        });
        Box::new(SignedTx::try_from(tx).unwrap())
    }

    fn make_receipt(used_gas: u64) -> ReceiptV3 {
        ReceiptV3::EIP1559(EIP658ReceiptData {
            status_code: 1,
            used_gas: U256::from(used_gas),
            logs_bloom: Bloom::default(),
            logs: Vec::new(),
        })
    }

    #[test]
    fn block_template_add_remove_and_getters() {
        let (_guard, backend, dir) = make_backend();
        let vicinity = Vicinity {
            block_number: U256::zero(),
            block_base_fee_per_gas: U256::from(10_000_000_000u64),
            ..Vicinity::default()
        };
        let mut template = BlockTemplate::new(
            vicinity,
            BlockContext::new_for_test(1, 30_000_000),
            backend,
        );

        assert!(template.is_genesis_block());
        assert_eq!(template.get_block_number(), U256::zero());
        assert_eq!(
            template.get_block_base_fee_per_gas(),
            U256::from(10_000_000_000u64)
        );
        assert_eq!(template.get_latest_logs_bloom(), Bloom::default());

        let hash1 = [1u8; 32];
        let hash2 = [2u8; 32];
        let mut bloom2 = Bloom::default();
        bloom2.0[0] = 0xab;

        template
            .add_tx(
                ExecTxState {
                    tx: make_signed_tx(0),
                    receipt: (make_receipt(21_000), None),
                    logs_bloom: Bloom::default(),
                    gas_used: U256::from(21_000u64),
                    gas_fees: U256::from(100),
                },
                hash1,
            )
            .unwrap();
        template.backend.increase_tx_count();

        template
            .add_tx(
                ExecTxState {
                    tx: make_signed_tx(1),
                    receipt: (make_receipt(30_000), None),
                    logs_bloom: bloom2,
                    gas_used: U256::from(30_000u64),
                    gas_fees: U256::from(200),
                },
                hash2,
            )
            .unwrap();
        template.backend.increase_tx_count();

        assert_eq!(template.total_gas_used, U256::from(51_000u64));
        assert_eq!(template.get_latest_logs_bloom(), bloom2);

        // Removing a non-existent hash removes nothing
        assert!(template.remove_txs_above_hash([9u8; 32]).unwrap().is_empty());

        // Removing hash2 leaves hash1 and recalculates total_gas_used
        let removed = template.remove_txs_above_hash(hash2).unwrap();
        assert_eq!(removed, vec![hash2]);
        assert_eq!(template.transactions.len(), 1);
        assert_eq!(template.total_gas_used, U256::from(21_000u64));

        // Overflow on add_tx
        let overflow = template.add_tx(
            ExecTxState {
                tx: make_signed_tx(2),
                receipt: (make_receipt(1), None),
                logs_bloom: Bloom::default(),
                gas_used: U256::MAX,
                gas_fees: U256::zero(),
            },
            [3u8; 32],
        );
        assert!(matches!(overflow, Err(BlockTemplateError::ValueOverflow)));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn template_system_tx_and_error_display() {
        let item = TemplateTxItem::new_system_tx(
            make_signed_tx(0),
            (make_receipt(0), None),
            Bloom::default(),
        );
        assert_eq!(item.gas_used, U256::zero());
        assert_eq!(item.gas_fees, U256::zero());
        assert_eq!(item.tx_hash, [0u8; 32]);

        assert!(!BlockTemplateError::NoSuchTemplate.to_string().is_empty());
        assert!(!BlockTemplateError::ValueOverflow.to_string().is_empty());
    }
}
