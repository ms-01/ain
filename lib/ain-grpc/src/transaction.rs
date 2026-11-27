use ain_evm::transaction::{SignedTx, TransactionError};
use ethereum::{AccessListItem, BlockAny, EnvelopedEncodable, TransactionV2};
use ethereum_types::{H256, U256};

use crate::{
    codegen::types::{EthAccessList, EthTransactionInfo},
    utils::{format_address, format_h256, format_u256},
};

impl From<SignedTx> for EthTransactionInfo {
    fn from(signed_tx: SignedTx) -> Self {
        let input = if signed_tx.data().is_empty() {
            String::from("0x")
        } else {
            format!("0x{}", hex::encode(signed_tx.data()))
        };

        let access_list: Vec<EthAccessList> = match &signed_tx.transaction {
            TransactionV2::Legacy(_) => Vec::new(),
            TransactionV2::EIP2930(tx) => tx
                .access_list
                .clone()
                .into_iter()
                .map(std::convert::Into::into)
                .collect(),
            TransactionV2::EIP1559(tx) => tx
                .access_list
                .clone()
                .into_iter()
                .map(std::convert::Into::into)
                .collect(),
        };

        EthTransactionInfo {
            hash: format_h256(signed_tx.hash()),
            from: format_address(signed_tx.sender),
            to: signed_tx.to().map(format_address),
            gas: format_u256(signed_tx.gas_limit()),
            gas_price: format_u256(signed_tx.gas_price()),
            value: format_u256(signed_tx.value()),
            input,
            nonce: format_u256(signed_tx.nonce()),
            v: format!("0x{:x}", signed_tx.v()),
            r: format_h256(signed_tx.r()),
            s: format_h256(signed_tx.s()),
            block_hash: None,
            block_number: None,
            transaction_index: None,
            field_type: format_u256(U256::from(
                signed_tx.transaction.type_id().unwrap_or_default(),
            )),
            max_fee_per_gas: signed_tx
                .max_fee_per_gas()
                .map(format_u256)
                .unwrap_or_default(),
            max_priority_fee_per_gas: signed_tx
                .max_priority_fee_per_gas()
                .map(format_u256)
                .unwrap_or_default(),
            access_list,
            chain_id: format!("{:#x}", signed_tx.chain_id()),
        }
    }
}

impl From<AccessListItem> for EthAccessList {
    fn from(access_list: AccessListItem) -> Self {
        Self {
            address: format_address(access_list.address),
            storage_keys: access_list
                .storage_keys
                .into_iter()
                .map(format_h256)
                .collect(),
        }
    }
}

impl TryFrom<TransactionV2> for EthTransactionInfo {
    type Error = TransactionError;

    fn try_from(tx: TransactionV2) -> Result<Self, Self::Error> {
        let signed_tx: SignedTx = tx.try_into()?;
        Ok(signed_tx.into())
    }
}

impl TryFrom<&str> for EthTransactionInfo {
    type Error = TransactionError;

    fn try_from(raw_tx: &str) -> Result<Self, Self::Error> {
        let signed_tx: SignedTx = raw_tx.try_into()?;
        Ok(signed_tx.into())
    }
}

impl EthTransactionInfo {
    pub fn try_from_tx_block_and_index(
        tx: &TransactionV2,
        block: &BlockAny,
        index: usize,
    ) -> Result<Self, TransactionError> {
        let signed_tx: SignedTx = tx.clone().try_into()?;

        Ok(EthTransactionInfo {
            block_hash: Some(format_h256(block.header.hash())),
            block_number: Some(format_u256(block.header.number)),
            transaction_index: Some(format_u256(U256::from(index))),
            gas_price: format_u256(
                signed_tx
                    .effective_gas_price(block.header.base_fee)
                    .map_err(|_| TransactionError::ConstructionError)?,
            ),
            ..EthTransactionInfo::from(signed_tx)
        })
    }

    #[must_use]
    pub fn into_pending_transaction_info(self) -> Self {
        Self {
            block_hash: Some(format_h256(H256::zero())),
            block_number: Some(String::from("null")),
            transaction_index: Some(String::from("0x0")),
            ..self
        }
    }
}

#[cfg(test)]
mod tests {
    use ain_evm::transaction::{LegacyUnsignedTransaction, SignedTx};
    use ethereum::{
        AccessListItem, Block, EIP1559Transaction, EIP2930Transaction, EnvelopedEncodable,
        PartialHeader, TransactionAction, TransactionSignature, TransactionV2,
    };
    use ethereum_types::{H160, H256, U256};

    use crate::codegen::types::{EthAccessList, EthTransactionInfo};

    const TEST_CHAIN_ID: u64 = 1127;
    const TEST_SECRET_KEY: [u8; 32] = [0x02; 32];

