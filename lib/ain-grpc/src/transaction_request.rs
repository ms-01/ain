use ain_evm::bytes::Bytes;
use ethereum::{
    AccessListItem, EIP1559TransactionMessage, EIP2930TransactionMessage, LegacyTransactionMessage,
};
use ethereum_types::{H160, U256};
use serde::{Deserialize, Serialize};

pub enum TransactionMessage {
    Legacy(LegacyTransactionMessage),
    EIP2930(EIP2930TransactionMessage),
    EIP1559(EIP1559TransactionMessage),
}

/// Transaction request coming from RPC
#[derive(Clone, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct TransactionRequest {
    /// Sender
    pub from: Option<H160>,
    /// Recipient
    pub to: Option<H160>,
    /// Gas Price, legacy.
    #[serde(default)]
    pub gas_price: Option<U256>,
    /// Max BaseFeePerGas the user is willing to pay.
    #[serde(default)]
    pub max_fee_per_gas: Option<U256>,
    /// The miner's tip.
    #[serde(default)]
    pub max_priority_fee_per_gas: Option<U256>,
    /// Gas
    pub gas: Option<U256>,
    /// Value of transaction in wei
    pub value: Option<U256>,
    /// Additional data sent with transaction
    pub data: Option<Bytes>,
    /// Input
    pub input: Option<Bytes>,
    /// Transaction's nonce
    pub nonce: Option<U256>,
    /// Pre-pay to warm storage access.
    #[serde(default)]
    pub access_list: Option<Vec<AccessListItem>>,
    /// EIP-2718 type
    #[serde(rename = "type")]
    pub transaction_type: Option<U256>,
}

