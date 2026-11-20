use std::sync::Arc;

use ain_evm::{
    evm::EVMServices,
    filters::{FilterCriteria, FilterService},
    storage::traits::BlockStorage,
    subscription::Notification,
};
use anyhow::format_err;
use ethereum_types::U256;
use jsonrpsee::{
    core::{async_trait, SubscriptionResult},
    proc_macros::rpc,
    types::ErrorObjectOwned,
    PendingSubscriptionSink, SubscriptionMessage,
};
use log::{debug, trace};
use tokio::runtime::Handle as AsyncHandle;

use crate::{
    errors::RPCError,
    subscription::{PubSubResult, Subscription, SubscriptionParams, SyncStatus},
};

/// Builds the filter criteria of a log subscription from the subscription params.
///
/// The topic filter is converted with the very same helper as on the
/// `eth_getLogs` / `eth_newFilter` path, so that both RPC surfaces interpret the
/// payload identically (incl. the `null` placeholder of a positional wildcard).
fn log_criteria(params: Option<&SubscriptionParams>) -> FilterCriteria {
    params
        .map(|p| {
            let topics = p.topics.clone().map(|topics| topics.into_filter_topics());
            FilterCriteria {
                addresses: p.address.clone(),
                topics,
                ..Default::default()
            }
        })
        .unwrap_or_default()
}

/// Metachain WebSockets interface.
#[rpc(server, namespace = "eth")]
pub trait MetachainPubSub {
    /// Subscribe to Eth subscription.
    #[subscription(
    name = "subscribe" => "subscription",
    unsubscribe = "unsubscribe",
    item = Result,
    )]
    async fn subscribe(
        &self,
        subscription: Subscription,
        params: Option<SubscriptionParams>,
    ) -> SubscriptionResult;
}

pub struct MetachainPubSubModule {
    handler: Arc<EVMServices>,
    tokio_runtime: AsyncHandle,
}

impl MetachainPubSubModule {
    #[must_use]
    pub fn new(handler: Arc<EVMServices>, tokio_runtime: AsyncHandle) -> Self {
        Self {
            handler,
            tokio_runtime,
        }
    }
}

