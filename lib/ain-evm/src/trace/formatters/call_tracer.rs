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

use std::{cell::RefCell, cmp::Ordering, rc::Rc};

use super::blockscout::BlockscoutCallInner;
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
    type Response = Vec<TransactionTrace>;

    fn format(listener: Rc<RefCell<Listener>>, system_tx: bool) -> Option<Vec<TransactionTrace>> {
        // Remove empty BTreeMaps pushed to `entries`.
        // I.e. InvalidNonce or other pallet_evm::runner exits
        let mut l = listener.borrow_mut();
        l.entries.retain(|x| !x.is_empty());
        let mut traces = Vec::new();
        for entry in l.entries.iter() {
            let mut result: Vec<Call> = entry
                .iter()
                .map(|(_, it)| {
                    let from = it.from;
                    let trace_address = it.trace_address.clone();
                    let value = it.value;
                    let gas = it.gas;
                    let gas_used = if system_tx { U256::zero() } else { it.gas_used };
                    let inner = it.inner.clone();
                    Call::CallTracer(CallTracerCall {
                        from,
                        gas,
                        gas_used,
                        trace_address: Some(trace_address.clone()),
                        inner: match inner.clone() {
                            BlockscoutCallInner::Call {
                                input,
                                to,
                                res,
                                call_type,
                            } => CallTracerInner::Call {
                                call_type: match call_type {
                                    CallType::Call => "CALL".as_bytes().to_vec(),
                                    CallType::CallCode => "CALLCODE".as_bytes().to_vec(),
                                    CallType::DelegateCall => "DELEGATECALL".as_bytes().to_vec(),
                                    CallType::StaticCall => "STATICCALL".as_bytes().to_vec(),
                                },
                                to,
                                input,
                                res,
                                value: Some(value),
                            },
                            BlockscoutCallInner::Create { init, res } => CallTracerInner::Create {
                                input: init,
                                error: match res {
                                    CreateResult::Success { .. } => None,
                                    CreateResult::Error { ref error } => Some(error.clone()),
                                },
                                to: match res {
                                    CreateResult::Success {
                                        created_contract_address_hash,
                                        ..
                                    } => Some(created_contract_address_hash),
                                    CreateResult::Error { .. } => None,
                                },
                                output: match res {
                                    CreateResult::Success {
                                        created_contract_code,
                                        ..
                                    } => Some(created_contract_code),
                                    CreateResult::Error { .. } => None,
                                },
                                value,
                                call_type: "CREATE".as_bytes().to_vec(),
                            },
                            BlockscoutCallInner::SelfDestruct { balance, to } => {
                                CallTracerInner::SelfDestruct {
                                    value: balance,
                                    to,
                                    call_type: "SELFDESTRUCT".as_bytes().to_vec(),
                                }
                            }
                        },
                        calls: Vec::new(),
                    })
                })
                .collect();
            // Geth's `callTracer` expects a tree of nested calls and we have a stack.
            //
            // We iterate over the sorted stack, and push each children to it's
            // parent (the item which's `trace_address` matches &T[0..T.len()-1]) until there
            // is a single item on the list.
            //
            // The last remaining item is the context call with all it's descendants. I.e.
            //
            // 		# Input
            // 		[]
            // 		[0]
            // 		[0,0]
            // 		[0,0,0]
            // 		[0,1]
            // 		[0,1,0]
            // 		[0,1,1]
            // 		[0,1,2]
            // 		[1]
            // 		[1,0]
            //
            // 		# Sorted
            // 		[0,0,0] -> pop 0 and push to [0,0]
            // 		[0,1,0] -> pop 0 and push to [0,1]
            // 		[0,1,1] -> pop 1 and push to [0,1]
            // 		[0,1,2] -> pop 2 and push to [0,1]
            // 		[0,0] -> pop 0 and push to [0]
            // 		[0,1] -> pop 1 and push to [0]
            // 		[1,0] -> pop 0 and push to [1]
            // 		[0] -> pop 0 and push to root
            // 		[1] -> pop 1 and push to root
            // 		[]
            //
            // 		# Result
            // 		root {
            // 			calls: {
            // 				0 { 0 { 0 }, 1 { 0, 1, 2 }},
            // 				1 { 0 },
            // 			}
            // 		}
            if result.len() > 1 {
                // Sort the stack. Assume there is no `Ordering::Equal`, as we are
                // sorting by index.
                //
                // We consider an item to be `Ordering::Less` when:
                // 	- Is closer to the root or
                //	- Is greater than its sibling.
                result.sort_by(|a, b| match (a, b) {
                    (
                        Call::CallTracer(CallTracerCall {
                            trace_address: Some(a),
                            ..
                        }),
                        Call::CallTracer(CallTracerCall {
                            trace_address: Some(b),
                            ..
                        }),
                    ) => {
                        let a_len = a.len();
                        let b_len = b.len();
                        let sibling_greater_than = |a: &Vec<u32>, b: &Vec<u32>| -> bool {
                            for (i, a_value) in a.iter().enumerate() {
                                match a_value.cmp(&b[i]) {
                                    Ordering::Greater => return true,
                                    Ordering::Less => return false,
                                    Ordering::Equal => continue,
                                }
                            }
                            false
                        };
                        if b_len > a_len || (a_len == b_len && sibling_greater_than(a, b)) {
                            Ordering::Less
                        } else {
                            Ordering::Greater
                        }
                    }
                    _ => unreachable!(),
                });
                // Stack pop-and-push.
                while result.len() > 1 {
                    let mut last = result
                        .pop()
                        .expect("result.len() > 1, so pop() necessarily returns an element");
                    // Find the parent index.
                    if let Some(index) =
                        result
                            .iter()
                            .position(|current| match (last.clone(), current) {
                                (
                                    Call::CallTracer(CallTracerCall {
                                        trace_address: Some(a),
                                        ..
                                    }),
                                    Call::CallTracer(CallTracerCall {
                                        trace_address: Some(b),
                                        ..
                                    }),
                                ) => {
                                    &b[..]
                                        == a.get(0..a.len() - 1).expect(
                                            "non-root element while traversing trace result",
                                        )
                                }
                                _ => unreachable!(),
                            })
                    {
                        // Remove `trace_address` from result.
                        if let Call::CallTracer(CallTracerCall {
                            ref mut trace_address,
                            ..
                        }) = last
                        {
                            *trace_address = None;
                        }
                        // Push the children to parent.
                        if let Some(Call::CallTracer(CallTracerCall { calls, .. })) =
                            result.get_mut(index)
                        {
                            calls.push(last);
                        }
                    }
                }
            }
            // Remove `trace_address` from result.
            if let Some(Call::CallTracer(CallTracerCall { trace_address, .. })) = result.get_mut(0)
            {
                *trace_address = None;
            }
            if result.len() == 1 {
                traces.push(TransactionTrace::CallListNested(result.pop().expect(
                    "result.len() == 1, so pop() necessarily returns this element",
                )));
            }
        }
        if traces.is_empty() {
            return None;
        }
        Some(traces)
    }
}