impl From<TransactionRequest> for Option<TransactionMessage> {
    fn from(req: TransactionRequest) -> Self {
        match (req.gas_price, req.max_fee_per_gas, req.access_list.clone()) {
            // Legacy
            (Some(_), None, None) => Some(TransactionMessage::Legacy(LegacyTransactionMessage {
                nonce: U256::zero(),
                gas_price: req.gas_price.unwrap_or_default(),
                gas_limit: req.gas.unwrap_or_default(),
                value: req.value.unwrap_or_default(),
                input: req
                    .input
                    .or(req.data)
                    .map(Bytes::into_vec)
                    .unwrap_or_default(),
                action: match req.to {
                    Some(to) => ethereum::TransactionAction::Call(to),
                    None => ethereum::TransactionAction::Create,
                },
                chain_id: None,
            })),
            // EIP2930
            (_, None, Some(_)) => Some(TransactionMessage::EIP2930(EIP2930TransactionMessage {
                nonce: U256::zero(),
                gas_price: req.gas_price.unwrap_or_default(),
                gas_limit: req.gas.unwrap_or_default(),
                value: req.value.unwrap_or_default(),
                input: req
                    .input
                    .or(req.data)
                    .map(Bytes::into_vec)
                    .unwrap_or_default(),
                action: match req.to {
                    Some(to) => ethereum::TransactionAction::Call(to),
                    None => ethereum::TransactionAction::Create,
                },
                chain_id: 0,
                access_list: req.access_list.unwrap_or_default(),
            })),
            // EIP1559
            (None, Some(_), _) | (None, None, None) => {
                // Empty fields fall back to the canonical transaction schema.
                Some(TransactionMessage::EIP1559(EIP1559TransactionMessage {
                    nonce: U256::zero(),
                    max_fee_per_gas: req.max_fee_per_gas.unwrap_or_default(),
                    max_priority_fee_per_gas: req.max_priority_fee_per_gas.unwrap_or_default(),
                    gas_limit: req.gas.unwrap_or_default(),
                    value: req.value.unwrap_or_default(),
                    input: req
                        .input
                        .or(req.data)
                        .map(Bytes::into_vec)
                        .unwrap_or_default(),
                    action: match req.to {
                        Some(to) => ethereum::TransactionAction::Call(to),
                        None => ethereum::TransactionAction::Create,
                    },
                    chain_id: 0,
                    access_list: req.access_list.unwrap_or_default(),
                }))
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use ain_evm::bytes::Bytes;
    use ethereum::{AccessListItem, TransactionAction};
    use ethereum_types::{H160, H256, U256};
    use serde_json::json;

    use super::{TransactionMessage, TransactionRequest};

    #[test]
    fn transaction_request_deserializes_and_rejects_unknown_fields() {
        let req: TransactionRequest = serde_json::from_value(json!({
            "from": "0x0000000000000000000000000000000000000001",
            "to": "0x0000000000000000000000000000000000000002",
            "gas": "0x5208",
            "value": "0x10",
            "type": "0x2"
        }))
        .unwrap();
        assert_eq!(req.from, Some(H160::from_low_u64_be(1)));
        assert_eq!(req.to, Some(H160::from_low_u64_be(2)));
        assert_eq!(req.transaction_type, Some(U256::from(2)));

        assert!(serde_json::from_value::<TransactionRequest>(json!({ "extra": 1 })).is_err());
    }

    #[test]
    fn legacy_transaction_request_converts_call_and_create() {
        let req = TransactionRequest {
            to: Some(H160::from_low_u64_be(0xaa)),
            gas_price: Some(U256::from(100)),
            gas: Some(U256::from(21_000)),
            value: Some(U256::from(5)),
            data: Some(Bytes::from(vec![0x01, 0x02])),
            ..Default::default()
        };
        match Option::<TransactionMessage>::from(req).unwrap() {
            TransactionMessage::Legacy(msg) => {
                assert_eq!(msg.gas_price, U256::from(100));
                assert_eq!(msg.gas_limit, U256::from(21_000));
                assert_eq!(msg.value, U256::from(5));
                assert_eq!(msg.input, vec![0x01, 0x02]);
                assert_eq!(msg.action, TransactionAction::Call(H160::from_low_u64_be(0xaa)));
            }
            _ => panic!("expected Legacy transaction message"),
        }

        let create_req = TransactionRequest {
            gas_price: Some(U256::from(50)),
            input: Some(Bytes::from(vec![0x60])),
            ..Default::default()
        };
        match Option::<TransactionMessage>::from(create_req).unwrap() {
            TransactionMessage::Legacy(msg) => {
                assert_eq!(msg.action, TransactionAction::Create);
                assert_eq!(msg.input, vec![0x60]);
            }
            _ => panic!("expected Legacy transaction message"),
        }
    }

    #[test]
    fn eip2930_transaction_request_converts_call_and_create() {
        let item = AccessListItem {
            address: H160::from_low_u64_be(0xbb),
            storage_keys: vec![H256::from_low_u64_be(1)],
        };
        let req = TransactionRequest {
            to: Some(H160::from_low_u64_be(0xaa)),
            gas_price: Some(U256::from(200)),
            access_list: Some(vec![item.clone()]),
            ..Default::default()
        };
        match Option::<TransactionMessage>::from(req).unwrap() {
            TransactionMessage::EIP2930(msg) => {
                assert_eq!(msg.gas_price, U256::from(200));
                assert_eq!(msg.access_list, vec![item]);
                assert_eq!(msg.action, TransactionAction::Call(H160::from_low_u64_be(0xaa)));
            }
            _ => panic!("expected EIP2930 transaction message"),
        }

        let create_req = TransactionRequest {
            access_list: Some(vec![]),
            ..Default::default()
        };
        match Option::<TransactionMessage>::from(create_req).unwrap() {
            TransactionMessage::EIP2930(msg) => {
                assert_eq!(msg.action, TransactionAction::Create);
            }
            _ => panic!("expected EIP2930 transaction message"),
        }
    }

    #[test]
    fn eip1559_and_default_and_invalid_transaction_requests() {
        let req = TransactionRequest {
            to: Some(H160::from_low_u64_be(0xcc)),
            max_fee_per_gas: Some(U256::from(300)),
            max_priority_fee_per_gas: Some(U256::from(10)),
            ..Default::default()
        };
        match Option::<TransactionMessage>::from(req).unwrap() {
            TransactionMessage::EIP1559(msg) => {
                assert_eq!(msg.max_fee_per_gas, U256::from(300));
                assert_eq!(msg.max_priority_fee_per_gas, U256::from(10));
                assert_eq!(msg.action, TransactionAction::Call(H160::from_low_u64_be(0xcc)));
            }
            _ => panic!("expected EIP1559 transaction message"),
        }

        // Default empty request falls back to EIP1559 Create
        match Option::<TransactionMessage>::from(TransactionRequest::default()).unwrap() {
            TransactionMessage::EIP1559(msg) => {
                assert_eq!(msg.action, TransactionAction::Create);
                assert_eq!(msg.max_fee_per_gas, U256::zero());
            }
            _ => panic!("expected EIP1559 transaction message"),
        }

        // Conflicting gas_price and max_fee_per_gas returns None
        let invalid = TransactionRequest {
            gas_price: Some(U256::from(1)),
            max_fee_per_gas: Some(U256::from(2)),
            ..Default::default()
        };
        assert!(Option::<TransactionMessage>::from(invalid).is_none());
    }
}