    fn make_legacy_signed(input: Vec<u8>, action: TransactionAction) -> SignedTx {
        let unsigned = LegacyUnsignedTransaction {
            nonce: U256::from(1),
            gas_price: U256::from(10_000_000_000u64),
            gas_limit: U256::from(21_000u64),
            action,
            value: U256::from(5u64),
            input,
            sig: TransactionSignature::new(27, H256::from_low_u64_be(1), H256::from_low_u64_be(1))
                .unwrap(),
        };
        let signed = unsigned
            .sign_with_chain_id(&TEST_SECRET_KEY, TEST_CHAIN_ID)
            .unwrap();
        SignedTx::try_from(TransactionV2::Legacy(signed)).unwrap()
    }

    #[test]
    fn eth_transaction_info_converts_legacy_eip2930_and_eip1559() {
        let legacy =
            make_legacy_signed(vec![], TransactionAction::Call(H160::from_low_u64_be(0xee)));
        let info = EthTransactionInfo::from(legacy.clone());
        assert_eq!(info.input, "0x");
        assert!(info.to.is_some());
        assert!(info.access_list.is_empty());
        assert_eq!(info.field_type, "0x0");

        let legacy_create = make_legacy_signed(vec![0x60, 0x00], TransactionAction::Create);
        let create_info = EthTransactionInfo::from(legacy_create);
        assert_eq!(create_info.input, "0x6000");
        assert!(create_info.to.is_none());

        let access_item = AccessListItem {
            address: H160::from_low_u64_be(0x11),
            storage_keys: vec![H256::from_low_u64_be(0x22)],
        };
        let eth_al = EthAccessList::from(access_item.clone());
        assert_eq!(eth_al.storage_keys.len(), 1);

        let eip2930_tx = TransactionV2::EIP2930(EIP2930Transaction {
            chain_id: TEST_CHAIN_ID,
            nonce: U256::from(2),
            gas_price: U256::from(10_000_000_000u64),
            gas_limit: U256::from(30_000u64),
            action: TransactionAction::Call(H160::from_low_u64_be(0xaa)),
            value: U256::zero(),
            input: vec![0xde, 0xad],
            access_list: vec![access_item.clone()],
            odd_y_parity: false,
            r: H256::from_low_u64_be(1),
            s: H256::from_low_u64_be(1),
        });
        let eip2930_info = EthTransactionInfo::try_from(eip2930_tx).unwrap();
        assert_eq!(eip2930_info.field_type, "0x1");
        assert_eq!(eip2930_info.access_list.len(), 1);

        let eip1559_tx = TransactionV2::EIP1559(EIP1559Transaction {
            chain_id: TEST_CHAIN_ID,
            nonce: U256::from(3),
            max_priority_fee_per_gas: U256::from(1_000_000_000u64),
            max_fee_per_gas: U256::from(20_000_000_000u64),
            gas_limit: U256::from(30_000u64),
            action: TransactionAction::Call(H160::from_low_u64_be(0xbb)),
            value: U256::zero(),
            input: vec![],
            access_list: vec![access_item],
            odd_y_parity: false,
            r: H256::from_low_u64_be(1),
            s: H256::from_low_u64_be(1),
        });
        let eip1559_info = EthTransactionInfo::try_from(eip1559_tx).unwrap();
        assert_eq!(eip1559_info.field_type, "0x2");
        assert_eq!(eip1559_info.access_list.len(), 1);
    }

    #[test]
    fn eth_transaction_info_try_from_str_block_and_pending() {
        let legacy = make_legacy_signed(
            vec![0x01],
            TransactionAction::Call(H160::from_low_u64_be(0xee)),
        );
        let raw_hex = hex::encode(legacy.transaction.encode());
        let info = EthTransactionInfo::try_from(raw_hex.as_str()).unwrap();
        assert_eq!(info.input, "0x01");
        let info_from_tx = EthTransactionInfo::try_from(legacy.transaction.clone()).unwrap();
        assert_eq!(info_from_tx.hash, info.hash);

        assert!(EthTransactionInfo::try_from("not-valid-hex").is_err());

        let partial = PartialHeader {
            parent_hash: H256::zero(),
            beneficiary: H160::zero(),
            state_root: H256::zero(),
            receipts_root: H256::zero(),
            logs_bloom: Default::default(),
            difficulty: U256::zero(),
            number: U256::from(7),
            gas_limit: U256::from(30_000_000),
            gas_used: U256::zero(),
            timestamp: 1_700_000_000,
            extra_data: vec![],
            mix_hash: H256::zero(),
            nonce: Default::default(),
            base_fee: U256::from(1_000_000_000u64),
        };
        let block = Block::new(partial, vec![legacy.transaction.clone()], vec![]);
        let in_block =
            EthTransactionInfo::try_from_tx_block_and_index(&legacy.transaction, &block, 0)
                .unwrap();
        assert_eq!(in_block.block_number, Some("0x7".to_string()));
        assert_eq!(in_block.transaction_index, Some("0x0".to_string()));
        assert!(in_block.block_hash.is_some());

        let pending = info.into_pending_transaction_info();
        assert_eq!(pending.block_number, Some("null".to_string()));
        assert_eq!(pending.transaction_index, Some("0x0".to_string()));
    }
}
