use std::collections::{BTreeMap, HashMap};

use ain_evm::{backend::Overlay, bytes::Bytes, trace::service::AccessListInfo};
use ethereum::{AccessList, AccessListItem, Account};
use ethereum_types::{H160, H256, U256};
use jsonrpsee::types::ErrorObjectOwned;
use serde::Deserialize;

use crate::errors::RPCError;

/// Call request
#[derive(Clone, Debug, Default, Eq, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct CallRequest {
    /// From
    pub from: Option<H160>,
    /// To
    pub to: Option<H160>,
    /// Gas Price
    pub gas_price: Option<U256>,
    /// EIP-1559 Max base fee the caller is willing to pay
    pub max_fee_per_gas: Option<U256>,
    /// EIP-1559 Priority fee the caller is paying to the block author
    pub max_priority_fee_per_gas: Option<U256>,
    /// Gas
    pub gas: Option<U256>,
    /// Value
    pub value: Option<U256>,
    /// Data
    pub data: Option<Bytes>,
    /// Input
    pub input: Option<Bytes>,
    /// Nonce
    pub nonce: Option<U256>,
    /// AccessList
    pub access_list: Option<Vec<AccessListItem>>,
    /// EIP-2718 type
    #[serde(rename = "type")]
    pub transaction_type: Option<U256>,
}

enum TxType {
    Legacy,
    EIP2930,
    EIP1559,
}

impl TryFrom<U256> for TxType {
    type Error = ErrorObjectOwned;

    fn try_from(v: U256) -> Result<Self, Self::Error> {
        match v {
            v if v == U256::zero() => Ok(Self::Legacy),
            v if v == U256::one() => Ok(Self::EIP2930),
            v if v == U256::from(2) => Ok(Self::EIP1559),
            _ => Err(RPCError::InvalidTransactionType.into()),
        }
    }
}

fn guess_tx_type(req: &CallRequest) -> Result<TxType, ErrorObjectOwned> {
    if let Some(tx_type) = req.transaction_type {
        return TxType::try_from(tx_type);
    }

    // Validate call request gas fees
    if req.gas_price.is_some()
        && (req.max_fee_per_gas.is_some() || req.max_priority_fee_per_gas.is_some())
    {
        return Err(RPCError::InvalidGasPrice.into());
    }
    if req.max_fee_per_gas.is_some() && req.max_priority_fee_per_gas.is_none() {
        return Err(RPCError::InvalidGasPrice.into());
    }

    if req.max_fee_per_gas.is_some() && req.max_priority_fee_per_gas.is_some() {
        Ok(TxType::EIP1559)
    } else if req.access_list.is_some() {
        Ok(TxType::EIP2930)
    } else {
        Ok(TxType::Legacy)
    }
}

impl CallRequest {
    pub fn get_effective_gas_price(&self) -> Result<Option<U256>, ErrorObjectOwned> {
        match guess_tx_type(self)? {
            TxType::Legacy | TxType::EIP2930 => {
                if let Some(gas_price) = self.gas_price {
                    if gas_price.is_zero() {
                        return Ok(None);
                    }
                }
                Ok(self.gas_price)
            }
            TxType::EIP1559 => {
                if let Some(max_fee_per_gas) = self.max_fee_per_gas {
                    if max_fee_per_gas.is_zero() {
                        return Ok(None);
                    }
                }
                Ok(self.max_fee_per_gas)
            }
        }
    }

    // https://github.com/ethereum/go-ethereum/blob/281e8cd5abaac86ed3f37f98250ff147b3c9fe62/internal/ethapi/transaction_args.go#L67
    // We accept "data" and "input" for backwards-compatibility reasons.
    //  "input" is the newer name and should be preferred by clients.
    // 	Issue detail: https://github.com/ethereum/go-ethereum/issues/15628
    pub fn get_data(&self) -> Result<Bytes, ErrorObjectOwned> {
        if self.data.is_some() && self.input.is_some() {
            return Err(RPCError::InvalidDataInput.into());
        }

        if let Some(data) = self.data.clone() {
            Ok(data)
        } else if let Some(data) = self.input.clone() {
            Ok(data)
        } else {
            Ok(Default::default())
        }
    }
}

