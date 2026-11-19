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

use super::blockscout::BlockscoutCallInner as CallInner;
use crate::trace::{
    listeners::call_list::Listener,
    types::{
        block::{
            TransactionTrace, TransactionTraceAction, TransactionTraceOutput,
            TransactionTraceResult,
        },
        CallResult, CreateResult, CreateType,
    },
};

use ethereum_types::{H256, U256};

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
        for (eth_tx_index, entry) in l.entries.iter().enumerate() {
            let mut tx_traces: Vec<_> = entry
                .iter()
                .map(|(_, trace)| match trace.inner.clone() {
                    CallInner::Call {
                        input,
                        to,
                        res,
                        call_type,
                    } => TransactionTrace {
                        action: TransactionTraceAction::Call {
                            call_type,
                            from: trace.from,
                            gas: trace.gas,
                            input,
                            to,
                            value: trace.value,
                        },
                        // Can't be known here, must be inserted upstream.
                        block_hash: H256::default(),
                        // Can't be known here, must be inserted upstream.
                        block_number: 0,
                        output: match res {
                            CallResult::Output(output) => {
                                TransactionTraceOutput::Result(TransactionTraceResult::Call {
                                    gas_used: if system_tx {
                                        U256::zero()
                                    } else {
                                        trace.gas_used
                                    },
                                    output,
                                })
                            }
                            CallResult::Error(error) => TransactionTraceOutput::Error(error),
                        },
                        subtraces: trace.subtraces,
                        trace_address: trace.trace_address.clone(),
                        // Can't be known here, must be inserted upstream.
                        transaction_hash: H256::default(),
                        transaction_position: eth_tx_index as u32,
                    },
                    CallInner::Create { init, res } => {
                        TransactionTrace {
                            action: TransactionTraceAction::Create {
                                creation_method: CreateType::Create,
                                from: trace.from,
                                gas: trace.gas,
                                init,
                                value: trace.value,
                            },
                            // Can't be known here, must be inserted upstream.
                            block_hash: H256::default(),
                            // Can't be known here, must be inserted upstream.
                            block_number: 0,
                            output: match res {
                                CreateResult::Success {
                                    created_contract_address_hash,
                                    created_contract_code,
                                } => {
                                    TransactionTraceOutput::Result(TransactionTraceResult::Create {
                                        gas_used: if system_tx {
                                            U256::zero()
                                        } else {
                                            trace.gas_used
                                        },
                                        code: created_contract_code,
                                        address: created_contract_address_hash,
                                    })
                                }
                                CreateResult::Error { error } => {
                                    TransactionTraceOutput::Error(error)
                                }
                            },
                            subtraces: trace.subtraces,
                            trace_address: trace.trace_address.clone(),
                            // Can't be known here, must be inserted upstream.
                            transaction_hash: H256::default(),
                            transaction_position: eth_tx_index as u32,
                        }
                    }
                    CallInner::SelfDestruct { balance, to } => TransactionTrace {
                        action: TransactionTraceAction::Suicide {
                            address: trace.from,
                            balance,
                            refund_address: to,
                        },
                        // Can't be known here, must be inserted upstream.
                        block_hash: H256::default(),
                        // Can't be known here, must be inserted upstream.
                        block_number: 0,
                        output: TransactionTraceOutput::Result(TransactionTraceResult::Suicide),
                        subtraces: trace.subtraces,
                        trace_address: trace.trace_address.clone(),
                        // Can't be known here, must be inserted upstream.
                        transaction_hash: H256::default(),
                        transaction_position: eth_tx_index as u32,
                    },
                })
                .collect();

            traces.append(&mut tx_traces);
        }
        Some(traces)
    }
}

#[cfg(test)]
mod tests {
    use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

    use ethereum_types::{H160, U256};
    use serde_json::json;

    use crate::trace::{
        formatters::{
            blockscout::{BlockscoutCall, BlockscoutCallInner},
            trace_filter::Formatter,
            ResponseFormatter,
        },
        listeners::call_list::Listener,
        types::{CallResult, CallType},
    };

    fn address(value: u64) -> H160 {
        H160::from_low_u64_be(value)
    }

    fn listener_with(entry: BTreeMap<u32, BlockscoutCall>) -> Rc<RefCell<Listener>> {
        let mut listener = Listener::default();
        listener.entries = vec![entry];
        Rc::new(RefCell::new(listener))
    }

    #[test]
    fn trace_filter_formats_a_flat_call_list() {
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
                inner: BlockscoutCallInner::Call {
                    call_type: CallType::Call,
                    to: address(0x2222),
                    input: vec![0xde, 0xad],
                    res: CallResult::Output(vec![0xbe, 0xef]),
                },
            },
        );

        let traces = Formatter::format(listener_with(entry), false).expect("expected a trace");
        let actual = serde_json::to_value(traces).unwrap();

        assert_eq!(
            actual,
            json!([
                {
                    "type": "call",
                    "action": {
                        "callType": "call",
                        "from": "0x0000000000000000000000000000000000001111",
                        "gas": "0x186a0",
                        "input": "0xdead",
                        "to": "0x0000000000000000000000000000000000002222",
                        "value": "0x5"
                    },
                    "blockHash": "0x0000000000000000000000000000000000000000000000000000000000000000",
                    "blockNumber": 0,
                    "result": {
                        "gasUsed": "0x5208",
                        "output": "0xbeef"
                    },
                    "subtraces": 0,
                    "traceAddress": [],
                    "transactionHash": "0x0000000000000000000000000000000000000000000000000000000000000000",
                    "transactionPosition": 0
                }
            ])
        );
    }

    #[test]
    fn trace_filter_returns_an_empty_list_for_an_empty_listener() {
        let listener = Rc::new(RefCell::new(Listener::default()));
        let traces = Formatter::format(listener, false).expect("expected a trace");
        assert!(traces.is_empty());
        assert_eq!(serde_json::to_value(traces).unwrap(), json!([]));
    }

    #[test]
    fn trace_filter_reports_a_revert() {
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
                inner: BlockscoutCallInner::Call {
                    call_type: CallType::Call,
                    to: address(0x2222),
                    input: vec![0xde, 0xad],
                    res: CallResult::Error(b"execution reverted".to_vec()),
                },
            },
        );

        let traces = Formatter::format(listener_with(entry), false).expect("expected a trace");
        let actual = serde_json::to_value(traces).unwrap();

        assert_eq!(actual[0]["error"], json!("execution reverted"));
        assert!(actual[0].get("result").is_none());
    }
}
