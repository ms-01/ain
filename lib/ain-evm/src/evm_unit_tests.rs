use std::{
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
};

use ain_contracts::{
    get_dfi_reserved_contract, get_dst20_contract, get_dst20_v1_contract, get_dst20_v2_contract,
    get_dst20_v3_contract,
};
use ethereum::{
    Block, Header, LegacyTransaction, PartialHeader, TransactionAction, TransactionSignature,
    TransactionV2,
};
use ethereum_types::{Bloom, H160, H256, U256};
use evm::{
    backend::{Apply, ApplyBackend, Backend, Basic},
    ExitReason, ExitRevert, ExitSucceed,
};
use once_cell::sync::OnceCell;
use vsdb_core::vsdb_set_base_dir;

use crate::{
    backend::{BackendError, EVMBackend, InsufficientBalance, Overlay, Vicinity},
    bytes::Bytes,
    contract::dst20::{
        bridge_dst20_in, bridge_dst20_out, dst20_allowance, dst20_deploy_contract_tx,
        dst20_v1_deploy_info, dst20_v2_deploy_info, dst20_v3_deploy_info, reserve_dst20_namespace,
    },
    core::{EVMCoreService, EthCallArgs},
    executor::{AinExecutor, ExecutorContext},
    storage::{traits::BlockStorage, Storage},
    transaction::{
        cache::{SignedTxCache, TransactionCache, TxValidationCache},
        system::TransferDirection,
        SignedTx, LOWER_H256,
    },
    trie::{TrieDBStore, GENESIS_STATE_ROOT},
};

static TEST_TRIE_STORE: OnceCell<Arc<TrieDBStore>> = OnceCell::new();
static VSDB_LOCK: parking_lot::Mutex<()> = parking_lot::const_mutex(());
static TEST_DIR_COUNTER: AtomicU64 = AtomicU64::new(0);

pub(crate) fn vsdb_lock() -> parking_lot::MutexGuard<'static, ()> {
    VSDB_LOCK.lock()
}