// State override
#[derive(Clone, Debug, Default, Eq, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct CallStateOverride {
    /// Fake balance to set for the account before executing the call.
    pub balance: Option<U256>,
    /// Fake nonce to set for the account before executing the call.
    pub nonce: Option<U256>,
    /// Fake EVM bytecode to inject into the account before executing the call.
    pub code: Option<Bytes>,
    /// Fake key-value mapping to override all slots in the account storage before
    /// executing the call.
    pub state: Option<BTreeMap<H256, H256>>,
    /// Fake key-value mapping to override individual slots in the account storage before
    /// executing the call.
    pub state_diff: Option<BTreeMap<H256, H256>>,
}

pub fn override_to_overlay(r#override: BTreeMap<H160, CallStateOverride>) -> Overlay {
    let mut overlay = Overlay::default();

    for (address, state_override) in r#override {
        let code = state_override.code.map(|b| b.into_vec());
        let mut storage = state_override
            .state
            .unwrap_or_default()
            .into_iter()
            .collect::<HashMap<_, _>>();

        let account = Account {
            balance: state_override.balance.unwrap_or_default(),
            nonce: state_override.nonce.unwrap_or_default(),
            storage_root: H256::zero(),
            code_hash: H256::zero(),
        };

        let reset_storage = storage.is_empty();
        if let Some(diff) = state_override.state_diff {
            for (k, v) in diff {
                storage.insert(k, v);
            }
        }

        overlay.apply(address, account, code, storage, reset_storage);
    }

    overlay
}

#[derive(Clone, Debug, Default, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AccessListResult {
    pub access_list: AccessList,
    pub gas_used: U256,
}