#[derive(Clone, Eq, PartialEq, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CallTracerCall {
    pub from: H160,
    /// Indices of parent calls. Used to build the Etherscan nested response.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub trace_address: Option<Vec<u32>>,
    /// Remaining gas in the runtime.
    pub gas: U256,
    /// Gas used by this context.
    pub gas_used: U256,
    #[serde(flatten)]
    pub inner: CallTracerInner,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub calls: Vec<Call>,
}

#[derive(Clone, Eq, PartialEq, Debug, Serialize, Deserialize)]
#[serde(untagged)]
pub enum CallTracerInner {
    Call {
        #[serde(rename = "type", serialize_with = "opcode_serialize")]
        call_type: Vec<u8>,
        to: H160,
        #[serde(serialize_with = "bytes_0x_serialize")]
        input: Vec<u8>,
        /// "output" or "error" field
        #[serde(flatten)]
        res: CallResult,

        #[serde(skip_serializing_if = "Option::is_none")]
        value: Option<U256>,
    },
    Create {
        #[serde(rename = "type", serialize_with = "opcode_serialize")]
        call_type: Vec<u8>,
        #[serde(serialize_with = "bytes_0x_serialize")]
        input: Vec<u8>,
        #[serde(skip_serializing_if = "Option::is_none")]
        to: Option<H160>,
        #[serde(
            skip_serializing_if = "Option::is_none",
            serialize_with = "option_bytes_0x_serialize"
        )]
        output: Option<Vec<u8>>,
        #[serde(
            skip_serializing_if = "Option::is_none",
            serialize_with = "option_string_serialize"
        )]
        error: Option<Vec<u8>>,
        value: U256,
    },
    SelfDestruct {
        #[serde(rename = "type", serialize_with = "opcode_serialize")]
        call_type: Vec<u8>,
        to: H160,
        value: U256,
    },
}

#[cfg(test)]
mod tests {
    use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

    use ethereum_types::{H160, U256};
    use serde_json::json;

    use crate::trace::{
        formatters::{
            blockscout::{BlockscoutCall, BlockscoutCallInner},
            call_tracer::Formatter,
            ResponseFormatter,
        },
        listeners::call_list::Listener,
        types::{CallResult, CallType, CreateResult},
    };

    fn address(value: u64) -> H160 {
        H160::from_low_u64_be(value)
    }

