use ain_evm::bytes::Bytes;
use ethereum_types::{H160, H256, U256};
use jsonrpsee::types::ErrorObjectOwned;

use crate::call_request::CallRequest;
use crate::errors::RPCError;
use crate::utils::{format_address, format_h256, format_u256, try_get_reverted_error_or_default};

const CUSTOM_ERROR_CODE: i32 = -32001;
const REVERT_ERROR_CODE: i32 = 3;

fn call_request(data: Option<Bytes>, input: Option<Bytes>) -> CallRequest {
    CallRequest {
        data,
        input,
        ..Default::default()
    }
}

/// Builds ABI-encoded revert data: 4 byte selector, 32 byte offset, 32 byte length and the
/// reason string.
fn revert_data(reason: &str) -> Vec<u8> {
    let mut data = Vec::new();
    data.extend_from_slice(&[0x08, 0xc3, 0x79, 0xa0]); // Error(string) selector
    data.extend_from_slice(&[0u8; 31]);
    data.push(0x20); // offset of the string
    data.extend_from_slice(&[0u8; 24]);
    data.extend_from_slice(&(reason.len() as u64).to_be_bytes()); // 32 byte string length
    data.extend_from_slice(reason.as_bytes());
    data
}

fn error_code(error: &ErrorObjectOwned) -> i32 {
    error.code()
}

#[test]
fn call_request_data_precedence() {
    // `data` wins when it is the only payload set.
    let request = call_request(Some(Bytes::new(vec![1, 2, 3])), None);
    assert_eq!(request.get_data().unwrap(), Bytes::new(vec![1, 2, 3]));

    // `input` is used when `data` is absent.
    let request = call_request(None, Some(Bytes::new(vec![4, 5])));
    assert_eq!(request.get_data().unwrap(), Bytes::new(vec![4, 5]));

    // Without any payload an empty payload is returned.
    let request = call_request(None, None);
    assert_eq!(request.get_data().unwrap(), Bytes::default());

    // Both fields set at the same time are rejected.
    let request = call_request(Some(Bytes::new(vec![1])), Some(Bytes::new(vec![2])));
    let error = request.get_data().unwrap_err();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(
        error.message(),
        "Custom error: data and input fields are mutually exclusive"
    );
}

#[test]
fn call_request_effective_gas_price() {
    // Legacy transactions use the gas price as is.
    let mut request = call_request(None, None);
    request.gas_price = Some(U256::from(100));
    assert_eq!(
        request.get_effective_gas_price().unwrap(),
        Some(U256::from(100))
    );

    // A zero gas price means "no gas price".
    request.gas_price = Some(U256::zero());
    assert_eq!(request.get_effective_gas_price().unwrap(), None);

    // EIP-1559 transactions use the max fee per gas.
    let mut request = call_request(None, None);
    request.max_fee_per_gas = Some(U256::from(200));
    request.max_priority_fee_per_gas = Some(U256::from(1));
    assert_eq!(
        request.get_effective_gas_price().unwrap(),
        Some(U256::from(200))
    );

    request.max_fee_per_gas = Some(U256::zero());
    assert_eq!(request.get_effective_gas_price().unwrap(), None);

    // A gas price combined with the EIP-1559 fee fields is ambiguous.
    let mut request = call_request(None, None);
    request.gas_price = Some(U256::from(100));
    request.max_fee_per_gas = Some(U256::from(200));
    let error = request.get_effective_gas_price().unwrap_err();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(
        error.message(),
        "Custom error: both gasPrice and (maxFeePerGas or maxPriorityFeePerGas) specified"
    );

    // A max fee per gas without a priority fee is incomplete.
    let mut request = call_request(None, None);
    request.max_fee_per_gas = Some(U256::from(200));
    let error = request.get_effective_gas_price().unwrap_err();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);

    // Unknown transaction types are rejected.
    let mut request = call_request(None, None);
    request.transaction_type = Some(U256::from(3));
    let error = request.get_effective_gas_price().unwrap_err();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(
        error.message(),
        "Custom error: invalid transaction type specified"
    );
}

#[test]
fn rpc_error_mapping() {
    let error: ErrorObjectOwned = RPCError::BlockNotFound.into();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(error.message(), "Custom error: header not found");

    let error: ErrorObjectOwned = RPCError::InvalidTransactionType.into();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(
        error.message(),
        "Custom error: invalid transaction type specified"
    );

    let error: ErrorObjectOwned = RPCError::InsufficientFunds.into();
    assert_eq!(error_code(&error), CUSTOM_ERROR_CODE);
    assert_eq!(
        error.message(),
        "Custom error: insufficient funds for transfer"
    );

    // Revert errors carry their own error code and keep the revert payload as data.
    let error: ErrorObjectOwned =
        RPCError::RevertError("execution reverted".to_string(), "0x01".to_string()).into();
    assert_eq!(error_code(&error), REVERT_ERROR_CODE);
    assert_eq!(error.message(), "execution reverted");
    assert_eq!(error.data().unwrap().to_string(), "\"0x01\"");
}

#[test]
fn utils_formatting_and_revert_reason() {
    assert_eq!(
        format_h256(H256::from_low_u64_be(0x1234)),
        "0x0000000000000000000000000000000000000000000000000000000000001234"
    );
    assert_eq!(
        format_address(H160::from_low_u64_be(0xabcd)),
        "0x000000000000000000000000000000000000abcd"
    );
    assert_eq!(format_u256(U256::from(255)), "0xff");

    // A revert reason is decoded from the ABI-encoded revert data.
    assert_eq!(
        try_get_reverted_error_or_default(&revert_data("boom")),
        "execution reverted: boom"
    );

    // Truncated or non-UTF-8 payloads do not produce a reason.
    assert_eq!(try_get_reverted_error_or_default(&[]), "");
    assert_eq!(try_get_reverted_error_or_default(&[0u8; 40]), "");

    let mut invalid = revert_data("boom");
    let len = invalid.len();
    invalid[len - 1] = 0xff;
    assert_eq!(try_get_reverted_error_or_default(&invalid), "");
}
