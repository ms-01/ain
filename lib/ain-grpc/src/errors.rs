use ain_evm::EVMError;
use ethereum_types::H256;
use jsonrpsee::{
    core::to_json_raw_value,
    types::{ErrorObject, ErrorObjectOwned},
};

pub enum RPCError {
    AccountError,
    BlockNotFound,
    DebugNotEnabled,
    Error(Box<dyn std::error::Error>),
    EvmError(EVMError),
    GasCapTooLow(u64),
    InsufficientFunds,
    InvalidBlockInput,
    InvalidDataInput,
    InvalidGasPrice,
    InvalidTransactionMessage,
    InvalidTransactionType,
    NonceCacheError,
    ReceiptNotFound(H256),
    RevertError(String, String),
    StateRootNotFound,
    TraceNotEnabled,
    TracingParamError([u8; 16]),
    TxExecutionFailed,
    TxNotFound(H256),
    ValueOverflow,
    ValueUnderflow,
    DivideError,
}

impl From<RPCError> for ErrorObjectOwned {
    fn from(e: RPCError) -> Self {
        match e {
            RPCError::AccountError => to_custom_err("error getting account"),
            RPCError::BlockNotFound => to_custom_err("header not found"),
            RPCError::DebugNotEnabled => to_custom_err("debug_* RPCs have not been enabled"),
            RPCError::Error(e) => to_custom_err(format!("{:?}", e.to_string())),
            RPCError::EvmError(e) => {
                to_custom_err(format!("error calling EVM : {:?}", e.to_string()))
            }
            RPCError::GasCapTooLow(cap) => {
                to_custom_err(format!("gas required exceeds allowance {:#?}", cap))
            }
            RPCError::InsufficientFunds => to_custom_err("insufficient funds for transfer"),
            RPCError::InvalidBlockInput => {
                to_custom_err("both blockHash and fromBlock/toBlock specified")
            }
            RPCError::InvalidDataInput => {
                to_custom_err("data and input fields are mutually exclusive")
            }
            RPCError::InvalidGasPrice => {
                to_custom_err("both gasPrice and (maxFeePerGas or maxPriorityFeePerGas) specified")
            }
            RPCError::InvalidTransactionMessage => {
                to_custom_err("invalid transaction message parameters")
            }
            RPCError::InvalidTransactionType => to_custom_err("invalid transaction type specified"),
            RPCError::NonceCacheError => to_custom_err("could not cache account nonce"),
            RPCError::ReceiptNotFound(hash) => to_custom_err(format!(
                "could not find receipt for transaction hash {:#?}",
                hash
            )),
            RPCError::RevertError(msg, data) => {
                let raw_value = to_json_raw_value(&data).ok();
                ErrorObject::owned(3, msg, raw_value)
            }
            RPCError::StateRootNotFound => to_custom_err("state root not found"),
            RPCError::TraceNotEnabled => to_custom_err("debug_trace* RPCs have not been enabled"),
            RPCError::TracingParamError(hash) => to_custom_err(format!(
                "javascript based tracing is not available (hash :{:?})",
                hash
            )),
            RPCError::TxExecutionFailed => to_custom_err("transaction execution failed"),
            RPCError::TxNotFound(hash) => to_custom_err(format!(
                "could not find transaction for transaction hash {:#?}",
                hash
            )),
            RPCError::ValueOverflow => to_custom_err("value overflow"),
            RPCError::ValueUnderflow => to_custom_err("value underflow"),
            RPCError::DivideError => to_custom_err("divide error"),
        }
    }
}

pub fn to_custom_err<T: ToString>(e: T) -> ErrorObjectOwned {
    // Match jsonrpsee 0.16 Error::Custom code (-32001) and prefix expected by RPC clients/tests
    ErrorObject::owned(
        -32001,
        format!("Custom error: {}", e.to_string()),
        None::<()>,
    )
}

#[cfg(test)]
mod tests {
    use ain_evm::EVMError;
    use ethereum_types::H256;
    use jsonrpsee::types::ErrorObjectOwned;

    use super::{to_custom_err, RPCError};