impl From<AccessListInfo> for AccessListResult {
    fn from(value: AccessListInfo) -> Self {
        Self {
            access_list: value.access_list,
            gas_used: value.gas_used,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use ain_evm::{bytes::Bytes, trace::service::AccessListInfo};
    use ethereum::AccessListItem;
    use ethereum_types::{H160, H256, U256};
    use serde_json::json;

    use super::{override_to_overlay, AccessListResult, CallRequest, CallStateOverride};

    #[test]
    fn call_request_deserializes_camel_case_and_rejects_unknown_fields() {
        let req: CallRequest = serde_json::from_value(json!({
            "from": "0x0000000000000000000000000000000000000001",
            "to": "0x0000000000000000000000000000000000000002",
            "gasPrice": "0x10",
            "gas": "0x5208",
            "value": "0x1",
            "data": "0xdeadbeef",
            "type": "0x0"
        }))
        .unwrap();
        assert_eq!(req.from, Some(H160::from_low_u64_be(1)));
        assert_eq!(req.to, Some(H160::from_low_u64_be(2)));
        assert_eq!(req.gas_price, Some(U256::from(0x10)));
        assert_eq!(req.transaction_type, Some(U256::zero()));

        let err = serde_json::from_value::<CallRequest>(json!({ "unknownField": "0x1" }));
        assert!(err.is_err());
    }

    #[test]
    fn effective_gas_price_handles_legacy_eip2930_eip1559_and_errors() {
        // Default legacy with no gas price
        let req = CallRequest::default();
        assert_eq!(req.get_effective_gas_price().unwrap(), None);

        // Legacy with zero gas price normalizes to None
        let req = CallRequest {
            gas_price: Some(U256::zero()),
            ..Default::default()
        };
        assert_eq!(req.get_effective_gas_price().unwrap(), None);

        // Legacy with non-zero gas price
        let req = CallRequest {
            gas_price: Some(U256::from(42)),
            ..Default::default()
        };
        assert_eq!(req.get_effective_gas_price().unwrap(), Some(U256::from(42)));

        // EIP-2930 inferred from access_list
        let req = CallRequest {
            gas_price: Some(U256::from(100)),
            access_list: Some(vec![]),
            ..Default::default()
        };
        assert_eq!(
            req.get_effective_gas_price().unwrap(),
            Some(U256::from(100))
        );

        // EIP-1559 inferred from max_fee + max_priority_fee
        let req = CallRequest {
            max_fee_per_gas: Some(U256::from(200)),
            max_priority_fee_per_gas: Some(U256::from(10)),
            ..Default::default()
        };
        assert_eq!(
            req.get_effective_gas_price().unwrap(),
            Some(U256::from(200))
        );

        // EIP-1559 with zero max_fee normalizes to None
        let req = CallRequest {
            max_fee_per_gas: Some(U256::zero()),
            max_priority_fee_per_gas: Some(U256::zero()),
            ..Default::default()
        };
        assert_eq!(req.get_effective_gas_price().unwrap(), None);

        // Explicit transaction types 0, 1, 2
        for tx_type in [0u64, 1, 2] {
            let req = CallRequest {
                transaction_type: Some(U256::from(tx_type)),
                gas_price: Some(U256::from(5)),
                max_fee_per_gas: Some(U256::from(9)),
                ..Default::default()
            };
            let expected = if tx_type == 2 { 9 } else { 5 };
            assert_eq!(
                req.get_effective_gas_price().unwrap(),
                Some(U256::from(expected))
            );
        }

        // Invalid explicit transaction type
        let req = CallRequest {
            transaction_type: Some(U256::from(99)),
            ..Default::default()
        };
        assert!(req
            .get_effective_gas_price()
            .unwrap_err()
            .message()
            .contains("invalid transaction type"));

        // Conflicting gas_price and max_fee_per_gas
        let req = CallRequest {
            gas_price: Some(U256::from(1)),
            max_fee_per_gas: Some(U256::from(2)),
            ..Default::default()
        };
        assert!(req
            .get_effective_gas_price()
            .unwrap_err()
            .message()
            .contains("both gasPrice and"));

        // Conflicting gas_price and max_priority_fee_per_gas
        let req = CallRequest {
            gas_price: Some(U256::from(1)),
            max_priority_fee_per_gas: Some(U256::from(2)),
            ..Default::default()
        };
        assert!(req.get_effective_gas_price().is_err());

        // max_fee_per_gas without max_priority_fee_per_gas
        let req = CallRequest {
            max_fee_per_gas: Some(U256::from(2)),
            ..Default::default()
        };
        assert!(req.get_effective_gas_price().is_err());
    }

    #[test]
    fn get_data_handles_data_input_default_and_conflict() {
        let empty = CallRequest::default();
        assert_eq!(empty.get_data().unwrap(), Bytes::default());

        let with_data = CallRequest {
            data: Some(Bytes::from(vec![0xaa, 0xbb])),
            ..Default::default()
        };
        assert_eq!(with_data.get_data().unwrap(), Bytes::from(vec![0xaa, 0xbb]));

        let with_input = CallRequest {
            input: Some(Bytes::from(vec![0xcc])),
            ..Default::default()
        };
        assert_eq!(with_input.get_data().unwrap(), Bytes::from(vec![0xcc]));

        let conflict = CallRequest {
            data: Some(Bytes::from(vec![0xaa])),
            input: Some(Bytes::from(vec![0xbb])),
            ..Default::default()
        };
        assert!(conflict
            .get_data()
            .unwrap_err()
            .message()
            .contains("mutually exclusive"));
    }

    #[test]
    fn override_to_overlay_applies_accounts_code_state_and_diff() {
        let addr1 = H160::from_low_u64_be(0x11);
        let addr2 = H160::from_low_u64_be(0x22);
        let slot1 = H256::from_low_u64_be(1);
        let slot2 = H256::from_low_u64_be(2);

        let mut state = BTreeMap::new();
        state.insert(slot1, H256::from_low_u64_be(10));

        let mut diff = BTreeMap::new();
        diff.insert(slot1, H256::from_low_u64_be(20));
        diff.insert(slot2, H256::from_low_u64_be(30));

        let mut overrides = BTreeMap::new();
        overrides.insert(
            addr1,
            CallStateOverride {
                balance: Some(U256::from(1000)),
                nonce: Some(U256::from(5)),
                code: Some(Bytes::from(vec![0x60, 0x00])),
                state: Some(state),
                state_diff: Some(diff),
            },
        );
        overrides.insert(addr2, CallStateOverride::default());

        let _overlay = override_to_overlay(overrides);
    }

    #[test]
    fn access_list_result_converts_from_info() {
        let item = AccessListItem {
            address: H160::from_low_u64_be(0x42),
            storage_keys: vec![H256::from_low_u64_be(7)],
        };
        let info = AccessListInfo {
            access_list: vec![item.clone()],
            gas_used: U256::from(25_000),
        };
        let result = AccessListResult::from(info);
        assert_eq!(result.access_list, vec![item]);
        assert_eq!(result.gas_used, U256::from(25_000));
    }
}
