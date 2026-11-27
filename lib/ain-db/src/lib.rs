use std::{
    collections::BTreeMap,
    fmt::Debug,
    iter::Iterator,
    marker::PhantomData,
    path::{Path, PathBuf},
    sync::Arc,
};
pub mod version;
use anyhow::format_err;
use log::debug;
use rocksdb::{
    BlockBasedOptions, Cache, ColumnFamily, ColumnFamilyDescriptor, DBCompressionType, DBIterator,
    Direction, IteratorMode, Options, DB,
};
use serde::{de::DeserializeOwned, Serialize};
use sha2::{Digest, Sha256};

pub type Result<T> = result::Result<T, DBError>;

fn get_db_default_options() -> Options {
    let mut block_opts = BlockBasedOptions::default();

    block_opts.set_block_size(64 << 10); // kb
    block_opts.set_block_cache(&Cache::new_lru_cache(64 << 20)); // mb
    block_opts.set_cache_index_and_filter_blocks(true);
    block_opts.set_bloom_filter(10.0, true);
    // block_opts.set_pin_top_level_index_and_filter(true);

    let mut opts = Options::default();

    opts.set_max_open_files(256);

    opts.create_if_missing(true);
    opts.create_missing_column_families(true);
    opts.set_write_buffer_size(64 << 20); // mb
    opts.set_max_write_buffer_number(2);
    // opts.set_min_blob_size(2 << 10); // kb
    //                                  // opts.set_blob_file_size(256 << 20); // mb
    // opts.set_enable_blob_files(true);
    // opts.set_enable_blob_gc(true);
    opts.set_enable_pipelined_write(true);

    opts.set_compression_type(DBCompressionType::Lz4);
    // opts.set_blob_compression_type(DBCompressionType::Lz4);
    opts.set_bottommost_compression_type(DBCompressionType::Zstd);
    opts.set_block_based_table_factory(&block_opts);
    opts.enable_statistics();
    let n = num_cpus::get();
    opts.increase_parallelism(n as i32);
    opts.set_level_compaction_dynamic_level_bytes(true);
    opts
}

#[derive(Debug)]
pub struct Rocks(DB);

