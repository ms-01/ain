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

use crate::trace::{listeners::raw::Listener, types::single::TransactionTrace};
use ethereum_types::U256;

pub struct Formatter;

impl super::ResponseFormatter for Formatter {
    type Listener = Listener;
    type Response = TransactionTrace;

    fn format(listener: Rc<RefCell<Listener>>, system_tx: bool) -> Option<TransactionTrace> {
        let l = listener.borrow();
        if l.remaining_memory_usage.is_none() {
            None
        } else {
            Some(TransactionTrace::Raw {
                struct_logs: l.struct_logs.clone(),
                failed: l.exec_flag,
                gas: if system_tx {
                    U256::zero()
                } else {
                    l.final_gas.into()
                },
                return_value: l.return_value.clone(),
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use std::{cell::RefCell, rc::Rc};

    use ethereum_types::U256;
    use serde_json::json;

    use crate::trace::{
        formatters::{raw::Formatter, ResponseFormatter},
        listeners::raw::Listener,
        types::single::RawStepLog,
    };

    fn listener() -> Rc<RefCell<Listener>> {
        let mut listener = Listener::new(false, false, false, 1024);
        listener.struct_logs = vec![RawStepLog {
            depth: U256::from(1),
            gas: U256::from(100),
            gas_cost: U256::from(3),
            memory: None,
            op: b"PUSH1".to_vec(),
            pc: U256::zero(),
            stack: None,
            storage: None,
        }];
        listener.return_value = vec![0x0a, 0x0b];
        listener.finish_transaction(false, 50_000);
        Rc::new(RefCell::new(listener))
    }

    fn format(listener: Rc<RefCell<Listener>>, system_tx: bool) -> serde_json::Value {
        let trace = Formatter::format(listener, system_tx).expect("expected a trace");
        serde_json::to_value(trace).unwrap()
    }

    #[test]
    fn raw_formatter_returns_none_without_memory_usage() {
        let mut listener = Listener::new(false, false, false, 1024);
        listener.remaining_memory_usage = None;
        let listener = Rc::new(RefCell::new(listener));
        assert!(Formatter::format(listener, false).is_none());
    }

    #[test]
    fn raw_formatter_serializes_the_struct_logs() {
        let actual = format(listener(), false);

        assert_eq!(
            actual,
            json!({
                "gas": "0xc350",
                "failed": false,
                "returnValue": "0x0a0b",
                "structLogs": [
                    {
                        "depth": 1,
                        "gas": 100,
                        "gasCost": 3,
                        "op": "PUSH1",
                        "pc": 0
                    }
                ]
            })
        );
    }

    #[test]
    fn raw_formatter_zeroes_gas_for_system_transactions() {
        let actual = format(listener(), true);

        assert_eq!(actual["gas"], json!("0x0"));
    }
    #[test]
    fn raw_formatter_reports_a_failed_execution() {
        let mut listener = Listener::new(false, false, false, 1024);
        listener.return_value = vec![];
        listener.finish_transaction(true, 50_000);
        let listener = Rc::new(RefCell::new(listener));

        let actual = format(listener, false);

        assert_eq!(actual["failed"], json!(true));
        assert_eq!(actual["returnValue"], json!("0x"));
        assert_eq!(actual["structLogs"], json!([]));
    }
}