pub(crate) fn test_trie_store() -> Arc<TrieDBStore> {
    Arc::clone(TEST_TRIE_STORE.get_or_init(|| {
        let dir = std::env::temp_dir().join(format!(
            "ain_evm_unit_tests_vsdb_{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("create vsdb temp dir");
        vsdb_set_base_dir(&dir).expect("set vsdb base dir");
        Arc::new(TrieDBStore::new())
    }))
}

fn new_temp_storage() -> (
    parking_lot::MutexGuard<'static, ()>,
    Arc<TrieDBStore>,
    Arc<Storage>,
    PathBuf,
) {
    let guard = vsdb_lock();
    let trie_store = test_trie_store();
    let id = TEST_DIR_COUNTER.fetch_add(1, Ordering::SeqCst);
    let dir = std::env::temp_dir().join(format!(
        "ain_evm_unit_tests_storage_{}_{}",
        std::process::id(),
        id
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("create storage temp dir");
    let storage = Arc::new(Storage::new(&dir).expect("create storage"));
    (guard, trie_store, storage, dir)
}

fn new_backend(
    vicinity: Vicinity,
) -> (
    parking_lot::MutexGuard<'static, ()>,
    EVMBackend,
    Arc<TrieDBStore>,
    Arc<Storage>,
    PathBuf,
) {
    let (guard, trie_store, storage, dir) = new_temp_storage();
    let backend = EVMBackend::from_root(
        GENESIS_STATE_ROOT,
        Arc::clone(&trie_store),
        Arc::clone(&storage),
        vicinity,
        None,
    )
    .expect("from_root with genesis state root");
    (guard, backend, trie_store, storage, dir)
}

fn sample_legacy_signed_tx(nonce: u64, gas_price: u64, gas_limit: u64) -> SignedTx {
    let tx = TransactionV2::Legacy(LegacyTransaction {
        nonce: U256::from(nonce),
        gas_price: U256::from(gas_price),
        gas_limit: U256::from(gas_limit),
        action: TransactionAction::Call(H160::from_low_u64_be(0x1234)),
        value: U256::zero(),
        input: Vec::new(),
        signature: TransactionSignature::new(27, LOWER_H256, LOWER_H256).unwrap(),
    });
    SignedTx::try_from(tx).expect("valid signed tx")
}

fn sample_header(number: u64, state_root: H256, extra_data: Vec<u8>) -> Header {
    Header::new(
        PartialHeader {
            parent_hash: H256::zero(),
            beneficiary: H160::from_low_u64_be(0xbeef),
            state_root,
            receipts_root: H256::zero(),
            logs_bloom: Bloom::default(),
            difficulty: U256::from(7),
            number: U256::from(number),
            gas_limit: U256::from(30_000_000u64),
            gas_used: U256::zero(),
            timestamp: 1_700_000_000,
            extra_data,
            mix_hash: H256::zero(),
            nonce: Default::default(),
            base_fee: U256::from(10_000_000_000u64),
        },
        H256::zero(),
        H256::zero(),
    )
}

// ============================================================================
// backend.rs tests
// ============================================================================

#[test]
fn vicinity_from_header_parses_dvm_block_and_fields() {
    let header = sample_header(12, H256::zero(), b"DFI: 99".to_vec());
    let vicinity = Vicinity::from(header.clone());
    assert_eq!(vicinity.beneficiary, header.beneficiary);
    assert_eq!(vicinity.block_number, U256::from(12));
    assert_eq!(vicinity.timestamp, 1_700_000_000);
    assert_eq!(vicinity.block_difficulty, U256::from(7));
    assert_eq!(vicinity.block_gas_limit, U256::from(30_000_000u64));
    assert_eq!(
        vicinity.block_base_fee_per_gas,
        U256::from(10_000_000_000u64)
    );
    assert_eq!(vicinity.dvm_block, Some(99));
    assert_eq!(vicinity.block_randomness, None);

    let no_prefix = Vicinity::from(sample_header(1, H256::zero(), b"OTHER: 99".to_vec()));
    assert_eq!(no_prefix.dvm_block, None);

    let bad_utf8 = Vicinity::from(sample_header(1, H256::zero(), vec![0xff, 0xfe]));
    assert_eq!(bad_utf8.dvm_block, None);
}

#[test]
fn backend_from_root_unknown_root_has_no_accounts() {
    let (_guard, trie_store, storage, dir) = new_temp_storage();
    let backend = EVMBackend::from_root(
        H256::from_low_u64_be(0xdead),
        trie_store,
        storage,
        Vicinity::default(),
        None,
    )
    .expect("from_root succeeds lazily");
    assert!(backend.get_account(&H160::from_low_u64_be(1)).is_none());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn backend_account_code_storage_commit_and_restore() {
    let (_guard, mut backend, trie_store, storage, dir) = new_backend(Vicinity {
        block_number: U256::from(5),
        block_base_fee_per_gas: U256::from(10),
        block_gas_limit: U256::from(30_000_000u64),
        beneficiary: H160::from_low_u64_be(0x11),
        timestamp: 12345,
        block_difficulty: U256::from(2),
        block_randomness: Some(H256::from_low_u64_be(0x99)),
        ..Vicinity::default()
    });

    let addr = H160::from_low_u64_be(0xaa);
    let slot = H256::from_low_u64_be(1);
    let val = H256::from_low_u64_be(0x42);
    let code = vec![0x60, 0x2a, 0x60, 0x00, 0x52];

    assert!(!backend.exists(addr));
    assert_eq!(backend.get_balance(&addr), U256::zero());
    assert_eq!(backend.get_nonce(&addr), U256::zero());
    assert_eq!(backend.storage(addr, slot), H256::zero());

    backend
        .apply(
            addr,
            Some(Basic {
                balance: U256::from(1_000),
                nonce: U256::from(3),
            }),
            Some(code.clone()),
            vec![(slot, val)],
            true,
        )
        .unwrap();

    assert!(backend.exists(addr));
    assert_eq!(backend.get_balance(&addr), U256::from(1_000));
    assert_eq!(backend.get_nonce(&addr), U256::from(3));
    assert_eq!(backend.code(addr), code);
    assert_eq!(backend.storage(addr, slot), val);
    assert_eq!(backend.original_storage(addr, slot), Some(val));
    assert_eq!(
        backend.get_contract_storage(addr, slot).unwrap(),
        U256::from(0x42)
    );

    // Backend trait getters
    assert_eq!(backend.block_number(), U256::from(5));
    assert_eq!(backend.block_coinbase(), H160::from_low_u64_be(0x11));
    assert_eq!(backend.block_timestamp(), U256::from(12345));
    assert_eq!(backend.block_difficulty(), U256::from(2));
    assert_eq!(
        backend.block_randomness(),
        Some(H256::from_low_u64_be(0x99))
    );
    assert_eq!(backend.block_gas_limit(), U256::from(30_000_000u64));
    assert_eq!(backend.block_base_fee_per_gas(), U256::from(10));
    assert_eq!(backend.block_hash(U256::from(999)), H256::zero());

    let new_root = backend.commit(false).unwrap();
    assert_ne!(new_root, GENESIS_STATE_ROOT);
    let _ro = backend.ro_handle();

    // Restore from committed root with empty overlay and verify persistence
    let restored = EVMBackend::from_root(
        new_root,
        Arc::clone(&trie_store),
        Arc::clone(&storage),
        Vicinity::default(),
        Some(Overlay::default()),
    )
    .unwrap();
    assert!(restored.exists(addr));
    assert_eq!(restored.get_balance(&addr), U256::from(1_000));
    assert_eq!(restored.get_nonce(&addr), U256::from(3));
    assert_eq!(restored.code(addr), code);
    assert_eq!(restored.storage(addr, slot), val);

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn backend_apply_backend_delete_and_changeset_resets() {
    let (_guard, mut backend, _trie_store, _storage, dir) = new_backend(Vicinity::default());
    let a1 = H160::from_low_u64_be(0x101);
    let a2 = H160::from_low_u64_be(0x102);

    backend.add_balance(a1, U256::from(500)).unwrap();
    backend.increase_tx_count(); // changeset[0] has a1=500

    backend.add_balance(a2, U256::from(700)).unwrap();
    backend.increase_tx_count(); // changeset[1] has a1=500, a2=700

    // Uncommitted overlay change
    backend.add_balance(a1, U256::from(100)).unwrap();
    assert_eq!(backend.get_balance(&a1), U256::from(600));

    // Reset to last changeset reverts uncommitted change
    backend.reset_to_last_changeset();
    assert_eq!(backend.get_balance(&a1), U256::from(500));
    assert_eq!(backend.get_balance(&a2), U256::from(700));

    // Reset from tx 0 reverts a2
    backend.reset_from_tx(0);
    assert_eq!(backend.get_balance(&a1), U256::from(500));
    assert_eq!(backend.get_balance(&a2), U256::zero());

    // clear_overlay clears in-memory state
    backend.clear_overlay();
    assert_eq!(backend.get_balance(&a1), U256::zero());

    // ApplyBackend::apply with Modify (delete_empty) and Delete
    let empty_addr = H160::from_low_u64_be(0x201);
    let live_addr = H160::from_low_u64_be(0x202);
    ApplyBackend::apply(
        &mut backend,
        vec![
            Apply::Modify {
                address: empty_addr,
                basic: Basic {
                    balance: U256::zero(),
                    nonce: U256::zero(),
                },
                code: None,
                storage: Vec::<(H256, H256)>::new(),
                reset_storage: false,
            },
            Apply::Modify {
                address: live_addr,
                basic: Basic {
                    balance: U256::from(10),
                    nonce: U256::from(1),
                },
                code: None,
                storage: Vec::new(),
                reset_storage: false,
            },
        ],
        Vec::new(),
        true,
    );
    let _root = backend.commit(true).unwrap();
    assert!(!backend.exists(empty_addr));
    assert!(backend.exists(live_addr));

    // Now delete live_addr via Apply::Delete
    ApplyBackend::apply(
        &mut backend,
        vec![Apply::<Vec<(H256, H256)>>::Delete { address: live_addr }],
        Vec::new(),
        true,
    );
    let _root2 = backend.commit(false).unwrap();
    assert!(!backend.exists(live_addr));

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn backend_balance_prepay_refund_and_vicinity_updates() {
    let (_guard, mut backend, _trie_store, _storage, dir) = new_backend(Vicinity {
        block_base_fee_per_gas: U256::from(10),
        ..Vicinity::default()
    });

    let tx = sample_legacy_signed_tx(0, 20, 21_000);
    let sender = tx.sender;

    // sub_balance on missing account fails with NoSuchAccount
    assert!(backend.sub_balance(sender, U256::from(1)).is_err());

    // add_balance + overflow check
    backend.add_balance(sender, U256::from(1_000_000)).unwrap();
    assert!(backend.add_balance(sender, U256::MAX).is_err());

    // sub_balance normal + insufficient balance
    backend.sub_balance(sender, U256::from(100_000)).unwrap();
    assert_eq!(backend.get_balance(&sender), U256::from(900_000));
    assert!(backend.sub_balance(sender, U256::from(900_001)).is_err());

    // update_vicinity_from_tx, update_vicinity_with_gas_used, update_vicinity_from_header
    backend.update_vicinity_from_tx(&tx).unwrap();
    assert_eq!(backend.origin(), sender);
    assert_eq!(backend.gas_price(), U256::from(20));

    backend.update_vicinity_with_gas_used(U256::from(21_000));
    assert_eq!(backend.vicinity.total_gas_used, U256::from(21_000));

    let header = sample_header(7, H256::zero(), b"DFI: 77".to_vec());
    backend.update_vicinity_from_header(header);
    assert_eq!(backend.block_number(), U256::from(7));
    assert_eq!(backend.vicinity.dvm_block, Some(77));

    // deduct_prepay_gas_fee & refund_unused_gas_fee
    backend
        .deduct_prepay_gas_fee(sender, U256::from(420_000))
        .unwrap();
    assert_eq!(backend.get_balance(&sender), U256::from(480_000));
    assert!(backend
        .deduct_prepay_gas_fee(sender, U256::from(999_999))
        .is_err());

    // Refund 11_000 unused gas at gas_price 20 => +220_000
    backend
        .refund_unused_gas_fee(&tx, U256::from(10_000), U256::from(10))
        .unwrap();
    assert_eq!(backend.get_balance(&sender), U256::from(700_000));

    // used_gas > gas_limit fails
    assert!(backend
        .refund_unused_gas_fee(&tx, U256::from(25_000), U256::from(10))
        .is_err());

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn backend_error_display_covers_all_variants() {
    let addr = H160::from_low_u64_be(1);
    let errors = [
        BackendError::TrieCreationFailed("c".into()),
        BackendError::TrieRestoreFailed("r".into()),
        BackendError::TrieError("t".into()),
        BackendError::NoSuchAccount(addr),
        BackendError::InsufficientBalance(InsufficientBalance {
            address: addr,
            account_balance: U256::from(5),
            amount: U256::from(10),
        }),
        BackendError::DeductPrepayGasFailed("d".into()),
        BackendError::RefundUnusedGasFailed("u".into()),
    ];
    for err in errors {
        assert!(!err.to_string().is_empty());
    }
}

// ============================================================================
// contract/dst20.rs tests
// ============================================================================

#[test]
fn dst20_deploy_info_versions_and_tx() {
    let v1 = dst20_v1_deploy_info();
    assert_eq!(v1.address, get_dst20_v1_contract().fixed_address);
    assert!(!v1.bytecode.0.is_empty());
    assert!(v1.storage.is_empty());

    let v2 = dst20_v2_deploy_info();
    assert_eq!(v2.address, get_dst20_v2_contract().fixed_address);
    assert!(!v2.bytecode.0.is_empty());

    let v3 = dst20_v3_deploy_info();
    assert_eq!(v3.address, get_dst20_v3_contract().fixed_address);
    assert!(!v3.bytecode.0.is_empty());

    let base_fee = U256::from(10_000_000_000u64);
    let (tx, receipt) = dst20_deploy_contract_tx(42, &base_fee).unwrap();
    assert_eq!(tx.nonce(), U256::from(42));
    assert_eq!(tx.gas_price(), base_fee);
    assert_eq!(tx.action(), TransactionAction::Create);
    match receipt {
        ethereum::ReceiptV3::Legacy(data) | ethereum::ReceiptV3::EIP1559(data) | ethereum::ReceiptV3::EIP2930(data) => {
            assert_eq!(data.status_code, 1)
        }
    }
}

#[test]
fn dst20_allowance_and_bridge_in_out() {
    let (_guard, mut backend, _trie_store, _storage, dir) = new_backend(Vicinity::default());
    let contract_addr = H160::from_low_u64_be(0x5000);
    let user = H160::from_low_u64_be(0x6000);

    // Allowance differs by direction
    let allow_in = dst20_allowance(TransferDirection::EvmIn, user, U256::from(123));
    let allow_out = dst20_allowance(TransferDirection::EvmOut, user, U256::from(123));
    assert_eq!(allow_in.len(), 1);
    assert_eq!(allow_out.len(), 1);
    assert_ne!(allow_in[0].0, allow_out[0].0);

    // Bridge on non-existent contract fails
    assert!(bridge_dst20_in(&backend, contract_addr, U256::from(100), false).is_err());
    assert!(bridge_dst20_out(&backend, contract_addr, U256::from(100), false).is_err());

    // Bridge on invalid codehash fails
    backend
        .deploy_contract(&contract_addr, vec![0x60, 0x00], Vec::new())
        .unwrap();
    assert!(bridge_dst20_in(&backend, contract_addr, U256::from(100), false).is_err());
    assert!(bridge_dst20_out(&backend, contract_addr, U256::from(100), false).is_err());

    // Deploy valid legacy DST20 bytecode
    let dst20_bytecode = get_dst20_contract().runtime_bytecode;
    backend
        .deploy_contract(&contract_addr, dst20_bytecode, Vec::new())
        .unwrap();

    // Bridge in 500
    let in_info = bridge_dst20_in(&backend, contract_addr, U256::from(500), false).unwrap();
    assert_eq!(in_info.address, contract_addr);
    assert_eq!(in_info.storage.len(), 2);
    backend
        .update_storage(&contract_addr, in_info.storage)
        .unwrap();

    // Overflow on bridge_dst20_in
    assert!(bridge_dst20_in(&backend, contract_addr, U256::MAX, false).is_err());

    // Bridge out 200 (remaining total supply = 300)
    let out_info = bridge_dst20_out(&backend, contract_addr, U256::from(200), false).unwrap();
    assert_eq!(out_info.address, contract_addr);
    backend
        .update_storage(&contract_addr, out_info.storage)
        .unwrap();
    assert_eq!(
        backend
            .get_contract_storage(contract_addr, H256::from_low_u64_be(2))
            .unwrap(),
        U256::from(300)
    );

    // Underflow on bridge_dst20_out
    assert!(bridge_dst20_out(&backend, contract_addr, U256::from(301), false).is_err());

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn dst20_reserve_namespace_deploys_reserved_code() {
    let (_guard, mut backend, _trie_store, _storage, dir) = new_backend(Vicinity::default());
    let mut executor = AinExecutor::new(&mut backend);
    reserve_dst20_namespace(&mut executor).unwrap();

    let first = ain_contracts::dst20_address_from_token_id(0).unwrap();
    let last = ain_contracts::dst20_address_from_token_id(1023).unwrap();
    let expected_hash = get_dfi_reserved_contract().codehash;
    assert_eq!(
        executor.backend.get_account(&first).unwrap().code_hash,
        expected_hash
    );
    assert_eq!(
        executor.backend.get_account(&last).unwrap().code_hash,
        expected_hash
    );

    let _ = std::fs::remove_dir_all(&dir);
}

// ============================================================================
// executor.rs tests
// ============================================================================

#[test]
fn executor_state_helpers_and_readonly_calls() {
    let (_guard, mut backend, _trie_store, _storage, dir) = new_backend(Vicinity {
        block_gas_limit: U256::from(30_000_000u64),
        dvm_block: None,
        ..Vicinity::default()
    });
    let caller = H160::from_low_u64_be(0x10);
    let contract = H160::from_low_u64_be(0x20);
    let revert_contract = H160::from_low_u64_be(0x21);

    // Runtime bytecode that returns 0x2a (42) as a 32-byte word:
    // PUSH1 0x2a, PUSH1 0x00, MSTORE, PUSH1 0x20, PUSH1 0x00, RETURN
    let return_42_code = vec![0x60, 0x2a, 0x60, 0x00, 0x52, 0x60, 0x20, 0x60, 0x00, 0xf3];
    // Runtime bytecode that reverts: PUSH1 0x00, PUSH1 0x00, REVERT
    let revert_code = vec![0x60, 0x00, 0x60, 0x00, 0xfd];

    let mut executor = AinExecutor::new(&mut backend);
    executor.add_balance(caller, U256::from(1_000)).unwrap();
    executor.sub_balance(caller, U256::from(200)).unwrap();
    executor.update_total_gas_used(U256::from(21_000));
    assert_eq!(executor.get_nonce(&caller), U256::zero());

    executor
        .deploy_contract(contract, Bytes::from(return_42_code), Vec::new())
        .unwrap();
    executor
        .deploy_contract(revert_contract, Bytes::from(revert_code), Vec::new())
        .unwrap();
    executor
        .update_storage(
            contract,
            vec![(H256::from_low_u64_be(1), H256::from_low_u64_be(9))],
        )
        .unwrap();

    // Call returning 42
    let resp = executor.call(ExecutorContext {
        caller,
        to: Some(contract),
        value: U256::zero(),
        data: &[],
        gas_limit: 100_000,
        access_list: Vec::new(),
    });
    assert_eq!(resp.exit_reason, ExitReason::Succeed(ExitSucceed::Returned));
    assert_eq!(resp.data.len(), 32);
    assert_eq!(resp.data[31], 0x2a);

    // Call reverting
    let rev = executor.call(ExecutorContext {
        caller,
        to: Some(revert_contract),
        value: U256::zero(),
        data: &[],
        gas_limit: 100_000,
        access_list: Vec::new(),
    });
    assert_eq!(rev.exit_reason, ExitReason::Revert(ExitRevert::Reverted));

    // Create call (to: None) deploying empty runtime code: PUSH1 0x00, PUSH1 0x00, RETURN
    let init_code = [0x60, 0x00, 0x60, 0x00, 0xf3];
    let create_resp = executor.call(ExecutorContext {
        caller,
        to: None,
        value: U256::zero(),
        data: &init_code,
        gas_limit: 100_000,
        access_list: Vec::new(),
    });
    assert_eq!(
        create_resp.exit_reason,
        ExitReason::Succeed(ExitSucceed::Returned)
    );

    let root = executor.commit(false).unwrap();
    assert_ne!(root, GENESIS_STATE_ROOT);

    let _ = std::fs::remove_dir_all(&dir);
}

// ============================================================================
// core.rs tests
// ============================================================================

#[test]
fn core_service_state_queries_nonces_and_call() {
    let (_guard, trie_store, storage, dir) = new_temp_storage();
    let tx_cache = Arc::new(TransactionCache {
        signed_tx_cache: SignedTxCache::new(64),
        tx_validation_cache: TxValidationCache::new(64),
    });
    let core = EVMCoreService::restore(
        Arc::clone(&trie_store),
        Arc::clone(&storage),
        Arc::clone(&tx_cache),
    );

    // Empty storage -> default state root & missing latest block backend
    assert_eq!(core.get_latest_state_root().unwrap(), H256::default());
    assert!(core.get_latest_block_backend().is_err());
    assert!(core
        .get_backend_from_block(Some(U256::zero()), None, None, None)
        .is_err());

    // Populate state and commit
    let mut backend = EVMBackend::from_root(
        GENESIS_STATE_ROOT,
        Arc::clone(&trie_store),
        Arc::clone(&storage),
        Vicinity::default(),
        None,
    )
    .unwrap();
    let addr = H160::from_low_u64_be(0x777);
    let return_7_code = vec![0x60, 0x07, 0x60, 0x00, 0x52, 0x60, 0x20, 0x60, 0x00, 0xf3];
    backend
        .apply(
            addr,
            Some(Basic {
                balance: U256::from(555),
                nonce: U256::from(2),
            }),
            Some(return_7_code.clone()),
            vec![(H256::from_low_u64_be(3), H256::from_low_u64_be(0xab))],
            true,
        )
        .unwrap();
    let state_root = backend.commit(false).unwrap();

    // Persist a block with this state_root
    let header = sample_header(0, state_root, Vec::new());
    let block: Block<TransactionV2> = Block {
        header,
        transactions: Vec::new(),
        ommers: Vec::new(),
    };
    storage.put_block(&block).unwrap();
    storage.put_latest_block(Some(&block)).unwrap();

    assert_eq!(core.get_latest_state_root().unwrap(), state_root);
    assert_eq!(core.get_balance(addr, state_root).unwrap(), U256::from(555));
    assert_eq!(
        core.get_balance(H160::from_low_u64_be(0x999), state_root)
            .unwrap(),
        U256::zero()
    );
    assert_eq!(
        core.get_code(addr, state_root).unwrap(),
        Some(return_7_code)
    );
    assert_eq!(
        core.get_code(H160::from_low_u64_be(0x999), state_root)
            .unwrap(),
        None
    );
    assert_eq!(
        core.get_storage_at(addr, U256::from(3), state_root)
            .unwrap(),
        Some(H256::from_low_u64_be(0xab).as_bytes().to_vec())
    );
    assert_eq!(
        core.get_storage_at(H160::from_low_u64_be(0x999), U256::from(3), state_root)
            .unwrap(),
        None
    );

    let latest_backend = core.get_latest_block_backend().unwrap();
    assert_eq!(latest_backend.get_balance(&addr), U256::from(555));

    let block_backend = core
        .get_backend_from_block(
            Some(U256::zero()),
            Some(addr),
            Some(U256::from(123)),
            None,
        )
        .unwrap();
    assert_eq!(block_backend.origin(), addr);
    assert_eq!(block_backend.gas_price(), U256::from(123));

    // Read-only call via EVMCoreService::call
    let call_resp = core
        .call(
            EthCallArgs {
                caller: H160::from_low_u64_be(0x1),
                to: Some(addr),
                value: U256::zero(),
                data: &[],
                gas_limit: 100_000,
                gas_price: U256::zero(),
                access_list: Vec::new(),
                block_number: U256::zero(),
            },
            None,
        )
        .unwrap();
    assert_eq!(
        call_resp.exit_reason,
        ExitReason::Succeed(ExitSucceed::Returned)
    );
    assert_eq!(call_resp.data[31], 0x07);

    // Nonce store helpers
    assert!(core.store_account_nonce(addr, U256::from(2)));
    assert!(core.store_account_nonce(addr, U256::from(3)));
    core.clear_account_nonce();

    let _ = std::fs::remove_dir_all(&dir);
}
