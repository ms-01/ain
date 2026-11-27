use std::{
    fs,
    marker::PhantomData,
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
};

use ain_db::{
    version::{DBVersionControl, Migration},
    Column, ColumnName, DBError, LedgerColumn, Result, Rocks, TypedColumn,
};
use rocksdb::Direction;
use serde::{Deserialize, Serialize};

static TEST_DIR_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempDbDir {
    path: PathBuf,
}

impl TempDbDir {
    fn new(prefix: &str) -> Self {
        let id = TEST_DIR_COUNTER.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "ain_db_it_{prefix}_{}_{}",
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

struct AccountsCol;
impl ColumnName for AccountsCol {
    const NAME: &'static str = "accounts";
}
impl Column for AccountsCol {
    type Index = u8;
}
impl TypedColumn for AccountsCol {
    type Type = AccountRecord;
}

struct MetadataCol;
impl ColumnName for MetadataCol {
    const NAME: &'static str = "metadata";
}
impl Column for MetadataCol {
    type Index = String;
}
impl TypedColumn for MetadataCol {
    type Type = u32;
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct AccountRecord {
    name: String,
    balance: u64,
}

struct VersionedStore {
    db: Arc<Rocks>,
    metadata: LedgerColumn<MetadataCol>,
    accounts: LedgerColumn<AccountsCol>,
}

impl VersionedStore {
    fn open(path: &PathBuf) -> Result<Self> {
        let db = Arc::new(Rocks::open(
            path,
            vec![(AccountsCol::NAME, None), (MetadataCol::NAME, None)],
            None,
        )?);
        let metadata = LedgerColumn {
            backend: Arc::clone(&db),
            column: PhantomData,
        };
        let accounts = LedgerColumn {
            backend: Arc::clone(&db),
            column: PhantomData,
        };
        Ok(Self {
            db,
            metadata,
            accounts,
        })
    }
}

struct MigrationV1;
impl Migration<VersionedStore> for MigrationV1 {
    fn version(&self) -> u32 {
        1
    }

    fn migrate(&self, store: &VersionedStore) -> Result<()> {
        store.accounts.put(
            &1,
            &AccountRecord {
                name: "genesis".to_string(),
                balance: 100,
            },
        )
    }
}

struct MigrationV2;
impl Migration<VersionedStore> for MigrationV2 {
    fn version(&self) -> u32 {
        2
    }

    fn migrate(&self, store: &VersionedStore) -> Result<()> {
        if let Some(mut rec) = store.accounts.get(&1)? {
            rec.balance = 250;
            store.accounts.put(&1, &rec)?;
        }
        Ok(())
    }
}

impl DBVersionControl for VersionedStore {
    const CURRENT_VERSION: u32 = 2;

    fn set_version(&self, version: u32) -> Result<()> {
        self.metadata
            .put(&Self::VERSION_KEY.to_string(), &version)?;
        self.db.flush()
    }

    fn get_version(&self) -> Result<u32> {
        Ok(self
            .metadata
            .get(&Self::VERSION_KEY.to_string())?
            .unwrap_or(0))
    }

    fn migrate(&self) -> Result<()> {
        let current = self.get_version()?;
        if current > Self::CURRENT_VERSION {
            return Err(DBError::UnsupportedVersion);
        }

        let migrations: Vec<Box<dyn Migration<Self>>> =
            vec![Box::new(MigrationV1), Box::new(MigrationV2)];
        for migration in migrations {
            if migration.version() > current {
                migration.migrate(self)?;
                self.set_version(migration.version())?;
            }
        }
        Ok(())
    }

    fn startup(&self) -> Result<()> {
        self.migrate()
    }
}

#[test]
fn iterator_forward_reverse_and_bounds() {
    let dir = TempDbDir::new("iterators");
    let store = VersionedStore::open(dir.path()).unwrap();

    for id in [1u8, 2, 3, 4, 5] {
        store
            .accounts
            .put(
                &id,
                &AccountRecord {
                    name: format!("acct-{id}"),
                    balance: u64::from(id) * 10,
                },
            )
            .unwrap();
    }

    // Forward from start
    let fwd_all: Vec<u8> = store
        .accounts
        .iter(None, Direction::Forward)
        .unwrap()
        .map(|r| r.unwrap().0)
        .collect();
    assert_eq!(fwd_all, vec![1, 2, 3, 4, 5]);

    // Forward from specific key
    let fwd_from_3: Vec<u8> = store
        .accounts
        .iter(Some(3), Direction::Forward)
        .unwrap()
        .map(|r| r.unwrap().0)
        .collect();
    assert_eq!(fwd_from_3, vec![3, 4, 5]);

    // Reverse from end
    let rev_all: Vec<u8> = store
        .accounts
        .iter(None, Direction::Reverse)
        .unwrap()
        .map(|r| r.unwrap().0)
        .collect();
    assert_eq!(rev_all, vec![5, 4, 3, 2, 1]);

    // Reverse from specific key
    let rev_from_3: Vec<u8> = store
        .accounts
        .iter(Some(3), Direction::Reverse)
        .unwrap()
        .map(|r| r.unwrap().0)
        .collect();
    assert_eq!(rev_from_3, vec![3, 2, 1]);
}

#[test]
fn reopen_persists_data_and_preserves_state_hash() {
    let dir = TempDbDir::new("reopen");
    let cf_names = [AccountsCol::NAME, MetadataCol::NAME];

    let initial_hash = {
        let store = VersionedStore::open(dir.path()).unwrap();
        store
            .accounts
            .put(
                &10,
                &AccountRecord {
                    name: "alice".to_string(),
                    balance: 500,
                },
            )
            .unwrap();
        store
            .accounts
            .put(
                &20,
                &AccountRecord {
                    name: "bob".to_string(),
                    balance: 750,
                },
            )
            .unwrap();
        store.set_version(2).unwrap();
        store.db.flush().unwrap();
        store.db.hash_db_state(&cf_names).unwrap()
    };

    // Reopen and verify all data & hash match
    let reopened = VersionedStore::open(dir.path()).unwrap();
    assert_eq!(
        reopened.accounts.get(&10).unwrap(),
        Some(AccountRecord {
            name: "alice".to_string(),
            balance: 500,
        })
    );
    assert_eq!(
        reopened.accounts.get(&20).unwrap(),
        Some(AccountRecord {
            name: "bob".to_string(),
            balance: 750,
        })
    );
    assert_eq!(reopened.get_version().unwrap(), 2);
    assert_eq!(reopened.db.hash_db_state(&cf_names).unwrap(), initial_hash);
}

#[test]
fn version_control_migrations_idempotency_and_unsupported_version() {
    let dir = TempDbDir::new("versioning");
    let store = VersionedStore::open(dir.path()).unwrap();

    assert_eq!(store.get_version().unwrap(), 0);
    store.startup().unwrap();
    assert_eq!(store.get_version().unwrap(), VersionedStore::CURRENT_VERSION);
    assert_eq!(
        store.accounts.get(&1).unwrap(),
        Some(AccountRecord {
            name: "genesis".to_string(),
            balance: 250,
        })
    );

    // Re-running startup is idempotent
    store.startup().unwrap();
    assert_eq!(store.get_version().unwrap(), VersionedStore::CURRENT_VERSION);

    // Higher DB version triggers UnsupportedVersion error
    store.set_version(VersionedStore::CURRENT_VERSION + 1).unwrap();
    assert!(matches!(
        store.startup(),
        Err(DBError::UnsupportedVersion)
    ));
}
