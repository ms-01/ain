use std::collections::HashMap;

use ain_evm::backend::Overlay;
use ethereum::Account;
use ethereum_types::{H160, H256, U256};

const ALICE: &str = "0x4a1080c5533cb89edc4b65013f08f78868e382de";
const BOB: &str = "0xebf9844ba89c4975bbe4e621dbaf085e6357df3f";

fn account(balance: U256, nonce: u64) -> Account {
    Account {
        balance,
        nonce: U256::from(nonce),
        storage_root: H256::zero(),
        code_hash: H256::zero(),
    }
}

/// 100 DFI in satoshi, i.e. a value that does not fit into an `u64`.
fn hundred_dfi() -> U256 {
    U256::from_str_radix("100000000000000000000", 10).unwrap()
}

#[test]
fn overlay_applies_account_balance_and_code() {
    let mut overlay = Overlay::new();
    let alice: H160 = ALICE.parse().unwrap();

    assert!(overlay.get_account(&alice).is_none());
    assert!(overlay.get_code(&alice).is_none());

    overlay.apply(
        alice,
        account(hundred_dfi(), 7),
        Some(vec![0x60, 0x00]),
        HashMap::new(),
        false,
    );

    let applied = overlay.get_account(&alice).expect("account not applied");
    assert_eq!(applied.balance, hundred_dfi());
    assert_eq!(applied.nonce, U256::from(7));
    assert_eq!(overlay.get_code(&alice), Some(vec![0x60, 0x00]));
}

#[test]
fn overlay_merges_storage_by_default() {
    let mut overlay = Overlay::new();
    let bob: H160 = BOB.parse().unwrap();
    let first = H256::from_low_u64_be(1);
    let second = H256::from_low_u64_be(2);

    let mut storage = HashMap::new();
    storage.insert(first, H256::from_low_u64_be(0xaa));
    overlay.apply(bob, account(U256::from(1), 0), None, storage, false);

    // A second apply without a storage reset keeps the previously stored value.
    let mut storage = HashMap::new();
    storage.insert(second, H256::from_low_u64_be(0xbb));
    overlay.apply(bob, account(U256::from(2), 0), None, storage, false);

    assert_eq!(
        overlay.storage_val(&bob, &first),
        Some(H256::from_low_u64_be(0xaa))
    );
    assert_eq!(
        overlay.storage_val(&bob, &second),
        Some(H256::from_low_u64_be(0xbb))
    );
    assert_eq!(overlay.get_account(&bob).unwrap().balance, U256::from(2));
}

#[test]
fn overlay_resets_storage_on_request() {
    let mut overlay = Overlay::new();
    let bob: H160 = BOB.parse().unwrap();
    let first = H256::from_low_u64_be(1);

    let mut storage = HashMap::new();
    storage.insert(first, H256::from_low_u64_be(0xaa));
    overlay.apply(bob, account(U256::from(1), 0), None, storage, false);

    // A storage reset drops the previously stored entries.
    overlay.apply(bob, account(U256::from(3), 0), None, HashMap::new(), true);

    assert_eq!(overlay.storage_val(&bob, &first), None);
    assert_eq!(overlay.get_account(&bob).unwrap().balance, U256::from(3));
}

#[test]
fn overlay_storage_val_lookup() {
    let mut overlay = Overlay::new();
    let bob: H160 = BOB.parse().unwrap();
    let key = H256::from_low_u64_be(42);

    let mut storage = HashMap::new();
    storage.insert(key, H256::from_low_u64_be(0xff));
    overlay.apply(bob, account(U256::from(1), 0), None, storage, false);

    assert_eq!(
        overlay.storage_val(&bob, &key),
        Some(H256::from_low_u64_be(0xff))
    );
    assert_eq!(overlay.storage_val(&bob, &H256::from_low_u64_be(43)), None);
    // Unknown accounts have no storage at all.
    let alice: H160 = ALICE.parse().unwrap();
    assert_eq!(overlay.storage_val(&alice, &key), None);
}