    fn assert_custom_err(err: RPCError, expected_msg: &str) {
        let obj: ErrorObjectOwned = err.into();
        assert_eq!(obj.code(), -32001);
        assert_eq!(obj.message(), format!("Custom error: {expected_msg}"));
        assert!(obj.data().is_none());
    }

    #[test]
    fn to_custom_err_sets_expected_code_and_prefix() {
        let obj = to_custom_err("boom");
        assert_eq!(obj.code(), -32001);
        assert_eq!(obj.message(), "Custom error: boom");
        assert!(obj.data().is_none());
    }

    #[test]
    fn static_rpc_error_variants_map_to_expected_messages() {
        assert_custom_err(RPCError::AccountError, "error getting account");
        assert_custom_err(RPCError::BlockNotFound, "header not found");
        assert_custom_err(
            RPCError::DebugNotEnabled,
            "debug_* RPCs have not been enabled",
        );
        assert_custom_err(
            RPCError::InsufficientFunds,
            "insufficient funds for transfer",
        );
        assert_custom_err(
            RPCError::InvalidBlockInput,
            "both blockHash and fromBlock/toBlock specified",
        );
        assert_custom_err(
            RPCError::InvalidDataInput,
            "data and input fields are mutually exclusive",
        );
        assert_custom_err(
            RPCError::InvalidGasPrice,
            "both gasPrice and (maxFeePerGas or maxPriorityFeePerGas) specified",
        );
        assert_custom_err(
            RPCError::InvalidTransactionMessage,
            "invalid transaction message parameters",
        );
        assert_custom_err(
            RPCError::InvalidTransactionType,
            "invalid transaction type specified",
        );
        assert_custom_err(RPCError::NonceCacheError, "could not cache account nonce");
        assert_custom_err(RPCError::StateRootNotFound, "state root not found");
        assert_custom_err(
            RPCError::TraceNotEnabled,
            "debug_trace* RPCs have not been enabled",
        );
        assert_custom_err(RPCError::TxExecutionFailed, "transaction execution failed");
        assert_custom_err(RPCError::ValueOverflow, "value overflow");
        assert_custom_err(RPCError::ValueUnderflow, "value underflow");
        assert_custom_err(RPCError::DivideError, "divide error");
    }

    #[test]
    fn parameterized_rpc_error_variants_map_correctly() {
        let io_err = std::io::Error::new(std::io::ErrorKind::Other, "disk full");
        let obj: ErrorObjectOwned = RPCError::Error(Box::new(io_err)).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj.message().contains("disk full"));

        let evm_err = EVMError::from(anyhow::anyhow!("evm failure"));
        let obj: ErrorObjectOwned = RPCError::EvmError(evm_err).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj.message().contains("error calling EVM"));
        assert!(obj.message().contains("evm failure"));

        let obj: ErrorObjectOwned = RPCError::GasCapTooLow(50_000).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj.message().contains("gas required exceeds allowance"));
        assert!(obj.message().contains("50000"));

        let hash = H256::from_low_u64_be(0xabcd);
        let obj: ErrorObjectOwned = RPCError::ReceiptNotFound(hash).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj
            .message()
            .contains("could not find receipt for transaction hash"));

        let obj: ErrorObjectOwned = RPCError::TxNotFound(hash).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj
            .message()
            .contains("could not find transaction for transaction hash"));

        let trace_hash = [1u8; 16];
        let obj: ErrorObjectOwned = RPCError::TracingParamError(trace_hash).into();
        assert_eq!(obj.code(), -32001);
        assert!(obj
            .message()
            .contains("javascript based tracing is not available"));
    }

    #[test]
    fn revert_error_uses_code_3_and_json_data() {
        let obj: ErrorObjectOwned =
            RPCError::RevertError("execution reverted: bad".to_string(), "0x08c379a0".to_string())
                .into();
        assert_eq!(obj.code(), 3);
        assert_eq!(obj.message(), "execution reverted: bad");
        assert_eq!(obj.data().unwrap().get(), "\"0x08c379a0\"");
    }
}
