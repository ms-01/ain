use std::collections::BTreeMap;

use ain_contracts::{dst20_address_from_token_id, validate_split_tokens_input, TokenSplitParams};
use ain_cpp_imports::{split_tokens_from_evm, TokenAmount};
use anyhow::format_err;
use ethereum_types::{H160, H256, U256};
use evm::{
    backend::Apply,
    executor::stack::{PrecompileFailure, PrecompileHandle, PrecompileOutput},
    ExitError, ExitSucceed,
};
use log::trace;

use super::DVMStatePrecompile;
use crate::{
    contract::{get_address_storage_index, u256_to_h256},
    precompiles::PrecompileResult,
    weiamount::{try_from_satoshi, WeiAmount},
    Result,
};

pub struct TokenSplit;

impl TokenSplit {
    const GAS_COST: u64 = 1000;

    /// Charges the precompile gas and decodes the ABI encoded input.
    ///
    /// The gas is charged *before* the input is validated (current behaviour).
    fn charge_and_parse(
        handle: &mut impl PrecompileHandle,
    ) -> core::result::Result<TokenSplitParams, PrecompileFailure> {
        handle.record_cost(TokenSplit::GAS_COST)?;

        TokenSplit::parse_input(handle.input())
    }

    /// Decodes `migrateTokensOnAddress(address sender, address tokenContract, uint256 amount)`.
    ///
    /// The 4 byte function selector is not part of the precompile input. Trailing data is
    /// ignored and only the lower 20 bytes of an address word are used (current behaviour).
    fn parse_input(input: &[u8]) -> core::result::Result<TokenSplitParams, PrecompileFailure> {
        validate_split_tokens_input(input).map_err(|e| PrecompileFailure::Error {
            exit_status: ExitError::Other(e.to_string().into()),
        })
    }

    /// Converts the given wei amount into satoshi.
    ///
    /// Fails if the amount is outside of the money range. Amounts smaller than one satoshi are
    /// truncated towards zero (current behaviour).
    fn to_satoshi(amount: U256) -> core::result::Result<U256, PrecompileFailure> {
        let Ok(amount) = WeiAmount(amount).to_satoshi() else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Failed to convert amount into Sats".into()),
            });
        };

        Ok(amount)
    }

    /// Derives the token id from the token contract address.
    ///
    /// The token contract address is caller supplied (calldata), hence it can be any address.
    /// Addresses below the base address (`0xdf00…00`) would underflow the `U256` subtraction,
    /// so the subtraction is done with `checked_sub` and an underflow is reported as a
    /// precompile error instead of panicking (a panic aborts the node).
    ///
    /// The id mapping of all addresses at or above the base address is unchanged:
    /// `(contract_value - contract_base).low_u64() as u32`.
    fn token_id_from_contract(contract: H160) -> core::result::Result<u32, PrecompileFailure> {
        let contract_value = U256::from_big_endian(contract.as_bytes());
        let Ok(contract_base) =
            U256::from_str_radix("df00000000000000000000000000000000000000", 16)
        else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Failed to convert base amount into U256".into()),
            });
        };

        let Some(offset) = contract_value.checked_sub(contract_base) else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Invalid token contract address".into()),
            });
        };

        Ok(offset.low_u64() as u32)
    }

    /// Encodes the 64 byte precompile output: the new DST20 contract address (left padded)
    /// followed by the new amount (big endian).
    fn encode_output(new_contract: H160, amount: U256) -> Vec<u8> {
        let mut bytes = [0u8; 64];
        bytes[12..32].copy_from_slice(new_contract.as_bytes());
        amount.to_big_endian(&mut bytes[32..]);
        bytes.to_vec()
    }

    /// Finalizes the precompile once the token migration call has returned.
    ///
    /// The outcome of the (C++) migration call is injected here, so that the Rust side of the
    /// precompile can be unit tested without the node state.
    fn finish(
        handle: &mut impl PrecompileHandle,
        original_contract: H160,
        old_token_id: u32,
        split_ok: bool,
        new_id: u32,
        new_amount: u64,
    ) -> PrecompileResult {
        if !split_ok {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Failed to split tokens".into()),
            });
        }

        let Ok(converted_amount) = try_from_satoshi(U256::from(new_amount)) else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Failed to convert new Sats amount into Wei".into()),
            });
        };

        let Ok(new_contract) = dst20_address_from_token_id(new_id.into()) else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Error getting DST20 from new_amount.id".into()),
            });
        };

        let output = TokenSplit::encode_output(new_contract, converted_amount.0);

        // No split took place
        if new_id == old_token_id {
            return Ok(PrecompileOutput {
                exit_status: ExitSucceed::Returned,
                state_changes: None,
                output,
            });
        }

        let Ok(storage) = get_new_contract_storage_update(
            handle,
            original_contract,
            new_contract,
            converted_amount.0,
        ) else {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::Other("Error getting storage update".into()),
            });
        };

        let new_contract_state_changes = Apply::Modify {
            address: new_contract,
            basic: handle.basic(new_contract),
            code: None,
            storage,
            reset_storage: false,
        };

        Ok(PrecompileOutput {
            exit_status: ExitSucceed::Returned,
            state_changes: Some(vec![new_contract_state_changes]),
            output,
        })
    }
}