#[async_trait]
impl MetachainPubSubServer for MetachainPubSubModule {
    async fn subscribe(
        &self,
        pending: PendingSubscriptionSink,
        subscription: Subscription,
        params: Option<SubscriptionParams>,
    ) -> SubscriptionResult {
        // The filter criteria of a log subscription are validated before the
        // subscription is accepted, so that no subscription id is handed out and no
        // log stream is started for an invalid filter. The validation is independent
        // of the block range (a subscription carries no range) and uses the same
        // limit as `eth_getLogs` / `eth_newFilter`, i.e. the very same error object
        // is returned for the very same payload.
        if let Subscription::Logs = &subscription {
            if let Err(e) = log_criteria(params.as_ref()).verify_topics() {
                pending
                    .reject(ErrorObjectOwned::from(RPCError::EvmError(e)))
                    .await;
                return Ok(());
            }
        }
        let sink = pending.accept().await?;
        trace!(target: "pubsub", "subscribing to {:#?}", subscription);
        trace!(target: "pubsub", "params {:?}", params);

        let mut rx = self.handler.subscriptions.tx.subscribe();
        let handler = self.handler.clone();
        match subscription {
            Subscription::NewHeads => {
                let fut = async move {
                    while !sink.is_closed() {
                        if let Notification::Block(hash) = rx.recv().await? {
                            if let Some(block) = handler.storage.get_block_by_hash(&hash)? {
                                if sink
                                    .send(SubscriptionMessage::from_json(&PubSubResult::Header(
                                        Box::new(block.header.into()),
                                    ))?)
                                    .await
                                    .is_err()
                                {
                                    break;
                                }
                            } else {
                                return Err(format_err!(
                                    "failed to retrieve block from storage with block hash: {:x?}",
                                    hash
                                ));
                            }
                        }
                    }
                    debug!("Ws connection ended, thread closing");
                    Ok::<(), anyhow::Error>(())
                };
                self.tokio_runtime.spawn(fut);
            }
            Subscription::Logs => {
                let criteria = log_criteria(params.as_ref());
                let fut = async move {
                    while !sink.is_closed() {
                        let logs = match rx.recv().await? {
                            Notification::Block(hash) => {
                                if let Some(block) = handler.storage.get_block_by_hash(&hash)? {
                                    handler
                                        .filters
                                        .get_block_logs(&criteria, block.header.number)?
                                } else {
                                    return Err(format_err!(
                                        "failed to retrieve block from storage with block hash: {:x?}",
                                        hash
                                    ));
                                }
                            }
                            Notification::LogsRemoved(logs) => {
                                FilterService::filter_logs(&criteria, logs)
                            }
                            Notification::Transaction(_) => continue,
                        };
                        let mut disconnect = false;
                        for log in logs {
                            if sink
                                .send(SubscriptionMessage::from_json(&PubSubResult::Log(
                                    Box::new(log.into()),
                                ))?)
                                .await
                                .is_err()
                            {
                                disconnect = true;
                                break;
                            }
                        }
                        if disconnect {
                            break;
                        }
                    }
                    debug!("Ws connection ended, thread closing");
                    Ok::<(), anyhow::Error>(())
                };
                self.tokio_runtime.spawn(fut);
            }
            Subscription::NewPendingTransactions => {
                let fut = async move {
                    while !sink.is_closed() {
                        if let Notification::Transaction(hash) = rx.recv().await? {
                            if sink
                                .send(SubscriptionMessage::from_json(
                                    &PubSubResult::TransactionHash(hash),
                                )?)
                                .await
                                .is_err()
                            {
                                break;
                            }
                        }
                    }
                    debug!("Ws connection ended, thread closing");
                    Ok::<(), anyhow::Error>(())
                };
                self.tokio_runtime.spawn(fut);
            }
            Subscription::Syncing => {
                let fut = async move {
                    let get_sync_status = || -> Result<SyncStatus, anyhow::Error> {
                        let (current, highest) = ain_cpp_imports::get_sync_status()
                            .map_err(|_| format_err!("failed to get sync status"))?;
                        let current_block = handler
                            .storage
                            .get_latest_block()?
                            .ok_or(format_err!("Unable to find latest block"))?
                            .header
                            .number;
                        let diff = U256::from(highest.saturating_sub(current));
                        let highest_block = current_block.saturating_add(diff);
                        Ok(SyncStatus {
                            syncing: current != highest,
                            starting_block: handler.block.get_starting_block_number(),
                            current_block,
                            highest_block,
                        })
                    };
                    let mut last_sync_status = get_sync_status()?;
                    if !last_sync_status.syncing {
                        // Node is at tip. Return false flag once and exit thread
                        let _ = sink.send(SubscriptionMessage::from_json(&false)?).await;
                        return Ok::<(), anyhow::Error>(());
                    } else {
                        sink.send(SubscriptionMessage::from_json(&PubSubResult::SyncState(
                            last_sync_status.clone(),
                        ))?)
                        .await
                        .map_err(|_| format_err!("send failed, sink closed"))?;
                    }

                    while !sink.is_closed() {
                        if let Notification::Block(_) = rx.recv().await? {
                            let sync_status = get_sync_status()?;
                            if sync_status.current_block != last_sync_status.current_block {
                                if !sync_status.syncing {
                                    // Node sync-ed to tip. Return false flag once and exit thread
                                    let _ =
                                        sink.send(SubscriptionMessage::from_json(&false)?).await;
                                    break;
                                }
                                if sink
                                    .send(SubscriptionMessage::from_json(
                                        &PubSubResult::SyncState(sync_status.clone()),
                                    )?)
                                    .await
                                    .is_err()
                                {
                                    break;
                                }
                            }
                            last_sync_status = sync_status;
                        }
                    }
                    debug!("Ws connection ended, thread closing");
                    Ok::<(), anyhow::Error>(())
                };
                self.tokio_runtime.spawn(fut);
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use jsonrpsee::types::ErrorObjectOwned;
    use serde_json::json;

    use super::{log_criteria, FilterCriteria};
    use crate::errors::RPCError;
    use crate::subscription::SubscriptionParams;

    const TOPIC_A: &str = "0x00000000000000000000000000000000000000000000000000000000000000aa";
    const TOPIC_B: &str = "0x00000000000000000000000000000000000000000000000000000000000000bb";
    const TOPIC_C: &str = "0x00000000000000000000000000000000000000000000000000000000000000cc";
    const TOPIC_D: &str = "0x00000000000000000000000000000000000000000000000000000000000000dd";
    const TOPIC_E: &str = "0x00000000000000000000000000000000000000000000000000000000000000ee";

    fn criteria(value: serde_json::Value) -> FilterCriteria {
        let params: SubscriptionParams = serde_json::from_value(value).unwrap();
        log_criteria(Some(&params))
    }

    /// A subscription without any topic filter is valid.
    #[test]
    fn subscription_without_topics_is_accepted() {
        let criteria = criteria(json!({"address": null}));
        criteria.verify_topics().unwrap();
    }

    /// Exactly four topic positions are valid.
    #[test]
    fn subscription_with_four_topic_positions_is_accepted() {
        let criteria = criteria(json!({"topics": [TOPIC_A, TOPIC_B, TOPIC_C, TOPIC_D]}));
        criteria.verify_topics().unwrap();
        assert_eq!(criteria.topics.as_ref().unwrap().len(), 4);
    }

    /// Five topic positions in the flat form are rejected.
    #[test]
    fn subscription_with_five_flat_topic_positions_is_rejected() {
        let criteria = criteria(json!({"topics": [TOPIC_A, TOPIC_B, TOPIC_C, TOPIC_D, TOPIC_E]}));
        let err = criteria.verify_topics().unwrap_err();
        assert!(
            err.to_string().contains("exceed max topics"),
            "unexpected error: {err}"
        );
    }

    /// Five topic positions in the nested form are rejected as well.
    #[test]
    fn subscription_with_five_nested_topic_positions_is_rejected() {
        let criteria =
            criteria(json!({"topics": [[TOPIC_A], [TOPIC_B], [TOPIC_C], [TOPIC_D], [TOPIC_E]]}));
        let err = criteria.verify_topics().unwrap_err();
        assert!(
            err.to_string().contains("exceed max topics"),
            "unexpected error: {err}"
        );
    }

    /// The `null` placeholder keeps its position, so five entries are five positions.
    #[test]
    fn subscription_with_a_null_placeholder_counts_the_position() {
        let accepted = criteria(json!({"topics": [TOPIC_A, null, TOPIC_B]}));
        accepted.verify_topics().unwrap();
        assert_eq!(accepted.topics.as_ref().unwrap().len(), 3);

        let rejected = criteria(json!({"topics": [TOPIC_A, null, TOPIC_B, TOPIC_C, TOPIC_D]}));
        let err = rejected.verify_topics().unwrap_err();
        assert!(
            err.to_string().contains("exceed max topics"),
            "unexpected error: {err}"
        );
    }

    /// The rejected subscription carries the same error object as `eth_newFilter` /
    /// `eth_getLogs` for the same payload.
    #[test]
    fn rejected_subscription_carries_the_new_filter_error_object() {
        let err = criteria(json!({"topics": [TOPIC_A, TOPIC_B, TOPIC_C, TOPIC_D, TOPIC_E]}))
            .verify_topics()
            .unwrap_err();
        let error = ErrorObjectOwned::from(RPCError::EvmError(err));
        assert_eq!(error.code(), -32001);
        assert_eq!(
            error.message(),
            "Custom error: error calling EVM : \"exceed max topics\""
        );
    }
}