impl Rocks {
    pub fn open(
        path: &PathBuf,
        cf_names: Vec<(&'static str, Option<Options>)>,
        opts: Option<Options>,
    ) -> Result<Self> {
        let cf_descriptors = cf_names.into_iter().map(|(cf_name, opts)| {
            ColumnFamilyDescriptor::new(cf_name, opts.unwrap_or_else(Options::default))
        });

        let db_opts = opts.unwrap_or_else(get_db_default_options);
        let db = DB::open_cf_descriptors(&db_opts, path, cf_descriptors)?;

        Ok(Self(db))
    }

    #[allow(dead_code)]
    fn destroy(path: &Path) -> Result<()> {
        DB::destroy(&Options::default(), path)?;

        Ok(())
    }

    pub fn compact(&self) {
        self.0.compact_range(None::<&[u8]>, None::<&[u8]>);
    }

    pub fn cf_handle(&self, cf: &str) -> Result<&ColumnFamily> {
        self.0
            .cf_handle(cf)
            .ok_or_else(|| DBError::Custom(format_err!("Unknown column: {}", cf)))
    }

    fn get_cf(&self, cf: &ColumnFamily, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let opt = self.0.get_cf(cf, key)?;
        Ok(opt)
    }

    fn put_cf(&self, cf: &ColumnFamily, key: &[u8], value: &[u8]) -> Result<()> {
        self.0.put_cf(cf, key, value)?;
        Ok(())
    }

    fn delete_cf(&self, cf: &ColumnFamily, key: &[u8]) -> Result<()> {
        self.0.delete_cf(cf, key)?;
        Ok(())
    }

    pub fn iterator_cf<C>(&self, cf: &ColumnFamily, iterator_mode: IteratorMode) -> DBIterator
    where
        C: Column,
    {
        self.0.iterator_cf(cf, iterator_mode)
    }

    pub fn flush(&self) -> Result<()> {
        self.0.flush()?;
        Ok(())
    }

    pub fn dump_table_sizes(&self, cf_names: &[&'static str]) -> Result<()> {
        let mut stats: BTreeMap<String, (u64, u64, f64)> = BTreeMap::new(); // (size, entries, avg_size)
        let mut total_size: u64 = 0;
        let mut total_entries: u64 = 0;

        for cf_name in cf_names.iter() {
            if let Some(cf) = self.0.cf_handle(cf_name) {
                let size = self
                    .0
                    .property_int_value_cf(cf, "rocksdb.estimate-live-data-size")?
                    .unwrap_or(0);
                let entries = self
                    .0
                    .property_int_value_cf(cf, "rocksdb.estimate-num-keys")?
                    .unwrap_or(0);
                let avg_size = if entries > 0 {
                    size as f64 / entries as f64
                } else {
                    0.0
                };

                stats.insert(cf_name.to_string(), (size, entries, avg_size));
                total_size += size;
                total_entries += entries;
            }
        }

        debug!("RocksDB Table Statistics:");
        debug!("{:-<80}", "");
        debug!(
            "{:<30} {:>10} {:>15} {:>15} {:>10}",
            "Table Name", "Size (MB)", "Entries", "Avg Size (B)", "%% of Total"
        );
        debug!("{:-<80}", "");

        for (name, (size, entries, avg_size)) in stats.iter() {
            let size_mb = *size as f64 / (1024.0 * 1024.0);
            let percentage = (*size as f64 / total_size as f64) * 100.0;
            debug!(
                "{:<30} {:>10.2} {:>15} {:>15.2} {:>9.2}%%",
                name, size_mb, entries, avg_size, percentage
            );
        }

        debug!("{:-<80}", "");
        let total_avg_size = if total_entries > 0 {
            total_size as f64 / total_entries as f64
        } else {
            0.0
        };
        debug!(
            "Total size: {:.2} MB",
            total_size as f64 / (1024.0 * 1024.0)
        );
        debug!("Total entries: {}", total_entries);
        debug!("Overall average entry size: {:.2} bytes", total_avg_size);

        Ok(())
    }

    pub fn hash_db_state(&self, cf_names: &[&'static str]) -> Result<String> {
        let mut hasher = Sha256::new();

        for cf_name in cf_names {
            let cf = self.cf_handle(cf_name)?;
            let iter = self.0.iterator_cf(cf, rocksdb::IteratorMode::Start);

            for item in iter {
                let (key, value) = item?;
                hasher.update(&key);
                hasher.update(&value);
            }
        }

        let result = hasher.finalize();
        Ok(hex::encode(result))
    }
}

//
// ColumnName trait. Define associated column family NAME
//
pub trait ColumnName {
    const NAME: &'static str;
}

//
// Column trait. Define associated index type
//
pub trait Column {
    type Index: Debug + Serialize + DeserializeOwned;

    fn key(index: &Self::Index) -> Result<Vec<u8>> {
        bincode::serialize(index).map_err(DBError::Bincode)
    }

    fn get_key(raw_key: Box<[u8]>) -> Result<Self::Index> {
        bincode::deserialize(&raw_key).map_err(DBError::Bincode)
    }
}

//
// TypedColumn trait. Define associated value type
//
pub trait TypedColumn: Column {
    type Type: Serialize + DeserializeOwned + Debug;
}

#[derive(Debug, Clone)]
pub struct LedgerColumn<C>
where
    C: Column + ColumnName,
{
    pub backend: Arc<Rocks>,
    pub column: PhantomData<C>,
}

impl<C> LedgerColumn<C>
where
    C: Column + ColumnName,
{
    pub fn get_bytes(&self, key: &C::Index) -> Result<Option<Vec<u8>>> {
        self.backend.get_cf(self.handle()?, &C::key(key)?)
    }

    pub fn put_bytes(&self, key: &C::Index, value: &[u8]) -> Result<()> {
        self.backend.put_cf(self.handle()?, &C::key(key)?, value)
    }

    pub fn handle(&self) -> Result<&ColumnFamily> {
        self.backend.cf_handle(C::NAME)
    }
}

impl<C> LedgerColumn<C>
where
    C: TypedColumn + ColumnName,
{
    pub fn get(&self, key: &C::Index) -> Result<Option<C::Type>> {
        if let Some(serialized_value) = self.get_bytes(key)? {
            let value = bincode::deserialize(&serialized_value)?;
            Ok(Some(value))
        } else {
            Ok(None)
        }
    }

    pub fn put(&self, key: &C::Index, value: &C::Type) -> Result<()> {
        let serialized_value = bincode::serialize(value)?;
        self.put_bytes(key, &serialized_value)
    }

    pub fn delete(&self, key: &C::Index) -> Result<()> {
        self.backend.delete_cf(self.handle()?, &C::key(key)?)
    }

    pub fn iter(
        &self,
        from: Option<C::Index>,
        direction: Direction,
    ) -> Result<impl Iterator<Item = Result<(C::Index, C::Type)>> + '_> {
        let index = from
            .as_ref()
            .map(|i| C::key(i))
            .transpose()?
            .unwrap_or_default();

        let iterator_mode = match direction {
            Direction::Forward => from.map_or(IteratorMode::Start, |_| {
                IteratorMode::From(&index, Direction::Forward)
            }),
            Direction::Reverse => from.map_or(IteratorMode::End, |_| {
                IteratorMode::From(&index, Direction::Reverse)
            }),
        };
        Ok(self
            .backend
            .iterator_cf::<C>(self.handle()?, iterator_mode)
            .map(|k| {
                let (key, value) = k?;
                let value = bincode::deserialize(&value)?;
                let key = C::get_key(key)?;
                Ok((key, value))
            }))
    }
}

use std::{error::Error, fmt, result};

use bincode::Error as BincodeError;
use rocksdb::Error as RocksDBError;

#[derive(Debug)]
pub enum DBError {
    RocksDBError(RocksDBError),
    Bincode(BincodeError),
    ParseKey,
    WrongKeyLength,
    Custom(anyhow::Error),
    UnsupportedVersion,
}

impl fmt::Display for DBError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DBError::RocksDBError(e) => write!(f, "RocksDB Error: {e}"),
            DBError::Bincode(e) => write!(f, "Bincode Error: {e}"),
            DBError::ParseKey => write!(f, "Error parsing key"),
            DBError::WrongKeyLength => write!(f, "Wrong key length"),
            DBError::Custom(e) => write!(f, "Custom Error: {e}"),
            DBError::UnsupportedVersion => write!(f, "DB version higher than expected. Node should be updated to support new DB version."),
        }
    }
}

