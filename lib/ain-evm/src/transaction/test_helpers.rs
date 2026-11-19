//! Test-only helpers to build signed transactions.
//!
//! This module is compiled exclusively for the test target (`#[cfg(test)]`) and is
//! therefore not part of any release build. It provides the small amount of signing
//! plumbing that is required to construct *genuinely* signed transactions: `SignedTx`
//! recovers the sender from the signature, so unsigned or dummy-signed transactions
//! cannot be used to exercise the transaction helpers.

use ethereum::{
    AccessList, EIP1559Transaction, EIP1559TransactionMessage, EIP2930Transaction,
    EIP2930TransactionMessage, TransactionAction, TransactionSignature, TransactionV2,
};
use ethereum_types::{H160, H256, U256};

use crate::transaction::{LegacyUnsignedTransaction, SignedTx};

/// Arbitrary, test-only chain id. Unit tests never call
/// `ain_cpp_imports::get_chain_id()` (the C++ side is not initialised in unit tests)
/// and the sender recovery path is pure Rust.
pub const TEST_CHAIN_ID: u64 = 1127;
/// Fixed test secret key, only used to produce recoverable signatures.
pub const TEST_SECRET_KEY: [u8; 32] = [0x01; 32];

/// Additional transaction fields (value and nonce) of the test builders.
#[derive(Clone, Copy, Debug)]
pub struct TxFields {
    pub value: U256,
    pub nonce: U256,
}

impl TxFields {
    /// Creates the transaction fields with the given value and nonce.
    pub fn new(value: U256, nonce: U256) -> Self {
        Self { value, nonce }
    }

    /// Creates the transaction fields with a zero value and a zero nonce.
    pub fn zero() -> Self {
        Self::new(U256::zero(), U256::zero())
    }
}

/// Signs the given hash with the test secret key and returns the parity flag
/// together with the `r` and `s` components of the signature.
pub fn sign_hash(hash: H256) -> (bool, H256, H256) {
    let message = libsecp256k1::Message::parse(hash.as_fixed_bytes());
    let secret_key = libsecp256k1::SecretKey::parse_slice(&TEST_SECRET_KEY).unwrap();
    let signature = libsecp256k1::sign(&message, &secret_key);
    let serialized = signature.0.serialize();
    (
        signature.1.serialize() == 1,
        H256::from_slice(&serialized[0..32]),
        H256::from_slice(&serialized[32..64]),
    )
}

/// Fails loudly if the test helper does not produce a recoverable signature.
pub fn assert_signed(signed_tx: SignedTx) -> SignedTx {
    assert_ne!(signed_tx.sender, H160::zero());
    signed_tx
}

/// Builds a signed legacy (EIP-155) transaction with the given value and nonce.
pub fn legacy_tx_with(
    gas_limit: U256,
    gas_price: U256,
    action: TransactionAction,
    input: Vec<u8>,
    fields: TxFields,
) -> SignedTx {
    let unsigned = LegacyUnsignedTransaction {
        nonce: fields.nonce,
        gas_price,
        gas_limit,
        action,
        value: fields.value,
        input,
        sig: TransactionSignature::new(27, H256::from_low_u64_be(1), H256::from_low_u64_be(1))
            .unwrap(),
    };
    let signed = unsigned
        .sign_with_chain_id(&TEST_SECRET_KEY, TEST_CHAIN_ID)
        .unwrap();
    assert_signed(SignedTx::try_from(TransactionV2::Legacy(signed)).unwrap())
}

/// Builds a signed legacy (EIP-155) transaction with a zero value and nonce.
pub fn legacy_tx(
    gas_limit: U256,
    gas_price: U256,
    action: TransactionAction,
    input: Vec<u8>,
) -> SignedTx {
    legacy_tx_with(gas_limit, gas_price, action, input, TxFields::zero())
}

/// Builds a signed EIP-2930 transaction with the given value and nonce. The crate
/// only exposes a signing helper for the legacy format, so the typed transaction is
/// hashed and signed locally.
pub fn eip2930_tx_with(
    gas_limit: U256,
    gas_price: U256,
    action: TransactionAction,
    input: Vec<u8>,
    access_list: AccessList,
    fields: TxFields,
) -> SignedTx {
    let mut tx = EIP2930Transaction {
        chain_id: TEST_CHAIN_ID,
        nonce: fields.nonce,
        gas_price,
        gas_limit,
        action,
        value: fields.value,
        input,
        access_list,
        odd_y_parity: false,
        r: H256::zero(),
        s: H256::zero(),
    };
    let message = EIP2930TransactionMessage::from(tx.clone());
    let (odd_y_parity, r, s) = sign_hash(message.hash());
    tx.odd_y_parity = odd_y_parity;
    tx.r = r;
    tx.s = s;
    assert_signed(SignedTx::try_from(TransactionV2::EIP2930(tx)).unwrap())
}

/// Builds a signed EIP-2930 transaction with a zero value and nonce.
pub fn eip2930_tx(
    gas_limit: U256,
    gas_price: U256,
    action: TransactionAction,
    input: Vec<u8>,
    access_list: AccessList,
) -> SignedTx {
    eip2930_tx_with(
        gas_limit,
        gas_price,
        action,
        input,
        access_list,
        TxFields::zero(),
    )
}

/// Builds a signed EIP-1559 transaction with the given value and nonce. See
/// `eip2930_tx_with` for the signing.
pub fn eip1559_tx_with(
    gas_limit: U256,
    max_fee_per_gas: U256,
    max_priority_fee_per_gas: U256,
    action: TransactionAction,
    input: Vec<u8>,
    access_list: AccessList,
    fields: TxFields,
) -> SignedTx {
    let mut tx = EIP1559Transaction {
        chain_id: TEST_CHAIN_ID,
        nonce: fields.nonce,
        max_priority_fee_per_gas,
        max_fee_per_gas,
        gas_limit,
        action,
        value: fields.value,
        input,
        access_list,
        odd_y_parity: false,
        r: H256::zero(),
        s: H256::zero(),
    };
    let message = EIP1559TransactionMessage::from(tx.clone());
    let (odd_y_parity, r, s) = sign_hash(message.hash());
    tx.odd_y_parity = odd_y_parity;
    tx.r = r;
    tx.s = s;
    assert_signed(SignedTx::try_from(TransactionV2::EIP1559(tx)).unwrap())
}

/// Builds a signed EIP-1559 transaction with a zero value and nonce.
pub fn eip1559_tx(
    gas_limit: U256,
    max_fee_per_gas: U256,
    max_priority_fee_per_gas: U256,
    action: TransactionAction,
    input: Vec<u8>,
    access_list: AccessList,
) -> SignedTx {
    eip1559_tx_with(
        gas_limit,
        max_fee_per_gas,
        max_priority_fee_per_gas,
        action,
        input,
        access_list,
        TxFields::zero(),
    )
}
