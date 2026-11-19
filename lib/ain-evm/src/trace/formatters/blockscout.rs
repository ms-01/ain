// Copyright 2019-2022 PureStake Inc.
// This file is part of Moonbeam.

// Moonbeam is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Moonbeam is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with Moonbeam.  If not, see <http://www.gnu.org/licenses/>.

use std::{cell::RefCell, rc::Rc};

use crate::trace::{
    listeners::call_list::Listener,
    types::{
        serialization::*,
        single::{Call, TransactionTrace},
        CallResult, CallType, CreateResult,
    },
};

use ethereum_types::{H160, U256};
use serde::{Deserialize, Serialize};

pub struct Formatter;

impl super::ResponseFormatter for Formatter {
    type Listener = Listener;
    type Response = TransactionTrace;

    fn format(listener: Rc<RefCell<Listener>>, system_tx: bool) -> Option<TransactionTrace> {
        if let Some(entry) = listener.borrow().entries.last() {
            return Some(TransactionTrace::CallList(
                entry
                    .iter()
                    .map(|(_, it)| {
                        let mut value = it.clone();
                        if system_tx {
                            value.gas_used = U256::zero();
                        }
                        Call::Blockscout(Box::new(value))
                    })
                    .collect(),
            ));
        }
        None
    }
}

#[derive(Clone, Eq, PartialEq, Debug, Serialize, Deserialize)]
#[serde(rename_all = "lowercase", tag = "type")]
pub enum BlockscoutCallInner {
    Call {
        #[serde(rename(serialize = "callType"))]
        /// Type of call.
        call_type: CallType,
        to: H160,
        #[serde(serialize_with = "bytes_0x_serialize")]
        input: Vec<u8>,
        /// "output" or "error" field
        #[serde(flatten)]
        res: CallResult,
    },
    Create {
        #[serde(serialize_with = "bytes_0x_serialize")]
        init: Vec<u8>,
        #[serde(flatten)]
        res: CreateResult,
    },
    SelfDestruct {
        #[serde(skip)]
        balance: U256,
        to: H160,
    },
}

#[derive(Clone, Eq, PartialEq, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BlockscoutCall {
    pub from: H160,
    /// Indices of parent calls.
    pub trace_address: Vec<u32>,
    /// Number of children calls.
    /// Not needed for Blockscout, but needed for `crate::block`
    /// types that are build from this type.
    #[serde(skip)]
    pub subtraces: u32,
    /// Sends funds to the (payable) function
    pub value: U256,
    /// Remaining gas in the runtime.
    pub gas: U256,
    /// Gas used by this context.
    pub gas_used: U256,
    #[serde(flatten)]
    pub inner: BlockscoutCallInner,
}

#[cfg(test)]
mod tests {
    use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

    use ethereum_types::{H160, U256};
    use serde_json::json;

    use crate::trace::{
        formatters::{
            blockscout::{BlockscoutCall, BlockscoutCallInner, Formatter},
            ResponseFormatter,
        },
        listeners::call_list::Listener,
        types::{CallResult, CallType},
    };

    fn address(value: u64) -> H160 {
        H160::from_low_u64_be(value)
    }

    fn call(res: CallResult) -> BlockscoutCall {
        BlockscoutCall {
            from: address(0x1111),
            trace_address: vec![],
            subtraces: 0,
            value: U256::from(5),
            gas: U256::from(100_000),
            gas_used: U256::from(21_000),
            inner: BlockscoutCallInner::Call {
                call_type: CallType::Call,
                to: address(0x2222),
                input: vec![0xde, 0xad],
                res,
            },
        }
    }

    fn listener_with(entry: BTreeMap<u32, BlockscoutCall>) -> Rc<RefCell<Listener>> {
        let mut listener = Listener::default();
        listener.entries = vec![entry];
        Rc::new(RefCell::new(listener))
    }

    fn format(listener: Rc<RefCell<Listener>>, system_tx: bool) -> serde_json::Value {
        let trace = Formatter::format(listener, system_tx).expect("expected a trace");
        serde_json::to_value(trace).unwrap()
    }

    #[test]
    fn blockscout_formats_a_call() {
        let mut entry = BTreeMap::new();
        entry.insert(0, call(CallResult::Output(vec![0xbe, 0xef])));

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "traceAddress": [],
                    "value": "0x5",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "call",
                    "callType": "call",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "output": "0xbeef"
                }
            ])
        );
    }

    #[test]
    fn blockscout_formats_a_revert() {
        let mut entry = BTreeMap::new();
        entry.insert(0, call(CallResult::Error(b"execution reverted".to_vec())));

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "traceAddress": [],
                    "value": "0x5",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "call",
                    "callType": "call",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "error": "execution reverted"
                }
            ])
        );
    }

    #[test]
    fn blockscout_zeroes_gas_used_for_system_transactions() {
        let mut entry = BTreeMap::new();
        entry.insert(0, call(CallResult::Output(vec![0xbe, 0xef])));

        let actual = format(listener_with(entry), true);

        assert_eq!(actual[0]["gasUsed"], json!("0x0"));
        assert_eq!(actual[0]["gas"], json!("0x186a0"));
    }

    #[test]
    fn blockscout_returns_none_without_entries() {
        let listener = Rc::new(RefCell::new(Listener::default()));
        assert!(Formatter::format(listener, false).is_none());
    }

    #[test]
    fn blockscout_and_call_tracer_agree_on_the_same_call() {
        let mut entry = BTreeMap::new();
        entry.insert(0, call(CallResult::Output(vec![0xbe, 0xef])));

        let blockscout = format(listener_with(entry.clone()), false);
        let call_tracer = serde_json::to_value(
            crate::trace::formatters::call_tracer::Formatter::format(listener_with(entry), false)
                .expect("expected a trace"),
        )
        .unwrap();

        assert_eq!(blockscout[0]["from"], call_tracer[0]["from"]);
        assert_eq!(blockscout[0]["to"], call_tracer[0]["to"]);
        assert_eq!(blockscout[0]["value"], call_tracer[0]["value"]);
        assert_eq!(blockscout[0]["gas"], call_tracer[0]["gas"]);
        assert_eq!(blockscout[0]["gasUsed"], call_tracer[0]["gasUsed"]);
        assert_eq!(blockscout[0]["input"], call_tracer[0]["input"]);
        assert_eq!(blockscout[0]["output"], call_tracer[0]["output"]);
        assert_eq!(blockscout[0]["type"], json!("call"));
        assert_eq!(call_tracer[0]["type"], json!("CALL"));
    }
}
