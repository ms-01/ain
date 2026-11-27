use ethereum_types::U256;
use serde::{Serialize, Serializer};

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct SyncInfo {
    pub starting_block: U256,
    pub current_block: U256,
    pub highest_block: U256,
}

/// Sync state
#[derive(Debug, Deserialize, Clone)]
pub enum SyncState {
    /// Only hashes
    Synced(bool),
    /// Full transactions
    Syncing(SyncInfo),
}

impl Serialize for SyncState {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            SyncState::Synced(ref sync) => sync.serialize(serializer),
            SyncState::Syncing(ref sync_info) => sync_info.serialize(serializer),
        }
    }
}

#[cfg(test)]
mod tests {
    use ethereum_types::U256;
    use serde_json::json;

    use super::{SyncInfo, SyncState};

    #[test]
    fn sync_state_serializes_synced_and_syncing_variants() {
        assert_eq!(serde_json::to_value(SyncState::Synced(false)).unwrap(), json!(false));
        assert_eq!(serde_json::to_value(SyncState::Synced(true)).unwrap(), json!(true));

        let info = SyncInfo {
            starting_block: U256::from(1),
            current_block: U256::from(50),
            highest_block: U256::from(100),
        };
        assert_eq!(
            serde_json::to_value(SyncState::Syncing(info)).unwrap(),
            json!({
                "starting_block": "0x1",
                "current_block": "0x32",
                "highest_block": "0x64"
            })
        );
    }
}