impl Error for DBError {}

impl From<RocksDBError> for DBError {
    fn from(e: RocksDBError) -> Self {
        DBError::RocksDBError(e)
    }
}

impl From<BincodeError> for DBError {
    fn from(e: BincodeError) -> Self {
        DBError::Bincode(e)
    }
}

impl From<anyhow::Error> for DBError {
    fn from(e: anyhow::Error) -> Self {
        DBError::Custom(e)
    }
}

#[cfg(test)]
mod tests {
    use std::{
        fs,
        sync::atomic::{AtomicU64, Ordering},
    };

    use serde::{Deserialize, Serialize};

    use super::*;

    static TEMP_DIR_COUNTER: AtomicU64 = AtomicU64::new(0);

    struct TempDbDir {
        path: PathBuf,
    }

    impl TempDbDir {
        fn new(prefix: &str) -> Self {
            let id = TEMP_DIR_COUNTER.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!(
                "ain_db_unit_{prefix}_{}_{}",
                std::process::id(),
                id
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).expect("failed to create temp dir");
            Self { path }
        }

        fn path(&self) -> &PathBuf {
            &self.path
        }
    }

    impl Drop for TempDbDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    struct ItemsCol;
    impl ColumnName for ItemsCol {
        const NAME: &'static str = "items";
    }
    impl Column for ItemsCol {
        type Index = u32;
    }
    impl TypedColumn for ItemsCol {
        type Type = TestValue;
    }

    struct MissingCol;
    impl ColumnName for MissingCol {
        const NAME: &'static str = "does_not_exist";
    }
    impl Column for MissingCol {
        type Index = u32;
    }
    impl TypedColumn for MissingCol {
        type Type = TestValue;
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    struct TestValue {
        label: String,
        amount: u64,
    }

    fn open_test_db(dir: &TempDbDir) -> Arc<Rocks> {
        Arc::new(
            Rocks::open(dir.path(), vec![(ItemsCol::NAME, None)], None)
                .expect("failed to open RocksDB"),
        )
    }

    #[test]
    fn open_default_and_custom_options_and_destroy() {
        let dir = TempDbDir::new("open_destroy");
        {
            let db = open_test_db(&dir);
            assert!(db.cf_handle(ItemsCol::NAME).is_ok());
            assert!(matches!(
                db.cf_handle("unknown_cf"),
                Err(DBError::Custom(_))
            ));
        }

        // Reopen with explicit DB and CF options
        let mut custom_opts = Options::default();
        custom_opts.create_if_missing(true);
        custom_opts.create_missing_column_families(true);
        {
            let db = Rocks::open(
                dir.path(),
                vec![(ItemsCol::NAME, Some(Options::default()))],
                Some(custom_opts),
            )
            .expect("failed to reopen with custom options");
            assert!(db.cf_handle(ItemsCol::NAME).is_ok());
        }

        Rocks::destroy(dir.path()).expect("destroy should succeed");
    }

    #[test]
    fn raw_cf_crud_and_ledger_column_bytes() {
        let dir = TempDbDir::new("raw_crud");
        let db = open_test_db(&dir);
        let cf = db.cf_handle(ItemsCol::NAME).unwrap();

        assert_eq!(db.get_cf(cf, b"k1").unwrap(), None);
        db.put_cf(cf, b"k1", b"v1").unwrap();
        assert_eq!(db.get_cf(cf, b"k1").unwrap(), Some(b"v1".to_vec()));
        db.delete_cf(cf, b"k1").unwrap();
        assert_eq!(db.get_cf(cf, b"k1").unwrap(), None);

        let col = LedgerColumn::<ItemsCol> {
            backend: Arc::clone(&db),
            column: PhantomData,
        };
        assert_eq!(col.get_bytes(&7).unwrap(), None);
        col.put_bytes(&7, b"raw-bytes").unwrap();
        assert_eq!(col.get_bytes(&7).unwrap(), Some(b"raw-bytes".to_vec()));
    }

