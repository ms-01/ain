use anyhow::format_err;
use ethereum::TransactionV2;
use ethereum_types::U256;

use crate::{transaction::SignedTx, Result};

const MEMPOOL_MINIMUM_RBF_FEE_PER_GAS: U256 = U256([1_000_000_000, 0, 0, 0]);

pub fn calculate_max_tip_gas_fee(signed_tx: &SignedTx, base_fee: U256) -> Result<U256> {
    match &signed_tx.transaction {
        TransactionV2::Legacy(tx) => {
            let tip_fee_per_gas = tx.gas_price.saturating_sub(base_fee);
            tx.gas_limit.checked_mul(tip_fee_per_gas)
        }
        TransactionV2::EIP2930(tx) => {
            let tip_fee_per_gas = tx.gas_price.saturating_sub(base_fee);
            tx.gas_limit.checked_mul(tip_fee_per_gas)
        }
        TransactionV2::EIP1559(tx) => tx.gas_limit.checked_mul(tx.max_priority_fee_per_gas),
    }
    .ok_or_else(|| format_err!("calculate max tip fee failed from overflow").into())
}

/// Computes the minimum tip fee (per gas) that a replacement transaction has to pay.
///
/// `rbf_fee_increment` is the value of the governance attribute
/// `v0/evm/block/rbf_increment_fee_pct`, stored as a fraction of 100% scaled by 1e8
/// (e.g. 50_000_000 = 50%, 10_000_000 = 10%, the default being `COIN / 10`).
/// The current implementation truncates that fraction to whole percentage points
/// (`increment / 1_000_000`) before applying it to the tip fee, so values below 1%
/// are treated as 0% and only the minimum fee per gas floor applies. This comment
/// documents the current behaviour only; the truncation deviation is a known
/// deviation that is tracked separately and is not changed here.
pub fn calculate_min_rbf_tip_gas_fee(
    signed_tx: &SignedTx,
    tip_fee: U256,
    rbf_fee_increment: u64,
) -> Result<U256> {
    let mempool_increment_percentage = U256::from(rbf_fee_increment / 1_000_000);

    let incremental_fee = tip_fee
        .checked_mul(mempool_increment_percentage)
        .and_then(|f| f.checked_div(U256::from(100)))
        .ok_or_else(|| format_err!("calculate incremental fees failed from overflow"))?;
    let min_incremental_fee = signed_tx
        .gas_limit()
        .checked_mul(MEMPOOL_MINIMUM_RBF_FEE_PER_GAS)
        .ok_or_else(|| format_err!("calculate incremental fees failed from overflow"))?;
    let additional_fee = if incremental_fee < min_incremental_fee {
        min_incremental_fee
    } else {
        incremental_fee
    };

    tip_fee
        .checked_add(additional_fee)
        .ok_or_else(|| format_err!("calculate incremental fees failed from overflow").into())
}

pub fn calculate_current_prepay_gas_fee(signed_tx: &SignedTx, base_fee: U256) -> Result<U256> {
    let gas_limit = signed_tx.gas_limit();

    gas_limit
        .checked_mul(signed_tx.effective_gas_price(base_fee)?)
        .ok_or_else(|| format_err!("calculate current prepay fee failed from overflow").into())
}

pub fn calculate_max_prepay_gas_fee(signed_tx: &SignedTx) -> Result<U256> {
    match &signed_tx.transaction {
        TransactionV2::Legacy(tx) => tx.gas_limit.checked_mul(tx.gas_price),
        TransactionV2::EIP2930(tx) => tx.gas_limit.checked_mul(tx.gas_price),
        TransactionV2::EIP1559(tx) => tx.gas_limit.checked_mul(tx.max_fee_per_gas),
    }
    .ok_or_else(|| format_err!("calculate max prepay fee failed from overflow").into())
}

