use ain_evm::{bytes::Bytes, log::LogIndex};
use ethereum_types::{H160, H256, U256};
use serde_with::{serde_as, OneOrMany};

use crate::block::BlockNumber;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct LogResult {
    // Consensus fields:
    // Address of the contract that generated the event
    pub address: H160,
    // List of topics provided by the contract
    pub topics: Vec<H256>,
    // Supplied by the contract, usually ABI-encoded
    pub data: Bytes,

    // Derived fields. These fields are filled in by the node
    // but not secured by consensus.
    // Block in which the transaction was included
    pub block_number: U256,
    // Hash of the transaction
    pub transaction_hash: H256,
    // Index of the transaction in the block
    pub transaction_index: U256,
    // Hash of the block in which the transaction was included
    pub block_hash: H256,
    // Index of the log in the block
    pub log_index: U256,

    // The removed field is true if this log was reverted due to a chain reorganization.
    // You must pay attention to this field if you receive logs through a filter query.
    pub removed: bool,
}

impl From<LogIndex> for LogResult {
    fn from(log: LogIndex) -> Self {
        Self {
            address: log.address,
            topics: log.topics,
            data: Bytes::from(log.data),
            block_number: log.block_number,
            transaction_hash: log.transaction_hash,
            transaction_index: log.transaction_index,
            block_hash: log.block_hash,
            log_index: log.log_index,
            removed: log.removed,
        }
    }
}

/// Call request
#[serde_as]
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct GetLogsRequest {
    #[serde_as(as = "Option<OneOrMany<_>>")]
    pub address: Option<Vec<H160>>,
    pub block_hash: Option<H256>,
    pub from_block: Option<BlockNumber>,
    pub to_block: Option<BlockNumber>,
    pub topics: Option<LogRequestTopics>,
}

#[derive(Clone, Debug, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(untagged)]
pub enum LogRequestTopics {
    VecOfHashes(Vec<Option<H256>>),
    VecOfHashVecs(Vec<Vec<Option<H256>>>),
}

