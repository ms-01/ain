use anyhow::format_err;
use ethereum::TransactionAction;
use evm::{
    gasometer::{call_transaction_cost, create_transaction_cost, Gasometer, TransactionCost},
    Config,
};
use log::debug;

use crate::{transaction::SignedTx, Result};

fn get_tx_cost(signed_tx: &SignedTx) -> TransactionCost {
    let access_list = signed_tx
        .access_list()
        .into_iter()
        .map(|x| (x.address, x.storage_keys))
        .collect::<Vec<_>>();

    match signed_tx.action() {
        TransactionAction::Call(_) => call_transaction_cost(signed_tx.data(), &access_list),
        TransactionAction::Create => create_transaction_cost(signed_tx.data(), &access_list),
    }
}

pub fn check_tx_intrinsic_gas(signed_tx: &SignedTx) -> Result<()> {
    const CONFIG: Config = Config::shanghai();
    let mut gasometer = Gasometer::new(u64::try_from(signed_tx.gas_limit())?, &CONFIG);

    let tx_cost = get_tx_cost(signed_tx);
    match gasometer.record_transaction(tx_cost) {
        Err(_) => {
            debug!("[check_tx_intrinsic_gas] gas limit is below the minimum gas per tx");
            Err(format_err!("gas limit is below the minimum gas per tx").into())
        }
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use ethereum::TransactionAction;
    use ethereum_types::{H160, H256, U256};
    use evm::Config;

    use crate::{
        gas::check_tx_intrinsic_gas,
        transaction::{
            test_helpers::{eip1559_tx, eip2930_tx, legacy_tx},
            SignedTx,
        },
    };

    /// The rejection message is part of the RPC visible behaviour of the node.
    const REJECTION_MESSAGE: &str = "gas limit is below the minimum gas per tx";

    /// Intrinsic gas of a plain call transaction (EIP-155, Shanghai).
    const CALL_BASE_GAS: u64 = 21_000;
    /// Intrinsic gas of a contract creation transaction (EIP-155, Shanghai).
    const CREATE_BASE_GAS: u64 = 53_000;
    /// Gas per zero byte of calldata.
    const ZERO_DATA_GAS: u64 = 4;
    /// Gas per non-zero byte of calldata.
    const NON_ZERO_DATA_GAS: u64 = 16;
    /// Gas per address of the access list.
    const ACCESS_LIST_ADDRESS_GAS: u64 = 2_400;
    /// Gas per storage key of the access list.
    const ACCESS_LIST_STORAGE_KEY_GAS: u64 = 1_900;
    /// EIP-3860 initcode word cost.
    const INITCODE_WORD_COST: u64 = 2;
    /// EIP-3860 initcode word size.
    const INITCODE_WORD_SIZE: usize = 32;

    /// EIP-3860 initcode cost: `INITCODE_WORD_COST * ceil(len / 32)`.
    fn initcode_cost(len: usize) -> u64 {
        INITCODE_WORD_COST * ((len as u64 + 31) / 32)
    }

    /// Asserts the boundary semantics of the intrinsic gas gate: a transaction with
    /// exactly `expected` gas is accepted, a transaction with one gas less is
    /// rejected with the stable rejection message.
    fn assert_intrinsic_gas(build: impl Fn(U256) -> SignedTx, expected: u64) {
        let accepted = build(U256::from(expected));
        assert!(
            check_tx_intrinsic_gas(&accepted).is_ok(),
            "expected {expected} gas to be accepted"
        );

        let rejected = build(U256::from(expected - 1));
        let error = check_tx_intrinsic_gas(&rejected).unwrap_err();
        assert_eq!(error.to_string(), REJECTION_MESSAGE);
    }

    #[test]
    fn config_constants_are_pinned() {
        // Pins the production fork configuration used by `check_tx_intrinsic_gas`.
        let config = Config::shanghai();
        assert_eq!(config.gas_transaction_call, CALL_BASE_GAS);
        assert_eq!(config.gas_transaction_create, CREATE_BASE_GAS);
        assert_eq!(config.gas_transaction_zero_data, ZERO_DATA_GAS);
        assert_eq!(config.gas_transaction_non_zero_data, NON_ZERO_DATA_GAS);
        assert_eq!(config.gas_access_list_address, ACCESS_LIST_ADDRESS_GAS);
        assert_eq!(
            config.gas_access_list_storage_key,
            ACCESS_LIST_STORAGE_KEY_GAS
        );
        assert_eq!(config.max_initcode_size, Some(0xC000));
    }

    #[test]
    fn plain_transfer_costs_21000() {
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    Vec::new(),
                )
            },
            CALL_BASE_GAS,
        );
    }

    #[test]
    fn create_costs_more_than_call() {
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Create,
                    Vec::new(),
                )
            },
            CREATE_BASE_GAS,
        );

        // A creation is strictly more expensive than a call.
        let too_low = legacy_tx(
            U256::from(CALL_BASE_GAS),
            U256::from(100),
            TransactionAction::Create,
            Vec::new(),
        );
        let error = check_tx_intrinsic_gas(&too_low).unwrap_err();
        assert_eq!(error.to_string(), REJECTION_MESSAGE);
    }

    #[test]
    fn calldata_zero_bytes_cost_4_each() {
        let input = vec![0u8; 32];
        let expected = CALL_BASE_GAS + 32 * ZERO_DATA_GAS;
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    input.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn calldata_non_zero_bytes_cost_16_each() {
        let input = vec![1u8; 32];
        let expected = CALL_BASE_GAS + 32 * NON_ZERO_DATA_GAS;
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    input.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn mixed_calldata_cost() {
        let input = vec![0u8, 1u8];
        let expected = CALL_BASE_GAS + ZERO_DATA_GAS + NON_ZERO_DATA_GAS;
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    input.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn access_list_address_costs_2400() {
        let access_list = vec![ethereum::AccessListItem {
            address: H160::from_low_u64_be(1),
            storage_keys: Vec::new(),
        }];
        let expected = CALL_BASE_GAS + ACCESS_LIST_ADDRESS_GAS;
        assert_intrinsic_gas(
            |gas_limit| {
                eip2930_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    Vec::new(),
                    access_list.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn access_list_storage_key_costs_1900() {
        let access_list = vec![ethereum::AccessListItem {
            address: H160::from_low_u64_be(1),
            storage_keys: vec![H256::from_low_u64_be(1)],
        }];
        let expected = CALL_BASE_GAS + ACCESS_LIST_ADDRESS_GAS + ACCESS_LIST_STORAGE_KEY_GAS;
        assert_intrinsic_gas(
            |gas_limit| {
                eip2930_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Call(H160::zero()),
                    Vec::new(),
                    access_list.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn create_with_zero_initcode_32_bytes() {
        let initcode = vec![0u8; INITCODE_WORD_SIZE];
        let expected =
            CREATE_BASE_GAS + initcode.len() as u64 * ZERO_DATA_GAS + initcode_cost(initcode.len());
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Create,
                    initcode.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn create_with_non_zero_initcode_32_bytes() {
        let initcode = vec![1u8; INITCODE_WORD_SIZE];
        let expected = CREATE_BASE_GAS
            + initcode.len() as u64 * NON_ZERO_DATA_GAS
            + initcode_cost(initcode.len());
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Create,
                    initcode.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn create_with_zero_initcode_33_bytes() {
        let initcode = vec![0u8; INITCODE_WORD_SIZE + 1];
        let expected =
            CREATE_BASE_GAS + initcode.len() as u64 * ZERO_DATA_GAS + initcode_cost(initcode.len());
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Create,
                    initcode.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn create_with_non_zero_initcode_33_bytes() {
        let initcode = vec![1u8; INITCODE_WORD_SIZE + 1];
        let expected = CREATE_BASE_GAS
            + initcode.len() as u64 * NON_ZERO_DATA_GAS
            + initcode_cost(initcode.len());
        assert_intrinsic_gas(
            |gas_limit| {
                legacy_tx(
                    gas_limit,
                    U256::from(100),
                    TransactionAction::Create,
                    initcode.clone(),
                )
            },
            expected,
        );
    }

    #[test]
    fn legacy_transaction_has_no_access_list() {
        let tx = legacy_tx(
            U256::from(CALL_BASE_GAS),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert!(tx.access_list().is_empty());
        assert!(check_tx_intrinsic_gas(&tx).is_ok());
    }

    #[test]
    fn eip1559_transaction_uses_the_same_intrinsic_gas() {
        assert_intrinsic_gas(
            |gas_limit| {
                eip1559_tx(
                    gas_limit,
                    U256::from(100),
                    U256::from(10),
                    TransactionAction::Call(H160::zero()),
                    Vec::new(),
                    Vec::new(),
                )
            },
            CALL_BASE_GAS,
        );
    }

    #[test]
    fn gas_limit_above_u64_max_is_rejected() {
        let tx = legacy_tx(
            U256::MAX,
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        let error = check_tx_intrinsic_gas(&tx).unwrap_err();
        assert_eq!(error.to_string(), "integer overflow when casting to u64");
    }

    #[test]
    fn rejection_message_is_stable() {
        let tx = legacy_tx(
            U256::from(CALL_BASE_GAS - 1),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        let error = check_tx_intrinsic_gas(&tx).unwrap_err();
        assert_eq!(error.to_string(), REJECTION_MESSAGE);
    }
}