impl DVMStatePrecompile for TokenSplit {
    fn execute(handle: &mut impl PrecompileHandle, mnview_ptr: usize) -> PrecompileResult {
        let TokenSplitParams {
            sender,
            token_contract: original_contract,
            amount: input_amount,
            ..
        } = TokenSplit::charge_and_parse(handle)?;

        trace!("[TokenSplit] sender {sender:x}, original_contract {original_contract:x}, input_amount : {input_amount:x}");

        let amount = TokenSplit::to_satoshi(input_amount)?;
        let old_token_id = TokenSplit::token_id_from_contract(original_contract)?;

        let old_amount = TokenAmount {
            id: old_token_id,
            amount: amount.low_u64(),
        };

        let mut new_amount = TokenAmount { id: 0, amount: 0 };
        // The only reference to the C++ node. It is kept inside this generic function on
        // purpose: the function is only code generated in the crate that instantiates it
        // (the node), hence the Rust only test binaries never reference the C++ symbol.
        let res = split_tokens_from_evm(mnview_ptr, old_amount, &mut new_amount);

        TokenSplit::finish(
            handle,
            original_contract,
            old_token_id,
            res,
            new_amount.id,
            new_amount.amount,
        )
    }
}

fn get_new_contract_storage_update(
    handle: &mut impl PrecompileHandle,
    sender: H160,
    contract: H160,
    amount: U256,
) -> Result<BTreeMap<H256, H256>> {
    let contract_balance_storage_index = get_address_storage_index(H256::zero(), sender);
    let sender_balance = U256::from(
        handle
            .storage(contract, contract_balance_storage_index)
            .as_bytes(),
    );

    let new_sender_balance = sender_balance
        .checked_add(amount)
        .ok_or_else(|| format_err!("Total supply overflow/underflow"))?;

    let contract_allowance_storage_index =
        get_address_storage_index(H256::from_low_u64_be(1), sender);
    let sender_allowance = U256::from(
        handle
            .storage(contract, contract_allowance_storage_index)
            .as_bytes(),
    );

    let new_sender_allowance = sender_allowance
        .checked_add(amount)
        .ok_or_else(|| format_err!("Total supply overflow/underflow"))?;

    let total_supply_index = H256::from_low_u64_be(2);
    let total_supply = U256::from(handle.storage(contract, total_supply_index).as_bytes());

    let new_total_supply = total_supply
        .checked_add(amount)
        .ok_or_else(|| format_err!("Total supply overflow/underflow"))?;

    Ok(BTreeMap::from([
        (
            contract_balance_storage_index,
            u256_to_h256(new_sender_balance),
        ),
        (
            contract_allowance_storage_index,
            u256_to_h256(new_sender_allowance),
        ),
        (total_supply_index, u256_to_h256(new_total_supply)),
    ]))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::precompiles::test_vector_support::MockHandle;
    use evm::Context;

    /// The maximum money amount in wei: `MAX_MONEY_SATS * 1e10`.
    const MAX_MONEY_WEI: u128 = 1_200_000_000_000_000_000_000_000_000;
    /// The maximum money amount in satoshi: `MAX_MONEY_SATS`.
    const MAX_MONEY_SATS: u128 = 120_000_000_000_000_000;

    fn context() -> Context {
        Context {
            address: Default::default(),
            caller: Default::default(),
            apparent_value: From::from(0),
        }
    }

    fn word(value: U256) -> [u8; 32] {
        let mut buf = [0u8; 32];
        value.to_big_endian(&mut buf);
        buf
    }

    fn address_word(address: H160) -> [u8; 32] {
        let mut buf = [0u8; 32];
        buf[12..32].copy_from_slice(address.as_bytes());
        buf
    }

    /// ABI encodes `migrateTokensOnAddress(address sender, address tokenContract, uint256 amount)`.
    /// The 4 byte function selector is not part of the precompile input.
    fn split_input(sender: H160, token_contract: H160, amount: U256) -> Vec<u8> {
        let mut input = Vec::new();
        input.extend_from_slice(&address_word(sender));
        input.extend_from_slice(&address_word(token_contract));
        input.extend_from_slice(&word(amount));
        input
    }

    fn contract_address(token_id: u64) -> H160 {
        H160::from_slice(&hex::decode(format!("df{token_id:0>38x}")).expect("valid hex address"))
    }

    fn dst20_address(token_id: u64) -> H160 {
        H160::from_slice(&hex::decode(format!("ff{token_id:0>38x}")).expect("valid hex address"))
    }

    fn handle(input: Vec<u8>) -> MockHandle {
        MockHandle::new(input, Some(10_000_000), context())
    }

    fn error_message(failure: &PrecompileFailure) -> String {
        match failure {
            PrecompileFailure::Error { exit_status } => match exit_status {
                ExitError::Other(msg) => msg.to_string(),
                other => format!("{other:?}"),
            },
            PrecompileFailure::Revert { exit_status, .. } => format!("revert: {exit_status:?}"),
            PrecompileFailure::Fatal { exit_status } => format!("fatal: {exit_status:?}"),
        }
    }

    // --- behaviour pinning: input parsing -----------------------------------------------

    #[test]
    fn test_parse_input_valid() {
        let sender = H160::repeat_byte(0x11);
        let token_contract = contract_address(7);
        let input = split_input(sender, token_contract, U256::from(1234u64));

        let params = TokenSplit::parse_input(&input).expect("valid input");

        assert_eq!(params.sender, sender);
        assert_eq!(params.token_contract, token_contract);
        assert_eq!(params.amount, U256::from(1234u64));
    }

    #[test]
    fn test_parse_input_too_short() {
        let input = split_input(H160::zero(), contract_address(1), U256::zero())[..95].to_vec();

        let failure = match TokenSplit::parse_input(&input) {
            Ok(_) => panic!("the input must be invalid"),
            Err(failure) => failure,
        };

        assert_eq!(error_message(&failure), "Invalid data");
    }

    #[test]
    fn test_parse_input_ignores_trailing_data() {
        let sender = H160::repeat_byte(0x22);
        let token_contract = contract_address(3);
        let mut input = split_input(sender, token_contract, U256::from(42u64));
        input.extend_from_slice(&[0xff; 16]);

        let params = TokenSplit::parse_input(&input).expect("trailing data is ignored");

        assert_eq!(params.sender, sender);
        assert_eq!(params.token_contract, token_contract);
        assert_eq!(params.amount, U256::from(42u64));
    }

    #[test]
    fn test_parse_input_ignores_dirty_address_bits() {
        let sender = H160::repeat_byte(0x33);
        let token_contract = contract_address(5);
        let mut input = split_input(sender, token_contract, U256::from(7u64));
        // Dirty the upper 12 bytes of the sender word.
        for byte in input[..12].iter_mut() {
            *byte = 0xff;
        }

        let params = TokenSplit::parse_input(&input).expect("dirty address bits are ignored");

        assert_eq!(params.sender, sender);
        assert_eq!(params.token_contract, token_contract);
    }

    // --- behaviour pinning: gas accounting ----------------------------------------------

    #[test]
    fn test_gas_is_charged_before_the_input_is_validated() {
        let mut handle = handle(vec![0u8; 10]);

        let failure = match TokenSplit::charge_and_parse(&mut handle) {
            Ok(_) => panic!("the input must be invalid"),
            Err(failure) => failure,
        };

        assert_eq!(error_message(&failure), "Invalid data");
        // The gas is charged before the validation (current behaviour).
        assert_eq!(handle.gas_used, TokenSplit::GAS_COST);
        assert_eq!(handle.gas_used, 1000);
    }

    #[test]
    fn test_gas_is_charged_for_valid_input() {
        let mut handle = handle(split_input(H160::zero(), contract_address(1), U256::zero()));

        TokenSplit::charge_and_parse(&mut handle).expect("valid input");

        assert_eq!(handle.gas_used, 1000);
    }

    // --- behaviour pinning: wei -> satoshi conversion ------------------------------------

    #[test]
    fn test_to_satoshi_zero() {
        assert_eq!(
            TokenSplit::to_satoshi(U256::zero()).expect("zero amount"),
            U256::zero()
        );
    }

    #[test]
    fn test_to_satoshi_truncates_towards_zero() {
        // 1 wei is less than one satoshi (1 satoshi = 1e10 wei) and is truncated to zero.
        assert_eq!(
            TokenSplit::to_satoshi(U256::from(1u64)).expect("small amount"),
            U256::zero()
        );
        // 1.5 satoshi are truncated to 1 satoshi.
        assert_eq!(
            TokenSplit::to_satoshi(U256::from(15_000_000_000u64)).expect("small amount"),
            U256::from(1u64)
        );
    }

    #[test]
    fn test_to_satoshi_money_range_boundary() {
        // Exactly the maximum money amount is inside the money range.
        assert_eq!(
            TokenSplit::to_satoshi(U256::from(MAX_MONEY_WEI)).expect("max money"),
            U256::from(MAX_MONEY_SATS)
        );

        // One wei more is outside of the money range.
        let failure = match TokenSplit::to_satoshi(U256::from(MAX_MONEY_WEI) + U256::one()) {
            Ok(_) => panic!("the amount must be out of range"),
            Err(failure) => failure,
        };
        assert_eq!(
            error_message(&failure),
            "Failed to convert amount into Sats"
        );

        // The maximum U256 value is outside of the money range as well.
        let failure = match TokenSplit::to_satoshi(U256::MAX) {
            Ok(_) => panic!("the amount must be out of range"),
            Err(failure) => failure,
        };
        assert_eq!(
            error_message(&failure),
            "Failed to convert amount into Sats"
        );
    }

    // --- behaviour pinning: token id derivation ------------------------------------------

    #[test]
    fn test_token_id_from_contract() {
        assert_eq!(
            TokenSplit::token_id_from_contract(contract_address(0)).expect("base address"),
            0
        );
        assert_eq!(
            TokenSplit::token_id_from_contract(contract_address(1)).expect("token id"),
            1
        );
        assert_eq!(
            TokenSplit::token_id_from_contract(contract_address(10)).expect("token id"),
            10
        );
        assert_eq!(
            TokenSplit::token_id_from_contract(contract_address(255)).expect("token id"),
            255
        );
    }

    #[test]
    fn test_token_id_from_contract_boundaries() {
        assert_eq!(
            TokenSplit::token_id_from_contract(contract_address(0)).expect("base address"),
            0
        );
        assert_eq!(
            TokenSplit::token_id_from_contract(dst20_address(1)).expect("dst20 address"),
            1
        );
        assert_eq!(
            TokenSplit::token_id_from_contract(dst20_address(255)).expect("dst20 address"),
            255
        );
    }

    #[test]
    fn test_token_id_from_contract_below_base_address_fails() {
        // An address below the base address underflows the subtraction. It has to be reported
        // as an error and must not panic (a panic aborts the node).
        let addresses = [
            H160::zero(),
            H160::from_low_u64_be(1),
            H160::from_low_u64_be(0xdeadbeef),
            H160::repeat_byte(0xde),
        ];

        for address in addresses {
            let result = std::panic::catch_unwind(|| TokenSplit::token_id_from_contract(address));

            let outcome = result.expect("the precompile must not panic");
            let failure = match outcome {
                Ok(_) => panic!("the address must be rejected"),
                Err(failure) => failure,
            };

            assert_eq!(error_message(&failure), "Invalid token contract address");
        }
    }

    // --- behaviour pinning: output layout -----------------------------------------------

    #[test]
    fn test_encode_output_layout() {
        let new_contract = dst20_address(5);
        let amount = U256::from(0x0102u64);

        let output = TokenSplit::encode_output(new_contract, amount);

        assert_eq!(output.len(), 64);
        assert_eq!(&output[..12], &[0u8; 12]);
        assert_eq!(&output[12..32], new_contract.as_bytes());
        assert_eq!(&output[32..64], &word(amount));
    }

    // --- seam level: the outcome of the C++ migration call is injected -------------------

    #[test]
    fn test_finish_migration_failed() {
        let mut handle = handle(vec![]);

        let failure = TokenSplit::finish(&mut handle, contract_address(1), 1, false, 0, 0)
            .expect_err("migration failed");

        assert_eq!(error_message(&failure), "Failed to split tokens");
    }

    #[test]
    fn test_finish_new_amount_is_converted_to_wei() {
        // The new amount is returned in satoshi and converted into wei. The conversion can only
        // fail on a multiplication overflow, which is not reachable for a `u64` amount
        // (the largest possible value is ~1.8e19 satoshi => ~1.8e29 wei).
        let mut handle = handle(vec![]);

        let result = TokenSplit::finish(&mut handle, contract_address(1), 1, true, 2, 100)
            .expect("successful migration");

        assert_eq!(result.output.len(), 64);
        assert_eq!(
            &result.output[32..64],
            &word(U256::from(100u64) * U256::from(10_000_000_000u64))
        );
    }

    #[test]
    fn test_finish_no_split_took_place() {
        let mut handle = handle(vec![]);

        let result =
            TokenSplit::finish(&mut handle, contract_address(1), 1, true, 1, 0).expect("no split");

        assert_eq!(result.output.len(), 64);
        assert!(result.state_changes.is_none());
        // The output contains the (unchanged) DST20 address of the token.
        assert_eq!(&result.output[12..32], dst20_address(1).as_bytes());
    }

    #[test]
    fn test_finish_split_updates_the_storage_of_the_new_contract() {
        let original_contract = contract_address(1);
        let new_contract = dst20_address(2);
        // The new amount is returned in satoshi and is added to the storage in wei.
        let new_amount_sats = 500u64;
        let amount_wei = U256::from(new_amount_sats) * U256::from(10_000_000_000u64);

        let balance_index = get_address_storage_index(H256::zero(), original_contract);
        let allowance_index =
            get_address_storage_index(H256::from_low_u64_be(1), original_contract);
        let total_supply_index = H256::from_low_u64_be(2);

        let mut handle = handle(vec![])
            .with_storage_entry(new_contract, balance_index, u256_to_h256(U256::from(10u64)))
            .with_storage_entry(
                new_contract,
                allowance_index,
                u256_to_h256(U256::from(20u64)),
            )
            .with_storage_entry(
                new_contract,
                total_supply_index,
                u256_to_h256(U256::from(30u64)),
            );

        let result =
            TokenSplit::finish(&mut handle, original_contract, 1, true, 2, new_amount_sats)
                .expect("successful migration");

        let state_changes = result.state_changes.expect("state changes");
        assert_eq!(state_changes.len(), 1);
        match &state_changes[0] {
            Apply::Modify {
                address,
                storage,
                reset_storage,
                ..
            } => {
                assert_eq!(*address, new_contract);
                assert!(!reset_storage);
                assert_eq!(storage.len(), 3);
                assert_eq!(
                    storage.get(&balance_index),
                    Some(&u256_to_h256(U256::from(10u64) + amount_wei))
                );
                assert_eq!(
                    storage.get(&allowance_index),
                    Some(&u256_to_h256(U256::from(20u64) + amount_wei))
                );
                assert_eq!(
                    storage.get(&total_supply_index),
                    Some(&u256_to_h256(U256::from(30u64) + amount_wei))
                );
            }
            other => panic!("unexpected state change: {other:?}"),
        }
    }

    #[test]
    fn test_finish_storage_update_overflow() {
        let original_contract = contract_address(1);
        let new_contract = dst20_address(2);
        let balance_index = get_address_storage_index(H256::zero(), original_contract);

        let mut handle =
            handle(vec![]).with_storage_entry(new_contract, balance_index, u256_to_h256(U256::MAX));

        let failure = TokenSplit::finish(&mut handle, original_contract, 1, true, 2, 1)
            .expect_err("storage overflow");

        assert_eq!(error_message(&failure), "Error getting storage update");
    }
}
