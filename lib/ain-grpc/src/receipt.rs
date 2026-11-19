use ain_evm::{bytes::Bytes, receipt::Receipt};
use ethereum::{EIP658ReceiptData, Log};
use ethereum_types::{H160, H256, U256};

use crate::logs::LogResult;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ReceiptResult {
    pub block_hash: H256,
    pub block_number: U256,
    pub contract_address: Option<H160>,
    pub cumulative_gas_used: U256,
    pub effective_gas_price: U256,
    pub from: H160,
    pub gas_used: U256,
    pub logs: Vec<LogResult>,
    pub logs_bloom: String,
    pub status: String,
    pub to: Option<H160>,
    pub transaction_hash: H256,
    pub transaction_index: String,
    pub r#type: String,
}

impl From<Receipt> for ReceiptResult {
    fn from(b: Receipt) -> Self {
        let data = EIP658ReceiptData::from(b.receipt);
        ReceiptResult {
            block_hash: b.block_hash,
            block_number: b.block_number,
            contract_address: b.contract_address,
            cumulative_gas_used: b.cumulative_gas,
            effective_gas_price: b.effective_gas_price,
            from: b.from,
            gas_used: data.used_gas,
            logs: {
                data.logs
                    .into_iter()
                    .enumerate()
                    .map(
                        |(
                            log_index,
                            Log {
                                address,
                                topics,
                                data,
                            },
                        )| LogResult {
                            address,
                            topics,
                            data: Bytes::from(data),
                            block_number: b.block_number,
                            block_hash: b.block_hash,
                            transaction_hash: b.tx_hash,
                            transaction_index: b.tx_index.into(),
                            log_index: (b.logs_index + log_index).into(), // safe since we sub the total number of logs during receipt generation
                            removed: false,
                        },
                    )
                    .collect::<Vec<LogResult>>()
            },
            logs_bloom: format!("{:#x}", data.logs_bloom),
            status: format!("{:#x}", data.status_code),
            to: b.to,
            transaction_hash: b.tx_hash,
            transaction_index: format!("{:#x}", b.tx_index),
            r#type: format!("{:#x}", b.tx_type),
        }
    }
}

#[cfg(test)]
mod tests {
    use ethereum::{EIP658ReceiptData, Log, ReceiptV3};
    use ethereum_types::{Bloom, H160, H256, U256};
    use serde_json::json;

    use ain_evm::receipt::Receipt;

    use crate::receipt::ReceiptResult;

    fn make_log(address: H160, topic: H256, data: Vec<u8>) -> Log {
        Log {
            address,
            topics: vec![topic],
            data,
        }
    }

    fn make_receipt(status_code: u8, used_gas: u64, logs: Vec<Log>) -> Receipt {
        Receipt {
            tx_hash: H256::from_low_u64_be(0xaa),
            receipt: ReceiptV3::Legacy(EIP658ReceiptData {
                status_code,
                used_gas: U256::from(used_gas),
                logs_bloom: Bloom::default(),
                logs,
            }),
            block_hash: H256::from_low_u64_be(0xbb),
            block_number: U256::from(100),
            from: H160::from_low_u64_be(0x11),
            to: Some(H160::from_low_u64_be(0x22)),
            tx_index: 3,
            tx_type: 2,
            contract_address: None,
            logs_index: 0,
            cumulative_gas: U256::from(500),
            effective_gas_price: U256::from(7),
        }
    }

    #[test]
    fn receipt_result_reports_success_status() {
        let result = ReceiptResult::from(make_receipt(1, 21_000, vec![]));

        assert_eq!(result.status, "0x1");
        assert_eq!(result.gas_used, U256::from(21_000));
        assert_eq!(result.cumulative_gas_used, U256::from(500));
        assert_eq!(result.transaction_index, "0x3");
        assert_eq!(result.r#type, "0x2");
        assert!(result.logs.is_empty());
    }

    #[test]
    fn receipt_result_reports_revert_status() {
        let result = ReceiptResult::from(make_receipt(0, 21_000, vec![]));

        assert_eq!(result.status, "0x0");
        assert_eq!(result.gas_used, U256::from(21_000));
        assert!(result.logs.is_empty());
    }

    #[test]
    fn receipt_result_offsets_log_indices_by_receipt_logs_index() {
        let mut receipt = make_receipt(
            1,
            21_000,
            vec![
                make_log(
                    H160::from_low_u64_be(0x11),
                    H256::from_low_u64_be(1),
                    vec![],
                ),
                make_log(
                    H160::from_low_u64_be(0x22),
                    H256::from_low_u64_be(2),
                    vec![],
                ),
            ],
        );
        receipt.logs_index = 2;

        let result = ReceiptResult::from(receipt);

        assert_eq!(result.logs.len(), 2);
        assert_eq!(result.logs[0].log_index, U256::from(2));
        assert_eq!(result.logs[1].log_index, U256::from(3));
    }

    #[test]
    fn receipt_result_carries_contract_address_for_creations() {
        let mut receipt = make_receipt(1, 21_000, vec![]);
        receipt.to = None;
        receipt.contract_address = Some(H160::from_low_u64_be(0x33));

        let result = ReceiptResult::from(receipt);

        assert_eq!(result.contract_address, Some(H160::from_low_u64_be(0x33)));
        assert_eq!(result.to, None);
    }

    #[test]
    fn receipt_result_serializes_to_the_expected_json() {
        let mut receipt = make_receipt(
            1,
            21_000,
            vec![make_log(
                H160::from_low_u64_be(0x11),
                H256::from_low_u64_be(0x1234),
                vec![0xde, 0xad],
            )],
        );
        receipt.logs_index = 0;

        let actual = serde_json::to_value(ReceiptResult::from(receipt)).unwrap();

        assert_eq!(
            actual,
            json!({
                "blockHash": "0x00000000000000000000000000000000000000000000000000000000000000bb",
                "blockNumber": "0x64",
                "contractAddress": null,
                "cumulativeGasUsed": "0x1f4",
                "effectiveGasPrice": "0x7",
                "from": "0x0000000000000000000000000000000000000011",
                "gasUsed": "0x5208",
                "logs": [
                    {
                        "address": "0x0000000000000000000000000000000000000011",
                        "topics": [
                            "0x0000000000000000000000000000000000000000000000000000000000001234"
                        ],
                        "data": "0xdead",
                        "blockNumber": "0x64",
                        "transactionHash": "0x00000000000000000000000000000000000000000000000000000000000000aa",
                        "transactionIndex": "0x3",
                        "blockHash": "0x00000000000000000000000000000000000000000000000000000000000000bb",
                        "logIndex": "0x0",
                        "removed": false
                    }
                ],
                "logsBloom": "0x00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
                "status": "0x1",
                "to": "0x0000000000000000000000000000000000000022",
                "transactionHash": "0x00000000000000000000000000000000000000000000000000000000000000aa",
                "transactionIndex": "0x3",
                "type": "0x2"
            })
        );
    }
}