    #[test]
    fn typed_column_crud_corrupt_decode_and_missing_cf() {
        let dir = TempDbDir::new("typed_crud");
        let db = open_test_db(&dir);
        let col = LedgerColumn::<ItemsCol> {
            backend: Arc::clone(&db),
            column: PhantomData,
        };

        let v1 = TestValue {
            label: "alpha".to_string(),
            amount: 42,
        };
        let v2 = TestValue {
            label: "beta".to_string(),
            amount: 99,
        };

        assert_eq!(col.get(&1).unwrap(), None);
        col.put(&1, &v1).unwrap();
        assert_eq!(col.get(&1).unwrap(), Some(v1));

        // Overwrite existing key
        col.put(&1, &v2).unwrap();
        assert_eq!(col.get(&1).unwrap(), Some(v2));

        // Delete existing and non-existing keys
        col.delete(&1).unwrap();
        assert_eq!(col.get(&1).unwrap(), None);
        col.delete(&999).unwrap();

        // Corrupt bytes trigger Bincode error on typed get
        col.put_bytes(&2, &[0xff, 0xff]).unwrap();
        assert!(matches!(col.get(&2), Err(DBError::Bincode(_))));

        // Corrupt raw key triggers Bincode error on Column::get_key
        assert!(matches!(
            ItemsCol::get_key(vec![0x01].into_boxed_slice()),
            Err(DBError::Bincode(_))
        ));

        // Unopened column family returns Custom error
        let missing = LedgerColumn::<MissingCol> {
            backend: Arc::clone(&db),
            column: PhantomData,
        };
        assert!(matches!(missing.handle(), Err(DBError::Custom(_))));
        assert!(matches!(missing.get(&1), Err(DBError::Custom(_))));
    }

    #[test]
    fn flush_compact_dump_table_sizes_and_hash_db_state() {
        let dir = TempDbDir::new("maintenance");
        let db = open_test_db(&dir);
        let col = LedgerColumn::<ItemsCol> {
            backend: Arc::clone(&db),
            column: PhantomData,
        };

        // Empty DB table stats and hash
        db.dump_table_sizes(&[ItemsCol::NAME, "nonexistent_cf"])
            .unwrap();
        let empty_hash = db.hash_db_state(&[ItemsCol::NAME]).unwrap();
        assert_eq!(empty_hash.len(), 64);

        // Unknown CF in hash_db_state returns error
        assert!(matches!(
            db.hash_db_state(&["nonexistent_cf"]),
            Err(DBError::Custom(_))
        ));

        for i in 0..10u32 {
            col.put(
                &i,
                &TestValue {
                    label: format!("item-{i}"),
                    amount: u64::from(i) * 10,
                },
            )
            .unwrap();
        }

        db.flush().unwrap();
        db.compact();
        db.dump_table_sizes(&[ItemsCol::NAME]).unwrap();

        let populated_hash = db.hash_db_state(&[ItemsCol::NAME]).unwrap();
        assert_eq!(populated_hash.len(), 64);
        assert_ne!(empty_hash, populated_hash);

        // Deterministic hash on repeated calls
        assert_eq!(
            populated_hash,
            db.hash_db_state(&[ItemsCol::NAME]).unwrap()
        );
    }

    #[test]
    fn db_error_display_and_conversions() {
        let bincode_err: DBError = bincode::deserialize::<u64>(&[0x01]).unwrap_err().into();
        assert!(bincode_err.to_string().contains("Bincode Error:"));

        let custom_err: DBError = format_err!("boom").into();
        assert!(custom_err.to_string().contains("Custom Error: boom"));

        assert_eq!(DBError::ParseKey.to_string(), "Error parsing key");
        assert_eq!(DBError::WrongKeyLength.to_string(), "Wrong key length");
        assert!(DBError::UnsupportedVersion
            .to_string()
            .contains("DB version higher than expected"));

        // Trigger a real RocksDB error by opening a non-existent DB with create_if_missing(false)
        let dir = TempDbDir::new("rocks_err");
        let missing_path = dir.path().join("does_not_exist");
        let mut opts = Options::default();
        opts.create_if_missing(false);
        let rocks_err = Rocks::open(&missing_path, vec![], Some(opts)).unwrap_err();
        assert!(rocks_err.to_string().contains("RocksDB Error:"));
        let _: &dyn Error = &rocks_err;
    }
}