// Gas prices are denoted in wei
pub fn calculate_gas_fee(signed_tx: &SignedTx, used_gas: U256, base_fee: U256) -> Result<U256> {
    used_gas
        .checked_mul(signed_tx.effective_gas_price(base_fee)?)
        .ok_or_else(|| format_err!("calculate gas fee failed from overflow").into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use ethereum::{
        EIP1559Transaction, EIP1559TransactionMessage, EIP2930Transaction,
        EIP2930TransactionMessage, LegacyTransaction, TransactionAction, TransactionSignature,
    };
    use ethereum_types::{H160, H256};

    use crate::transaction::LegacyUnsignedTransaction;

    /// Arbitrary, test-only chain id. Unit tests never call
    /// `ain_cpp_imports::get_chain_id()` (the C++ side is not initialised in unit
    /// tests) and the sender recovery path is pure Rust.
    const TEST_CHAIN_ID: u64 = 1127;
    /// Fixed test secret key, only used to produce recoverable signatures.
    const TEST_SECRET_KEY: [u8; 32] = [0x01; 32];
    /// Default gas limit used across the fee tests.
    const GAS_LIMIT: u64 = 21_000;
    /// `MEMPOOL_MINIMUM_RBF_FEE_PER_GAS` (1 gwei per gas) times `GAS_LIMIT`.
    const RBF_FLOOR_FEE: u64 = GAS_LIMIT * 1_000_000_000;

    fn sign_hash(hash: H256) -> (bool, H256, H256) {
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

    /// Builds a signed legacy (EIP-155) transaction. The signature is produced with
    /// `LegacyUnsignedTransaction::sign_with_chain_id`, the signing helper exposed by
    /// the `transaction` module.
    fn legacy_tx(gas_price: u64, gas_limit: U256) -> SignedTx {
        let placeholder = LegacyTransaction {
            nonce: U256::zero(),
            gas_price: U256::from(gas_price),
            gas_limit,
            action: TransactionAction::Call(H160::zero()),
            value: U256::zero(),
            input: Vec::new(),
            signature: TransactionSignature::new(
                27,
                H256::from_low_u64_be(1),
                H256::from_low_u64_be(1),
            )
            .unwrap(),
        };
        let unsigned = LegacyUnsignedTransaction::from(&placeholder);
        let signed = unsigned
            .sign_with_chain_id(&TEST_SECRET_KEY, TEST_CHAIN_ID)
            .unwrap();
        assert_signed(signed_tx_from(TransactionV2::Legacy(signed)))
    }

    /// Builds a signed EIP-2930 transaction. The crate only exposes a signing helper
    /// for the legacy format, so the typed transaction is hashed and signed locally.
    fn eip2930_tx(gas_price: u64, gas_limit: U256) -> SignedTx {
        let mut tx = EIP2930Transaction {
            chain_id: TEST_CHAIN_ID,
            nonce: U256::zero(),
            gas_price: U256::from(gas_price),
            gas_limit,
            action: TransactionAction::Call(H160::zero()),
            value: U256::zero(),
            input: Vec::new(),
            access_list: Vec::new(),
            odd_y_parity: false,
            r: H256::zero(),
            s: H256::zero(),
        };
        let message = EIP2930TransactionMessage::from(tx.clone());
        let (odd_y_parity, r, s) = sign_hash(message.hash());
        tx.odd_y_parity = odd_y_parity;
        tx.r = r;
        tx.s = s;
        assert_signed(signed_tx_from(TransactionV2::EIP2930(tx)))
    }

    /// Builds a signed EIP-1559 transaction. See `eip2930_tx` for the signing.
    fn eip1559_tx(
        max_fee_per_gas: u64,
        max_priority_fee_per_gas: u64,
        gas_limit: U256,
    ) -> SignedTx {
        let mut tx = EIP1559Transaction {
            chain_id: TEST_CHAIN_ID,
            nonce: U256::zero(),
            max_priority_fee_per_gas: U256::from(max_priority_fee_per_gas),
            max_fee_per_gas: U256::from(max_fee_per_gas),
            gas_limit,
            action: TransactionAction::Call(H160::zero()),
            value: U256::zero(),
            input: Vec::new(),
            access_list: Vec::new(),
            odd_y_parity: false,
            r: H256::zero(),
            s: H256::zero(),
        };
        let message = EIP1559TransactionMessage::from(tx.clone());
        let (odd_y_parity, r, s) = sign_hash(message.hash());
        tx.odd_y_parity = odd_y_parity;
        tx.r = r;
        tx.s = s;
        assert_signed(signed_tx_from(TransactionV2::EIP1559(tx)))
    }

    fn signed_tx_from(transaction: TransactionV2) -> SignedTx {
        SignedTx::try_from(transaction).unwrap()
    }

    /// Fails loudly if the test helper does not produce a recoverable signature.
    fn assert_signed(signed_tx: SignedTx) -> SignedTx {
        assert_ne!(signed_tx.sender, H160::zero());
        signed_tx
    }

    #[test]
    fn test_helpers_build_recoverable_transactions() {
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(legacy.gas_limit(), U256::from(GAS_LIMIT));
        assert_eq!(legacy.gas_price(), U256::from(100));

        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(eip2930.gas_limit(), U256::from(GAS_LIMIT));
        assert_eq!(eip2930.gas_price(), U256::from(100));

        let eip1559 = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        assert_eq!(eip1559.gas_limit(), U256::from(GAS_LIMIT));
        assert_eq!(eip1559.gas_price(), U256::from(100));
        assert_eq!(eip1559.max_fee_per_gas(), Some(U256::from(100)));
        assert_eq!(eip1559.max_priority_fee_per_gas(), Some(U256::from(10)));
    }

    #[test]
    fn calculate_max_tip_gas_fee_legacy_subtracts_base_fee() {
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        // base fee below gas price
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(60)
        );
        // base fee equal to gas price
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(100)).unwrap(),
            U256::zero()
        );
        // base fee above gas price saturates to zero
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(101)).unwrap(),
            U256::zero()
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::MAX).unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn calculate_max_tip_gas_fee_eip2930_subtracts_base_fee() {
        let tx = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(60)
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(100)).unwrap(),
            U256::zero()
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(101)).unwrap(),
            U256::zero()
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::MAX).unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn calculate_max_tip_gas_fee_eip1559_ignores_base_fee() {
        let tx = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        // the base fee is not subtracted for EIP-1559 transactions
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::zero()).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(10)
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(50)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(10)
        );
        // base fee above the priority fee does not change the result
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::from(1_000)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(10)
        );
        assert_eq!(
            calculate_max_tip_gas_fee(&tx, U256::MAX).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(10)
        );
    }

    #[test]
    fn calculate_max_tip_gas_fee_overflow() {
        let legacy = legacy_tx(2, U256::MAX);
        let error = calculate_max_tip_gas_fee(&legacy, U256::zero()).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max tip fee failed from overflow"
        );

        let eip2930 = eip2930_tx(2, U256::MAX);
        let error = calculate_max_tip_gas_fee(&eip2930, U256::zero()).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max tip fee failed from overflow"
        );

        let eip1559 = eip1559_tx(2, 2, U256::MAX);
        let error = calculate_max_tip_gas_fee(&eip1559, U256::zero()).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max tip fee failed from overflow"
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_increment_zero_uses_floor() {
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 0).unwrap(),
            tip_fee + U256::from(RBF_FLOOR_FEE)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_below_one_percent_truncates_to_zero() {
        // 999_999 = 0.0999999% -> truncated to 0%, only the floor applies
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 999_999).unwrap(),
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 0).unwrap()
        );
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 999_999).unwrap(),
            tip_fee + U256::from(RBF_FLOOR_FEE)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_one_percent() {
        // 1_000_000 = 1%
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 1_000_000).unwrap(),
            tip_fee + tip_fee / U256::from(100)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_fraction_truncates_down() {
        // 1_500_000 = 1.5% -> truncated to 1%
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 1_500_000).unwrap(),
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 1_000_000).unwrap()
        );
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 1_500_000).unwrap(),
            tip_fee + tip_fee / U256::from(100)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_percentage_matrix() {
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        let cases = [
            (5_000_000u64, 5u64),
            (10_000_000, 10),
            (50_000_000, 50),
            (100_000_000, 100),
        ];
        for (increment, percentage) in cases {
            assert_eq!(
                calculate_min_rbf_tip_gas_fee(&tx, tip_fee, increment).unwrap(),
                tip_fee + tip_fee * U256::from(percentage) / U256::from(100),
                "unexpected fee for increment {increment}"
            );
        }
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_floor_wins_for_small_tip_fee() {
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        // 10% of 1 gwei is far below the floor of 21_000 * 1 gwei
        let tip_fee = U256::from(1_000_000_000);
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&tx, tip_fee, 10_000_000).unwrap(),
            tip_fee + U256::from(RBF_FLOOR_FEE)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_floor_is_based_on_gas_limit() {
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        let small = legacy_tx(100, U256::from(GAS_LIMIT));
        let large = legacy_tx(100, U256::from(GAS_LIMIT * 2));
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&small, tip_fee, 0).unwrap(),
            tip_fee + U256::from(RBF_FLOOR_FEE)
        );
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&large, tip_fee, 0).unwrap(),
            tip_fee + U256::from(RBF_FLOOR_FEE * 2)
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_is_transaction_type_independent() {
        let tip_fee = U256::from(1_000_000_000_000_000_000u64);
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        let eip1559 = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        let expected = calculate_min_rbf_tip_gas_fee(&legacy, tip_fee, 10_000_000).unwrap();
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&eip2930, tip_fee, 10_000_000).unwrap(),
            expected
        );
        assert_eq!(
            calculate_min_rbf_tip_gas_fee(&eip1559, tip_fee, 10_000_000).unwrap(),
            expected
        );
    }

    #[test]
    fn calculate_min_rbf_tip_gas_fee_overflow() {
        let tx = legacy_tx(100, U256::from(GAS_LIMIT));
        // tip fee * percentage overflows
        let error = calculate_min_rbf_tip_gas_fee(&tx, U256::MAX, 100_000_000).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate incremental fees failed from overflow"
        );

        // gas limit * minimum fee per gas overflows
        let huge_gas_limit = legacy_tx(100, U256::MAX);
        let error = calculate_min_rbf_tip_gas_fee(&huge_gas_limit, U256::from(1_000_000_000), 0)
            .unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate incremental fees failed from overflow"
        );

        // tip fee + additional fee overflows
        let error = calculate_min_rbf_tip_gas_fee(&tx, U256::MAX, 0).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate incremental fees failed from overflow"
        );
    }

    #[test]
    fn calculate_current_prepay_gas_fee_legacy_and_eip2930_ignore_base_fee() {
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_current_prepay_gas_fee(&legacy, U256::zero()).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
        assert_eq!(
            calculate_current_prepay_gas_fee(&legacy, U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
        assert_eq!(
            calculate_current_prepay_gas_fee(&legacy, U256::MAX).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );

        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_current_prepay_gas_fee(&eip2930, U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
        assert_eq!(
            calculate_current_prepay_gas_fee(&eip2930, U256::MAX).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
    }

    #[test]
    fn calculate_current_prepay_gas_fee_eip1559_uses_effective_gas_price() {
        let tx = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        // base fee + priority fee below the max fee
        assert_eq!(
            calculate_current_prepay_gas_fee(&tx, U256::zero()).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(10)
        );
        assert_eq!(
            calculate_current_prepay_gas_fee(&tx, U256::from(5)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(15)
        );
        // boundary: base fee + priority fee is exactly the max fee
        assert_eq!(
            calculate_current_prepay_gas_fee(&tx, U256::from(90)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
        // base fee + priority fee above the max fee
        assert_eq!(
            calculate_current_prepay_gas_fee(&tx, U256::from(95)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
    }

    #[test]
    fn calculate_current_prepay_gas_fee_overflow() {
        // base fee + priority fee overflows
        let tx = eip1559_tx(100, 100, U256::from(GAS_LIMIT));
        let error = calculate_current_prepay_gas_fee(&tx, U256::MAX).unwrap_err();
        assert_eq!(error.to_string(), "effective_gas_price overflow");

        // gas limit * gas price overflows
        let legacy = legacy_tx(2, U256::MAX);
        let error = calculate_current_prepay_gas_fee(&legacy, U256::zero()).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate current prepay fee failed from overflow"
        );
    }

    #[test]
    fn test_calculate_max_prepay_gas_fee() {
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_max_prepay_gas_fee(&legacy).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );

        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_max_prepay_gas_fee(&eip2930).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );

        // EIP-1559 uses the max fee per gas, not the priority fee
        let eip1559 = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_max_prepay_gas_fee(&eip1559).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
    }

    #[test]
    fn calculate_max_prepay_gas_fee_overflow() {
        let legacy = legacy_tx(2, U256::MAX);
        let error = calculate_max_prepay_gas_fee(&legacy).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max prepay fee failed from overflow"
        );

        let eip2930 = eip2930_tx(2, U256::MAX);
        let error = calculate_max_prepay_gas_fee(&eip2930).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max prepay fee failed from overflow"
        );

        let eip1559 = eip1559_tx(2, 1, U256::MAX);
        let error = calculate_max_prepay_gas_fee(&eip1559).unwrap_err();
        assert_eq!(
            error.to_string(),
            "calculate max prepay fee failed from overflow"
        );
    }

    #[test]
    fn calculate_gas_fee_with_zero_used_gas() {
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&legacy, U256::zero(), U256::from(40)).unwrap(),
            U256::zero()
        );

        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&eip2930, U256::zero(), U256::from(40)).unwrap(),
            U256::zero()
        );

        let eip1559 = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&eip1559, U256::zero(), U256::from(40)).unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn calculate_gas_fee_uses_effective_gas_price() {
        let legacy = legacy_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&legacy, U256::from(GAS_LIMIT), U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );

        let eip2930 = eip2930_tx(100, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&eip2930, U256::from(GAS_LIMIT), U256::from(40)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );

        let eip1559 = eip1559_tx(100, 10, U256::from(GAS_LIMIT));
        assert_eq!(
            calculate_gas_fee(&eip1559, U256::from(GAS_LIMIT), U256::from(5)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(15)
        );
        assert_eq!(
            calculate_gas_fee(&eip1559, U256::from(GAS_LIMIT), U256::from(90)).unwrap(),
            U256::from(GAS_LIMIT) * U256::from(100)
        );
    }

    #[test]
    fn calculate_gas_fee_overflow() {
        let legacy = legacy_tx(2, U256::from(GAS_LIMIT));
        let error = calculate_gas_fee(&legacy, U256::MAX, U256::zero()).unwrap_err();
        assert_eq!(error.to_string(), "calculate gas fee failed from overflow");

        // base fee + priority fee overflows
        let eip1559 = eip1559_tx(100, 100, U256::from(GAS_LIMIT));
        let error = calculate_gas_fee(&eip1559, U256::from(GAS_LIMIT), U256::MAX).unwrap_err();
        assert_eq!(error.to_string(), "effective_gas_price overflow");
    }
}
