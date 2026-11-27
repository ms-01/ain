#[macro_use]
mod macros;

mod ocean_store;

use std::sync::Arc;

use ain_db::{Column, ColumnName, DBError, LedgerColumn, Result as DBResult, TypedColumn};
use bitcoin::{hashes::Hash, BlockHash, Txid};
pub use ocean_store::OceanStore;
use rocksdb::Direction;
use snafu::OptionExt;

use crate::{define_table, error::SecondaryIndexSnafu, model, Result};

#[derive(Debug, PartialEq, Eq, Clone)]
pub enum SortOrder {
    Ascending,
    Descending,
}

impl From<SortOrder> for Direction {
    fn from(sort_order: SortOrder) -> Self {
        match sort_order {
            SortOrder::Ascending => Self::Forward,
            SortOrder::Descending => Self::Reverse,
        }
    }
}

pub trait RepositoryOps<K, V> {
    type ListItem;
    fn get(&self, key: &K) -> Result<Option<V>>;
    fn put(&self, key: &K, value: &V) -> Result<()>;
    fn delete(&self, key: &K) -> Result<()>;
    fn list<'a>(
        &'a self,
        from: Option<K>,
        direction: SortOrder,
    ) -> Result<Box<dyn Iterator<Item = Self::ListItem> + 'a>>;
}

pub trait InitialKeyProvider<K, V>: RepositoryOps<K, V> {
    type PartialKey;
    fn initial_key(pk: Self::PartialKey) -> K;
}

pub trait SecondaryIndex<K, V>: RepositoryOps<K, V> {
    type Value;
    fn retrieve_primary_value(&self, el: Self::ListItem) -> Result<Self::Value>;
}

define_table! {
    #[derive(Debug)]
    pub struct Block {
        key_type = BlockHash,
        value_type = model::Block,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct BlockByHeight {
        key_type = u32,
        value_type = BlockHash,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                Ok(index.to_be_bytes().to_vec())
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 4 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut array = [0u8; 4];
                array.copy_from_slice(&raw_key);
                Ok(u32::from_be_bytes(array))
            }
        },
    },
    SecondaryIndex = Block
}

define_table! {
    #[derive(Debug)]
    pub struct Masternode {
        key_type = Txid,
        value_type = model::Masternode,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct MasternodeByHeight {
        key_type = (u32, Txid),
        value_type = u8,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (height, txid) = index;
                let mut vec = height.to_be_bytes().to_vec();
                vec.extend_from_slice(txid.as_byte_array().to_vec().as_ref());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 36 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut height_array = [0u8; 4];
                height_array.copy_from_slice(&raw_key[..4]);
                let mut txid_array = [0u8; 32];
                txid_array.copy_from_slice(&raw_key[4..]);

                let height = u32::from_be_bytes(height_array);
                let txid = Txid::from_byte_array(txid_array);
                Ok((height, txid))
            }
        },
    }
}

impl SecondaryIndex<(u32, Txid), u8> for MasternodeByHeight {
    type Value = model::Masternode;

    fn retrieve_primary_value(&self, el: Self::ListItem) -> Result<Self::Value> {
        let ((_, id), _) = el?;
        let col = self.store.column::<Masternode>();
        let tx = col.get(&id)?.context(SecondaryIndexSnafu)?;
        Ok(tx)
    }
}

define_table! {
    #[derive(Debug)]
    pub struct MasternodeStats {
        key_type = u32,
        value_type = model::MasternodeStats,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                Ok(index.to_be_bytes().to_vec())
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 4 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut array = [0u8; 4];
                array.copy_from_slice(&raw_key);
                Ok(u32::from_be_bytes(array))
            }
        },
    }
}

impl MasternodeStats {
    pub fn get_latest(&self) -> Result<Option<model::MasternodeStats>> {
        match self.col.iter(None, SortOrder::Descending.into())?.next() {
            None => Ok(None),
            Some(Ok((_, id))) => Ok(Some(id)),
            Some(Err(e)) => Err(e.into()),
        }
    }
}