    fn call(
        trace_address: Vec<u32>,
        to: H160,
        gas: u64,
        gas_used: u64,
        value: u64,
        input: Vec<u8>,
        res: CallResult,
    ) -> BlockscoutCall {
        BlockscoutCall {
            from: address(0x1111),
            trace_address,
            subtraces: 0,
            value: U256::from(value),
            gas: U256::from(gas),
            gas_used: U256::from(gas_used),
            inner: BlockscoutCallInner::Call {
                call_type: CallType::Call,
                to,
                input,
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
        let traces = Formatter::format(listener, system_tx).expect("expected a trace");
        serde_json::to_value(traces).unwrap()
    }

    #[test]
    fn call_tracer_formats_a_simple_call() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            call(
                vec![],
                address(0x2222),
                100_000,
                21_000,
                5,
                vec![0xde, 0xad],
                CallResult::Output(vec![0xbe, 0xef]),
            ),
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "CALL",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "output": "0xbeef",
                    "value": "0x5"
                }
            ])
        );
    }

    #[test]
    fn call_tracer_nests_child_calls() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            call(
                vec![],
                address(0x2222),
                100_000,
                21_000,
                5,
                vec![0xde, 0xad],
                CallResult::Output(vec![0xbe, 0xef]),
            ),
        );
        entry.insert(
            1,
            call(
                vec![0],
                address(0x3333),
                50_000,
                10_000,
                7,
                vec![0x01],
                CallResult::Output(vec![0x02]),
            ),
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "CALL",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "output": "0xbeef",
                    "value": "0x5",
                    "calls": [
                        {
                            "from": "0x0000000000000000000000000000000000001111",
                            "gas": "0xc350",
                            "gasUsed": "0x2710",
                            "type": "CALL",
                            "to": "0x0000000000000000000000000000000000003333",
                            "input": "0x01",
                            "output": "0x02",
                            "value": "0x7"
                        }
                    ]
                }
            ])
        );
    }

    #[test]
    fn call_tracer_reports_a_revert() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            call(
                vec![],
                address(0x2222),
                100_000,
                21_000,
                5,
                vec![0xde, 0xad],
                CallResult::Error(b"execution reverted".to_vec()),
            ),
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "CALL",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "error": "execution reverted",
                    "value": "0x5"
                }
            ])
        );
    }

    #[test]
    fn call_tracer_reports_an_out_of_gas_error() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            call(
                vec![],
                address(0x2222),
                100_000,
                100_000,
                5,
                vec![0xde, 0xad],
                CallResult::Error(b"out of gas".to_vec()),
            ),
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x186a0",
                    "gasUsed": "0x186a0",
                    "type": "CALL",
                    "to": "0x0000000000000000000000000000000000002222",
                    "input": "0xdead",
                    "error": "out of gas",
                    "value": "0x5"
                }
            ])
        );
    }

    #[test]
    fn call_tracer_zeroes_gas_used_for_system_transactions() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            call(
                vec![],
                address(0x2222),
                100_000,
                21_000,
                5,
                vec![0xde, 0xad],
                CallResult::Output(vec![0xbe, 0xef]),
            ),
        );

        let actual = format(listener_with(entry), true);

        assert_eq!(actual[0]["gasUsed"], json!("0x0"));
        assert_eq!(actual[0]["gas"], json!("0x186a0"));
    }

    #[test]
    fn call_tracer_returns_none_without_entries() {
        let listener = Rc::new(RefCell::new(Listener::default()));
        assert!(Formatter::format(listener, false).is_none());
    }

    #[test]
    fn call_tracer_formats_a_create() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            BlockscoutCall {
                from: address(0x1111),
                trace_address: vec![],
                subtraces: 0,
                value: U256::from(5),
                gas: U256::from(100_000),
                gas_used: U256::from(21_000),
                inner: BlockscoutCallInner::Create {
                    init: vec![0x60, 0x60],
                    res: CreateResult::Success {
                        created_contract_address_hash: address(0x3333),
                        created_contract_code: vec![0x00],
                    },
                },
            },
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x186a0",
                    "gasUsed": "0x5208",
                    "type": "CREATE",
                    "input": "0x6060",
                    "to": "0x0000000000000000000000000000000000003333",
                    "output": "0x00",
                    "value": "0x5"
                }
            ])
        );
    }

    #[test]
    fn call_tracer_formats_a_selfdestruct() {
        let mut entry = BTreeMap::new();
        entry.insert(
            0,
            BlockscoutCall {
                from: address(0x1111),
                trace_address: vec![],
                subtraces: 0,
                value: U256::zero(),
                gas: U256::zero(),
                gas_used: U256::zero(),
                inner: BlockscoutCallInner::SelfDestruct {
                    balance: U256::from(9),
                    to: address(0x4444),
                },
            },
        );

        let actual = format(listener_with(entry), false);

        assert_eq!(
            actual,
            json!([
                {
                    "from": "0x0000000000000000000000000000000000001111",
                    "gas": "0x0",
                    "gasUsed": "0x0",
                    "type": "SELFDESTRUCT",
                    "to": "0x0000000000000000000000000000000000004444",
                    "value": "0x9"
                }
            ])
        );
    }
}
