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
