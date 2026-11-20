pub mod eth;

use ethereum_types::{H160, H256, U256};
use jsonrpsee::core::traits::IdProvider;
use serde::{Deserialize, Serialize, Serializer};
use serde_with::{serde_as, OneOrMany};

use crate::{
    block::RpcBlockHeader,
    logs::{LogRequestTopics, LogResult},
};

#[derive(Clone, Debug, Default, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncStatus {
    pub syncing: bool,
    pub starting_block: U256,
    pub current_block: U256,
    pub highest_block: U256,
}

/// Subscription kind.
#[derive(Clone, Debug, Eq, PartialEq, Hash, Deserialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub enum Subscription {
    /// New block headers subscription.
    NewHeads,
    /// Logs subscription.
    Logs,
    /// New Pending Transactions subscription.
    NewPendingTransactions,
    /// Node syncing status subscription.
    Syncing,
}

#[serde_as]
#[derive(Clone, Debug, Eq, PartialEq, Hash, Deserialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct SubscriptionParams {
    #[serde_as(as = "Option<OneOrMany<_>>")]
    pub address: Option<Vec<H160>>,
    /// Topic filter of the subscription.
    ///
    /// This is the very same type as the one used by `eth_getLogs` / `eth_newFilter`
    /// (`LogRequestTopics`), so the conversion into the internal filter representation
    /// is shared and cannot drift apart between the RPC surfaces.
    pub topics: Option<LogRequestTopics>,
}

/// Subscription result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PubSubResult {
    /// New block header.
    Header(Box<RpcBlockHeader>),
    /// Log
    Log(Box<LogResult>),
    /// Transaction hash
    TransactionHash(H256),
    /// SyncStatus
    SyncState(SyncStatus),
}

impl Serialize for PubSubResult {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            PubSubResult::Header(ref header) => header.serialize(serializer),
            PubSubResult::Log(ref log) => log.serialize(serializer),
            PubSubResult::TransactionHash(ref hash) => hash.serialize(serializer),
            PubSubResult::SyncState(ref sync) => sync.serialize(serializer),
        }
    }
}

#[derive(Debug, Default)]
pub struct MetachainSubIdProvider;

impl IdProvider for MetachainSubIdProvider {
    fn next_id(&self) -> jsonrpsee::types::SubscriptionId<'static> {
        format!("0x{}", hex::encode(rand::random::<u128>().to_le_bytes())).into()
    }
}

#[cfg(test)]
mod filter_topics_tests {
    use std::str::FromStr;

    use ethereum_types::H256;
    use serde_json::json;

    use crate::subscription::SubscriptionParams;

    const TOPIC_A: &str = "0x00000000000000000000000000000000000000000000000000000000000000aa";
    const TOPIC_B: &str = "0x00000000000000000000000000000000000000000000000000000000000000bb";
    const TOPIC_C: &str = "0x00000000000000000000000000000000000000000000000000000000000000cc";

    fn topic(hash: &str) -> H256 {
        H256::from_str(hash).unwrap()
    }

    fn filter_topics(value: serde_json::Value) -> Vec<Vec<H256>> {
        let params: SubscriptionParams = serde_json::from_value(value).unwrap();
        params
            .topics
            .expect("topics are expected")
            .into_filter_topics()
    }

    /// The flat form keeps the position of a `null` placeholder (positional wildcard).
    #[test]
    fn flat_form_keeps_null_placeholder_position() {
        assert_eq!(
            filter_topics(json!({"topics": [TOPIC_A, null, TOPIC_B]})),
            vec![vec![topic(TOPIC_A)], vec![], vec![topic(TOPIC_B)]]
        );
    }

    /// A single `null` in the flat form is a wildcard on the only position.
    #[test]
    fn flat_form_with_only_null_is_wildcard() {
        assert_eq!(filter_topics(json!({"topics": [null]})), vec![vec![]]);
    }

    /// The flat form without any placeholder is unchanged.
    #[test]
    fn flat_form_without_placeholder_is_unchanged() {
        assert_eq!(
            filter_topics(json!({"topics": [TOPIC_A, TOPIC_B]})),
            vec![vec![topic(TOPIC_A)], vec![topic(TOPIC_B)]]
        );
    }

    /// A `null` inside an OR-list turns the whole position into a wildcard.
    #[test]
    fn nested_form_with_null_placeholder_is_wildcard() {
        assert_eq!(
            filter_topics(json!({"topics": [[TOPIC_A, null], [TOPIC_B]]})),
            vec![vec![], vec![topic(TOPIC_B)]]
        );
    }

    /// The nested form without any placeholder is unchanged.
    #[test]
    fn nested_form_without_placeholder_is_unchanged() {
        assert_eq!(
            filter_topics(json!({"topics": [[TOPIC_A], [TOPIC_B]]})),
            vec![vec![topic(TOPIC_A)], vec![topic(TOPIC_B)]]
        );
    }

    /// An empty OR-list is a wildcard on that position.
    #[test]
    fn nested_form_with_empty_list_is_wildcard() {
        assert_eq!(filter_topics(json!({"topics": [[]]})), vec![vec![]]);
    }

    /// The deserialization of the subscription params keeps the `null` placeholders, the
    /// wire format is not changed by the unification with `LogRequestTopics`.
    #[test]
    fn subscription_params_deserialization_keeps_placeholders() {
        let flat: SubscriptionParams = serde_json::from_value(json!({
            "address": null,
            "topics": [TOPIC_A, null, TOPIC_B],
        }))
        .unwrap();
        assert_eq!(
            flat.topics.unwrap().into_filter_topics(),
            vec![vec![topic(TOPIC_A)], vec![], vec![topic(TOPIC_B)]]
        );

        let nested: SubscriptionParams = serde_json::from_value(json!({
            "address": null,
            "topics": [[TOPIC_A, null], [TOPIC_B]],
        }))
        .unwrap();
        assert_eq!(
            nested.topics.unwrap().into_filter_topics(),
            vec![vec![], vec![topic(TOPIC_B)]]
        );
    }

    /// A subscription without any topics filter keeps all logs.
    #[test]
    fn subscription_params_without_topics() {
        let params: SubscriptionParams = serde_json::from_value(json!({"address": null})).unwrap();
        assert!(params.topics.is_none());
    }

    /// The topic filter of the subscription is the very same type as the one of
    /// `eth_getLogs` / `eth_newFilter`.
    #[test]
    fn subscription_topics_are_shared_with_get_logs() {
        let topics = crate::logs::LogRequestTopics::VecOfHashes(vec![
            Some(topic(TOPIC_A)),
            None,
            Some(topic(TOPIC_C)),
        ]);
        assert_eq!(
            topics.into_filter_topics(),
            vec![vec![topic(TOPIC_A)], vec![], vec![topic(TOPIC_C)]]
        );
    }
}
