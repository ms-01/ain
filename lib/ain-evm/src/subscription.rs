use std::collections::HashMap;

use ethereum_types::{H160, H256};
use tokio::sync::broadcast::{self, Sender};

use crate::{log::LogIndex, Result};

pub const NOTIFICATION_CHANNEL_BUFFER_SIZE: usize = 10_000;

#[derive(Clone)]
pub enum Notification {
    Block(H256),
    Transaction(H256),
    LogsRemoved(HashMap<H160, Vec<LogIndex>>),
}

pub struct SubscriptionService {
    pub tx: Sender<Notification>,
}

impl SubscriptionService {
    pub fn new() -> Self {
        let (tx, _rx) =
            broadcast::channel(ain_cpp_imports::get_evm_notification_channel_buffer_size());
        Self { tx }
    }

    pub fn send(&self, notification: Notification) -> Result<()> {
        // Do not need to handle send error since there may be no active receivers.
        let _ = self.tx.send(notification);
        Ok(())
    }
}

impl Default for SubscriptionService {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use ethereum_types::{H160, H256, U256};

    use super::{Notification, SubscriptionService, NOTIFICATION_CHANNEL_BUFFER_SIZE};
    use crate::log::LogIndex;

    #[test]
    fn subscription_delivers_removed_logs_notification() {
        let (tx, _rx) = tokio::sync::broadcast::channel(NOTIFICATION_CHANNEL_BUFFER_SIZE);
        let service = SubscriptionService { tx };
        let mut rx = service.tx.subscribe();

        let addr = H160::from_low_u64_be(42);
        let mut logs = HashMap::new();
        logs.insert(
            addr,
            vec![LogIndex {
                block_hash: H256::zero(),
                block_number: U256::from(5),
                topics: vec![],
                data: vec![],
                log_index: U256::zero(),
                address: addr,
                removed: true,
                transaction_hash: H256::zero(),
                transaction_index: U256::zero(),
            }],
        );

        service.send(Notification::LogsRemoved(logs)).unwrap();
        match rx.try_recv().unwrap() {
            Notification::LogsRemoved(received) => {
                let entries = received.get(&addr).unwrap();
                assert_eq!(entries.len(), 1);
                assert!(entries[0].removed);
            }
            _ => panic!("expected LogsRemoved notification"),
        }
    }
}