define_table! {
    #[derive(Debug)]
    pub struct Oracle {
        key_type = Txid,
        value_type = model::Oracle,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OracleHistory {
        key_type = model::OracleHistoryId,
        value_type = model::Oracle,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (txid, height) = index; // txid, u32
                let mut vec = txid.as_byte_array().to_vec();
                vec.extend_from_slice(&height.to_be_bytes());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 36 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut txid_array = [0u8; 32];
                txid_array.copy_from_slice(&raw_key[..32]);
                let mut height_array = [0u8; 4];
                height_array.copy_from_slice(&raw_key[32..]);

                let txid = Txid::from_byte_array(txid_array);
                let height = u32::from_be_bytes(height_array);
                Ok((txid, height))
            }
        },
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OraclePriceActive {
        key_type = model::OraclePriceActiveId,
        value_type = model::OraclePriceActive,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OraclePriceAggregated {
        key_type = model::OraclePriceAggregatedId,
        value_type = model::OraclePriceAggregated,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OraclePriceAggregatedInterval {
        key_type = model::OraclePriceAggregatedIntervalId,
        value_type = model::OraclePriceAggregatedInterval,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OraclePriceFeed {
        key_type = model::OraclePriceFeedId,
        value_type = model::OraclePriceFeed,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct OracleTokenCurrency {
        key_type = model::OracleTokenCurrencyId,
        value_type = model::OracleTokenCurrency,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct PoolSwap {
        key_type = model::PoolSwapKey,
        value_type = model::PoolSwap,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (pool_id, height, txno) = index; // u32, u32, usize
                let mut vec = Vec::with_capacity(16);
                vec.extend_from_slice(&pool_id.to_be_bytes());
                vec.extend_from_slice(&height.to_be_bytes());
                vec.extend_from_slice(&txno.to_be_bytes());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 16 {
                    return Err(DBError::WrongKeyLength);
                }
                let pool_id = u32::from_be_bytes(
                    raw_key[0..4]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let height = u32::from_be_bytes(
                    raw_key[4..8]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let txno = usize::from_be_bytes(
                    raw_key[8..]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );

                Ok((pool_id, height, txno))
            }

        },
    },
    InitialKeyProvider = |pk: u32| (pk, u32::MAX, usize::MAX)
}

define_table! {
    #[derive(Debug)]
    pub struct PoolSwapAggregated {
        key_type = model::PoolSwapAggregatedId,
        value_type = model::PoolSwapAggregated,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (pool_id, interval, hash) = index; // u32, u32, hash
                let mut vec = Vec::with_capacity(40);
                vec.extend_from_slice(&pool_id.to_be_bytes());
                vec.extend_from_slice(&interval.to_be_bytes());
                vec.extend_from_slice(hash.as_byte_array().to_vec().as_ref());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 40 {
                    return Err(DBError::WrongKeyLength);
                }
                let pool_id = u32::from_be_bytes(
                    raw_key[0..4]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let interval = u32::from_be_bytes(
                    raw_key[4..8]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let mut hash_array = [0u8; 32];
                hash_array.copy_from_slice(&raw_key[..32]);
                let hash = BlockHash::from_byte_array(hash_array);

                Ok((pool_id, interval, hash))
                }
            },
    }
}

define_table! {
    #[derive(Debug)]
    pub struct PoolSwapAggregatedKey {
        key_type = model::PoolSwapAggregatedKey,
        value_type = model::PoolSwapAggregatedId,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (pool_id, interval, bucket) = index;
                let mut vec = Vec::with_capacity(16);
                vec.extend_from_slice(&pool_id.to_be_bytes());
                vec.extend_from_slice(&interval.to_be_bytes());
                vec.extend_from_slice(&bucket.to_be_bytes());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 16 {
                    return Err(DBError::WrongKeyLength);
                }
                let pool_id = u32::from_be_bytes(
                    raw_key[0..4]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let interval = u32::from_be_bytes(
                    raw_key[4..8]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );
                let bucket = i64::from_be_bytes(
                    raw_key[8..16]
                        .try_into()
                        .map_err(|_| DBError::WrongKeyLength)?,
                );

                Ok((pool_id, interval, bucket))
                }
            },
    },
    SecondaryIndex = PoolSwapAggregated
}

define_table! {
    #[derive(Debug)]
    pub struct PriceTicker {
        key_type = model::PriceTickerId,
        value_type = model::PriceTicker,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct PriceTickerKey {
        key_type = model::PriceTickerKey,
        value_type = model::PriceTickerId,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct RawBlock {
        key_type = BlockHash,
        value_type = String,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct ScriptActivity {
        key_type = model::ScriptActivityId,
        value_type = model::ScriptActivity,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct ScriptAggregation {
        key_type = model::ScriptAggregationId,
        value_type = model::ScriptAggregation,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct ScriptUnspent {
        key_type = model::ScriptUnspentId,
        value_type = model::ScriptUnspent,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct ScriptUnspentKey {
        key_type = model::ScriptUnspentKey,
        value_type = model::ScriptUnspentId,
    },
    SecondaryIndex = ScriptUnspent
}

define_table! {
    #[derive(Debug)]
    pub struct Transaction {
        key_type = Txid,
        value_type = model::Transaction,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct TransactionByBlockHash {
        key_type = model::TransactionByBlockHashKey,
        value_type = Txid,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (hash, txno) = index;
                let mut vec = hash.as_byte_array().to_vec();
                vec.extend_from_slice(&txno.to_be_bytes());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 40 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut hash_array = [0u8; 32];
                hash_array.copy_from_slice(&raw_key[..32]);
                let mut txno_array = [0u8; 8];
                txno_array.copy_from_slice(&raw_key[32..]);

                let hash = BlockHash::from_byte_array(hash_array);
                let txno = usize::from_be_bytes(txno_array);
                Ok((hash, txno))
            }
        },
    },
    SecondaryIndex = Transaction,
    InitialKeyProvider = |pk: BlockHash| (pk, 0)
}

define_table! {
    #[derive(Debug)]
    pub struct TransactionVin {
        key_type = String,
        value_type = model::TransactionVin,
    },
    InitialKeyProvider = |pk: Txid| format!("{}00", pk)
}

define_table! {
    #[derive(Debug)]
    pub struct TransactionVout {
        key_type = model::TransactionVoutKey,
        value_type = model::TransactionVout,
        custom_key = {
            fn key(index: &Self::Index) -> DBResult<Vec<u8>> {
                let (txid, txno) = index;
                let mut vec = txid.as_byte_array().to_vec();
                vec.extend_from_slice(&txno.to_be_bytes());
                Ok(vec)
            }

            fn get_key(raw_key: Box<[u8]>) -> DBResult<Self::Index> {
                if raw_key.len() != 40 {
                    return Err(DBError::WrongKeyLength);
                }
                let mut hash_array = [0u8; 32];
                hash_array.copy_from_slice(&raw_key[..32]);
                let mut txno_array = [0u8; 8];
                txno_array.copy_from_slice(&raw_key[32..]);

                let txid = Txid::from_byte_array(hash_array);
                let txno = usize::from_be_bytes(txno_array);
                Ok((txid, txno))
            }
        },
    }
}

define_table! {
    #[derive(Debug)]
    pub struct TxResult {
        key_type = Txid,
        value_type = model::TxResult,
    }
}

define_table! {
    #[derive(Debug)]
    pub struct VaultAuctionHistory {
        key_type = model::AuctionHistoryKey,
        value_type = model::VaultAuctionBatchHistory,
    }
}

pub const COLUMN_NAMES: [&str; 28] = [
    Block::NAME,
    BlockByHeight::NAME,
    MasternodeStats::NAME,
    Masternode::NAME,
    MasternodeByHeight::NAME,
    Oracle::NAME,
    OracleHistory::NAME,
    OraclePriceActive::NAME,
    OraclePriceAggregated::NAME,
    OraclePriceAggregatedInterval::NAME,
    OraclePriceFeed::NAME,
    OracleTokenCurrency::NAME,
    PoolSwapAggregated::NAME,
    PoolSwapAggregatedKey::NAME,
    PoolSwap::NAME,
    PriceTicker::NAME,
    PriceTickerKey::NAME,
    RawBlock::NAME,
    ScriptActivity::NAME,
    ScriptAggregation::NAME,
    ScriptUnspent::NAME,
    ScriptUnspentKey::NAME,
    Transaction::NAME,
    TransactionByBlockHash::NAME,
    TransactionVin::NAME,
    TransactionVout::NAME,
    TxResult::NAME,
    VaultAuctionHistory::NAME,
];

#[cfg(test)]
mod tests {
    use std::{
        fs,
        path::PathBuf,
        sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        },
    };

    use ain_db::{Column, DBError};
    use bitcoin::{hashes::Hash, BlockHash, Txid};
    use rocksdb::Direction;
    use rust_decimal_macros::dec;

    use super::{
        BlockByHeight, InitialKeyProvider, MasternodeByHeight, MasternodeStats, OceanStore,
        OracleHistory, PoolSwap, PoolSwapAggregated, PoolSwapAggregatedKey, RepositoryOps,
        SecondaryIndex, SortOrder, TransactionByBlockHash, TransactionVin, TransactionVout,
    };
    use crate::{model, Services};

    static COUNTER: AtomicUsize = AtomicUsize::new(0);

    struct TempDir(PathBuf);

    impl TempDir {
        fn new(prefix: &str) -> Self {
            let id = COUNTER.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!(
                "ain-ocean-test-{prefix}-{}-{id}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn make_block(height: u32, hash_byte: u8) -> model::Block {
        let hash = BlockHash::from_byte_array([hash_byte; 32]);
        model::Block {
            hash,
            id: hash,
            previous_hash: None,
            height,
            version: 1,
            time: i64::from(height) * 30,
            median_time: i64::from(height) * 30,
            transaction_count: 1,
            difficulty: 1.0,
            masternode: None,
            minter: None,
            minter_block_count: 0,
            stake_modifier: String::new(),
            merkleroot: String::new(),
            size: 100,
            size_stripped: 100,
            weight: 400,
        }
    }

    #[test]
    fn test_sort_order_to_direction() {
        assert!(matches!(
            Direction::from(SortOrder::Ascending),
            Direction::Forward
        ));
        assert!(matches!(
            Direction::from(SortOrder::Descending),
            Direction::Reverse
        ));
    }

    #[test]
    fn test_custom_key_codecs_and_errors() {
        // BlockByHeight
        let raw = BlockByHeight::key(&42).unwrap();
        assert_eq!(BlockByHeight::get_key(raw.into_boxed_slice()).unwrap(), 42);
        assert!(matches!(
            BlockByHeight::get_key(vec![0u8; 2].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // MasternodeByHeight
        let txid = Txid::from_byte_array([3u8; 32]);
        let raw = MasternodeByHeight::key(&(100, txid)).unwrap();
        assert_eq!(
            MasternodeByHeight::get_key(raw.into_boxed_slice()).unwrap(),
            (100, txid)
        );
        assert!(matches!(
            MasternodeByHeight::get_key(vec![0u8; 10].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // MasternodeStats
        let raw = MasternodeStats::key(&7).unwrap();
        assert_eq!(MasternodeStats::get_key(raw.into_boxed_slice()).unwrap(), 7);
        assert!(matches!(
            MasternodeStats::get_key(vec![0u8; 3].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // OracleHistory
        let raw = OracleHistory::key(&(txid, 55)).unwrap();
        assert_eq!(
            OracleHistory::get_key(raw.into_boxed_slice()).unwrap(),
            (txid, 55)
        );
        assert!(matches!(
            OracleHistory::get_key(vec![0u8; 5].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // PoolSwap
        let raw = PoolSwap::key(&(1, 200, 3)).unwrap();
        assert_eq!(
            PoolSwap::get_key(raw.into_boxed_slice()).unwrap(),
            (1, 200, 3)
        );
        assert_eq!(PoolSwap::initial_key(5), (5, u32::MAX, usize::MAX));
        assert!(matches!(
            PoolSwap::get_key(vec![0u8; 4].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // PoolSwapAggregatedKey
        let raw = PoolSwapAggregatedKey::key(&(2, 3600, 1_700_000_000)).unwrap();
        assert_eq!(
            PoolSwapAggregatedKey::get_key(raw.into_boxed_slice()).unwrap(),
            (2, 3600, 1_700_000_000)
        );
        assert!(matches!(
            PoolSwapAggregatedKey::get_key(vec![0u8; 4].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // PoolSwapAggregated key error
        let hash = BlockHash::from_byte_array([4u8; 32]);
        let raw = PoolSwapAggregated::key(&(1, 3600, hash)).unwrap();
        assert_eq!(raw.len(), 40);
        assert!(matches!(
            PoolSwapAggregated::get_key(vec![0u8; 4].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // TransactionByBlockHash
        let raw = TransactionByBlockHash::key(&(hash, 9)).unwrap();
        assert_eq!(
            TransactionByBlockHash::get_key(raw.into_boxed_slice()).unwrap(),
            (hash, 9)
        );
        assert_eq!(TransactionByBlockHash::initial_key(hash), (hash, 0));
        assert!(matches!(
            TransactionByBlockHash::get_key(vec![0u8; 4].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));

        // TransactionVin initial_key
        assert_eq!(TransactionVin::initial_key(txid), format!("{txid}00"));

        // TransactionVout
        let raw = TransactionVout::key(&(txid, 2)).unwrap();
        assert_eq!(
            TransactionVout::get_key(raw.into_boxed_slice()).unwrap(),
            (txid, 2)
        );
        assert!(matches!(
            TransactionVout::get_key(vec![0u8; 4].into_boxed_slice()),
            Err(DBError::WrongKeyLength)
        ));
    }

    #[test]
    fn test_ocean_store_crud_pagination_and_secondary_indexes() {
        let dir = TempDir::new("store-crud");
        let store = Arc::new(OceanStore::new_for_test(&dir.0).unwrap());
        let services = Services::new(Arc::clone(&store));

        // Put and retrieve blocks + secondary index BlockByHeight
        for h in 1u32..=3 {
            let blk = make_block(h, h as u8);
            services.block.by_id.put(&blk.hash, &blk).unwrap();
            services.block.by_height.put(&h, &blk.hash).unwrap();
        }

        let b2 = services
            .block
            .by_id
            .get(&BlockHash::from_byte_array([2u8; 32]))
            .unwrap()
            .unwrap();
        assert_eq!(b2.height, 2);

        // Ascending list
        let asc: Vec<u32> = services
            .block
            .by_height
            .list(None, SortOrder::Ascending)
            .unwrap()
            .map(|r| r.unwrap().0)
            .collect();
        assert_eq!(asc, vec![1, 2, 3]);

        // Descending list with secondary index resolution
        let desc_blocks: Vec<u32> = services
            .block
            .by_height
            .list(None, SortOrder::Descending)
            .unwrap()
            .map(|item| {
                services
                    .block
                    .by_height
                    .retrieve_primary_value(item)
                    .unwrap()
                    .height
            })
            .collect();
        assert_eq!(desc_blocks, vec![3, 2, 1]);

        // Delete & verify removal
        services.block.by_height.delete(&2).unwrap();
        assert!(services.block.by_height.get(&2).unwrap().is_none());

        // Masternode + MasternodeByHeight secondary index + MasternodeStats::get_latest
        assert!(services.masternode.stats.get_latest().unwrap().is_none());
        let mn_id = Txid::from_byte_array([5u8; 32]);
        let mn = model::Masternode {
            id: mn_id,
            owner_address: "owner".to_string(),
            operator_address: "op".to_string(),
            creation_height: 10,
            resign_height: None,
            resign_tx: None,
            minted_blocks: 1,
            timelock: 0,
            collateral: dec!(20000),
            block: model::BlockContext::default(),
            history: vec![],
        };
        services.masternode.by_id.put(&mn_id, &mn).unwrap();
        services.masternode.by_height.put(&(10, mn_id), &1).unwrap();
        let item = services
            .masternode
            .by_height
            .list(None, SortOrder::Ascending)
            .unwrap()
            .next()
            .unwrap();
        let resolved = services
            .masternode
            .by_height
            .retrieve_primary_value(item)
            .unwrap();
        assert_eq!(resolved.id, mn_id);

        let stats1 = model::MasternodeStats {
            block: model::BlockContext {
                height: 10,
                ..Default::default()
            },
            stats: model::MasternodeStatsData::default(),
        };
        let stats2 = model::MasternodeStats {
            block: model::BlockContext {
                height: 20,
                ..Default::default()
            },
            stats: model::MasternodeStatsData::default(),
        };
        services.masternode.stats.put(&10, &stats1).unwrap();
        services.masternode.stats.put(&20, &stats2).unwrap();
        assert_eq!(
            services
                .masternode
                .stats
                .get_latest()
                .unwrap()
                .unwrap()
                .block
                .height,
            20
        );

        // OceanStore maintenance helpers
        let hash1 = store.hash_db_state().unwrap();
        assert!(!hash1.is_empty());
        store.dump_table_sizes().unwrap();
        store.compact();
    }
}