impl LogRequestTopics {
    /// Convert the requested topics into the internal filter representation.
    ///
    /// The internal representation holds one entry per topic position, where an empty entry
    /// is the wildcard (any topic at this position).
    ///
    /// * A `null` in the flat form is a positional wildcard and keeps its position, so the
    ///   following positions are not shifted.
    /// * A `null` inside an OR-list turns the whole position into a wildcard. This follows
    ///   the Ethereum JSON-RPC specification (null matches any topic at that position) and
    ///   deviates from go-ethereum, which decodes a nested `null` into the zero hash.
    pub fn into_filter_topics(self) -> Vec<Vec<H256>> {
        match self {
            LogRequestTopics::VecOfHashes(inputs) => inputs
                .into_iter()
                .map(|hash| hash.map(|hash| vec![hash]).unwrap_or_default())
                .collect(),
            LogRequestTopics::VecOfHashVecs(inputs) => inputs
                .into_iter()
                .map(|hashes| {
                    if hashes.iter().any(|hash| hash.is_none()) {
                        // Any topic is accepted at this position.
                        Vec::new()
                    } else {
                        hashes.into_iter().flatten().collect()
                    }
                })
                .collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use ethereum_types::{H160, H256, U256};
    use serde_json::json;

    use ain_evm::log::LogIndex;

    use crate::logs::{GetLogsRequest, LogRequestTopics, LogResult};

    const TOPIC_A: &str = "0x00000000000000000000000000000000000000000000000000000000000000aa";
    const TOPIC_B: &str = "0x00000000000000000000000000000000000000000000000000000000000000bb";

    fn make_log_index() -> LogIndex {
        LogIndex {
            block_hash: H256::from_low_u64_be(0xbb),
            block_number: U256::from(100),
            topics: vec![H256::from_low_u64_be(0xaa)],
            data: vec![0xde, 0xad],
            log_index: U256::from(7),
            address: H160::from_low_u64_be(0x11),
            removed: true,
            transaction_hash: H256::from_low_u64_be(0xaa),
            transaction_index: U256::from(3),
        }
    }

    #[test]
    fn get_logs_request_deserializes_a_flat_topic_list() {
        let request: GetLogsRequest =
            serde_json::from_value(json!({ "topics": [TOPIC_A, null, TOPIC_B] })).unwrap();

        match request.topics {
            Some(LogRequestTopics::VecOfHashes(topics)) => {
                assert_eq!(topics.len(), 3);
                assert_eq!(topics[0], Some(H256::from_low_u64_be(0xaa)));
                assert_eq!(topics[1], None);
                assert_eq!(topics[2], Some(H256::from_low_u64_be(0xbb)));
            }
            _ => panic!("expected a flat topic list"),
        }
    }

    #[test]
    fn get_logs_request_deserializes_a_nested_topic_list() {
        let request: GetLogsRequest =
            serde_json::from_value(json!({ "topics": [[TOPIC_A, null], [TOPIC_B]] })).unwrap();

        match request.topics {
            Some(LogRequestTopics::VecOfHashVecs(topics)) => {
                assert_eq!(topics.len(), 2);
                assert_eq!(topics[0].len(), 2);
                assert_eq!(topics[0][0], Some(H256::from_low_u64_be(0xaa)));
                assert_eq!(topics[0][1], None);
                assert_eq!(topics[1], vec![Some(H256::from_low_u64_be(0xbb))]);
            }
            _ => panic!("expected a nested topic list"),
        }
    }

    #[test]
    fn log_result_converts_from_log_index() {
        let log_index = make_log_index();

        let result = LogResult::from(log_index.clone());

        assert_eq!(result.address, log_index.address);
        assert_eq!(result.topics, log_index.topics);
        assert_eq!(
            result.data,
            ain_evm::bytes::Bytes::from(log_index.data.clone())
        );
        assert_eq!(result.block_number, log_index.block_number);
        assert_eq!(result.transaction_hash, log_index.transaction_hash);
        assert_eq!(result.transaction_index, log_index.transaction_index);
        assert_eq!(result.block_hash, log_index.block_hash);
        assert_eq!(result.log_index, log_index.log_index);
        assert_eq!(result.removed, log_index.removed);

        assert_eq!(
            serde_json::to_value(result).unwrap(),
            json!({
                "address": "0x0000000000000000000000000000000000000011",
                "topics": [
                    "0x00000000000000000000000000000000000000000000000000000000000000aa"
                ],
                "data": "0xdead",
                "blockNumber": "0x64",
                "transactionHash": "0x00000000000000000000000000000000000000000000000000000000000000aa",
                "transactionIndex": "0x3",
                "blockHash": "0x00000000000000000000000000000000000000000000000000000000000000bb",
                "logIndex": "0x7",
                "removed": true
            })
        );
    }
}

#[cfg(test)]
mod filter_topics_tests {
    use ethereum_types::H256;
    use serde_json::json;

    use super::{GetLogsRequest, LogRequestTopics};

    fn t(v: u64) -> H256 {
        H256::from_low_u64_be(v)
    }

    #[test]
    fn flat_topics_keep_their_position() {
        let topics = LogRequestTopics::VecOfHashes(vec![Some(t(1)), None, Some(t(3))]);
        assert_eq!(
            topics.into_filter_topics(),
            vec![vec![t(1)], vec![], vec![t(3)]]
        );
        let topics = LogRequestTopics::VecOfHashes(vec![None]);
        assert_eq!(topics.into_filter_topics(), vec![Vec::<H256>::new()]);
        let topics = LogRequestTopics::VecOfHashes(vec![Some(t(1)), Some(t(2))]);
        assert_eq!(topics.into_filter_topics(), vec![vec![t(1)], vec![t(2)]]);
    }

    #[test]
    fn nested_topics_keep_their_position() {
        let topics =
            LogRequestTopics::VecOfHashVecs(vec![vec![Some(t(1)), None], vec![Some(t(3))]]);
        assert_eq!(topics.into_filter_topics(), vec![vec![], vec![t(3)]]);
        let topics = LogRequestTopics::VecOfHashVecs(vec![vec![Some(t(1))], vec![Some(t(3))]]);
        assert_eq!(topics.into_filter_topics(), vec![vec![t(1)], vec![t(3)]]);
        let topics = LogRequestTopics::VecOfHashVecs(vec![vec![]]);
        assert_eq!(topics.into_filter_topics(), vec![Vec::<H256>::new()]);
    }

    #[test]
    fn a_deserialized_flat_topic_list_keeps_its_position() {
        let request: GetLogsRequest =
            serde_json::from_value(json!({ "topics": [t(1), null, t(3)] })).unwrap();
        let topics = request.topics.expect("missing topics");
        assert_eq!(
            topics.into_filter_topics(),
            vec![vec![t(1)], vec![], vec![t(3)]]
        );
    }

    #[test]
    fn get_logs_request_handles_address_variants_and_rejects_unknown_fields() {
        let single: GetLogsRequest = serde_json::from_value(json!({
            "address": "0x0000000000000000000000000000000000000011",
            "blockHash": "0x00000000000000000000000000000000000000000000000000000000000000bb"
        }))
        .unwrap();
        assert_eq!(single.address.unwrap().len(), 1);
        assert!(single.block_hash.is_some());

        let multi: GetLogsRequest = serde_json::from_value(json!({
            "address": [
                "0x0000000000000000000000000000000000000011",
                "0x0000000000000000000000000000000000000022"
            ],
            "fromBlock": "0x1",
            "toBlock": "latest"
        }))
        .unwrap();
        assert_eq!(multi.address.unwrap().len(), 2);

        // Unknown field rejected by deny_unknown_fields
        assert!(serde_json::from_value::<GetLogsRequest>(json!({ "badField": 123 })).is_err());

        // Malformed address rejected
        assert!(serde_json::from_value::<GetLogsRequest>(json!({ "address": "not-an-address" })).is_err());

        // Empty and multi-OR nested topics
        let empty = LogRequestTopics::VecOfHashes(vec![]);
        assert!(empty.into_filter_topics().is_empty());

        let multi_or = LogRequestTopics::VecOfHashVecs(vec![
            vec![Some(t(1)), Some(t(2))],
            vec![Some(t(3))],
        ]);
        assert_eq!(
            multi_or.into_filter_topics(),
            vec![vec![t(1), t(2)], vec![t(3)]]
        );
    }
}
