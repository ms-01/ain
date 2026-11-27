use ain_evm::filters::FilterResults;
use ethereum_types::{H160, H256};
use serde::{Serialize, Serializer};
use serde_with::{serde_as, OneOrMany};

use crate::{
    block::BlockNumber,
    logs::{LogRequestTopics, LogResult},
};

#[serde_as]
#[derive(Clone, Debug, Default, Eq, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct NewFilterRequest {
    #[serde_as(as = "Option<OneOrMany<_>>")]
    pub address: Option<Vec<H160>>,
    pub from_block: Option<BlockNumber>,
    pub to_block: Option<BlockNumber>,
    pub topics: Option<LogRequestTopics>,
}

#[derive(Debug, Deserialize, Clone, PartialEq)]
pub enum GetFilterChangesResult {
    Logs(Vec<LogResult>),
    NewBlock(Vec<H256>),
    NewPendingTransactions(Vec<H256>),
}

impl From<FilterResults> for GetFilterChangesResult {
    fn from(result: FilterResults) -> Self {
        match result {
            FilterResults::Logs(f) => {
                GetFilterChangesResult::Logs(f.into_iter().map(|log| log.into()).collect())
            }
            FilterResults::Blocks(f) => GetFilterChangesResult::NewBlock(f),
            FilterResults::Transactions(f) => GetFilterChangesResult::NewPendingTransactions(f),
        }
    }
}

impl Serialize for GetFilterChangesResult {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            GetFilterChangesResult::Logs(ref logs) => logs.serialize(serializer),
            GetFilterChangesResult::NewBlock(ref blocks) => blocks.serialize(serializer),
            GetFilterChangesResult::NewPendingTransactions(ref txs) => txs.serialize(serializer),
        }
    }
}

#[cfg(test)]
mod tests {
    use ain_evm::{filters::FilterResults, log::LogIndex};
    use ethereum_types::{H160, H256, U256};
    use serde_json::json;

    use super::{GetFilterChangesResult, NewFilterRequest};
    use crate::block::BlockNumber;

    #[test]
    fn new_filter_request_deserializes_single_and_multiple_addresses() {
        let single: NewFilterRequest = serde_json::from_value(json!({
            "address": "0x0000000000000000000000000000000000000011",
            "fromBlock": "earliest",
            "toBlock": "latest"
        }))
        .unwrap();
        assert_eq!(single.address, Some(vec![H160::from_low_u64_be(0x11)]));
        assert_eq!(single.from_block, Some(BlockNumber::Earliest));
        assert_eq!(single.to_block, Some(BlockNumber::Latest));

        let multi: NewFilterRequest = serde_json::from_value(json!({
            "address": [
                "0x0000000000000000000000000000000000000011",
                "0x0000000000000000000000000000000000000022"
            ]
        }))
        .unwrap();
        assert_eq!(
            multi.address,
            Some(vec![
                H160::from_low_u64_be(0x11),
                H160::from_low_u64_be(0x22)
            ])
        );

        assert!(serde_json::from_value::<NewFilterRequest>(json!({ "unknown": true })).is_err());
    }

    #[test]
    fn get_filter_changes_result_converts_and_serializes_all_variants() {
        let log_index = LogIndex {
            block_hash: H256::from_low_u64_be(0xbb),
            block_number: U256::from(10),
            topics: vec![H256::from_low_u64_be(0xaa)],
            data: vec![0x01],
            log_index: U256::zero(),
            address: H160::from_low_u64_be(0x11),
            removed: false,
            transaction_hash: H256::from_low_u64_be(0xcc),
            transaction_index: U256::zero(),
        };
        let logs = GetFilterChangesResult::from(FilterResults::Logs(vec![log_index]));
        let logs_json = serde_json::to_value(&logs).unwrap();
        assert!(logs_json.is_array());
        assert_eq!(logs_json.as_array().unwrap().len(), 1);

        let block_hash = H256::from_low_u64_be(0x1234);
        let blocks = GetFilterChangesResult::from(FilterResults::Blocks(vec![block_hash]));
        assert_eq!(
            serde_json::to_value(&blocks).unwrap(),
            json!([format!("{block_hash:#x}")])
        );

        let tx_hash = H256::from_low_u64_be(0x5678);
        let txs = GetFilterChangesResult::from(FilterResults::Transactions(vec![tx_hash]));
        assert_eq!(
            serde_json::to_value(&txs).unwrap(),
            json!([format!("{tx_hash:#x}")])
        );
    }
}
