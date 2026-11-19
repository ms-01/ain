use std::collections::{HashMap, HashSet};

use crate::trace::tracing::{Event, Listener as ListenerT, RuntimeEvent, StepEventFilter};
use ethereum::{AccessList, AccessListItem};
use ethereum_types::{H160, H256};

#[derive(Default)]
pub struct Listener {
    access_list: HashMap<H160, HashSet<H256>>,
}

impl Listener {
    pub fn runtime_event(&mut self, event: RuntimeEvent) {
        match event {
            RuntimeEvent::SLoad { address, index, .. } => {
                self.access_list.entry(address).or_default().insert(index);
            }
            RuntimeEvent::SStore { address, index, .. } => {
                self.access_list.entry(address).or_default().insert(index);
            }
            _ => {}
        }
    }

    pub fn finish_transaction(&self) -> AccessList {
        self.access_list
            .clone()
            .into_iter()
            .map(|(address, storage_keys)| AccessListItem {
                address,
                storage_keys: Vec::from_iter(storage_keys),
            })
            .collect()
    }
}

impl ListenerT for Listener {
    fn event(&mut self, event: Event) {
        if let Event::Runtime(runtime_event) = event {
            self.runtime_event(runtime_event);
        }
    }

    fn step_event_filter(&self) -> StepEventFilter {
        StepEventFilter {
            enable_memory: false,
            enable_stack: false,
        }
    }
}

#[cfg(test)]
mod tests {
    use ethereum_types::{H160, H256};

    use crate::trace::tracing::RuntimeEvent;

    use super::Listener;

    fn address(value: u64) -> H160 {
        H160::from_low_u64_be(value)
    }

    fn sload(address: H160, index: H256) -> RuntimeEvent {
        RuntimeEvent::SLoad {
            address,
            index,
            value: H256::zero(),
        }
    }

    fn sstore(address: H160, index: H256) -> RuntimeEvent {
        RuntimeEvent::SStore {
            address,
            index,
            value: H256::zero(),
        }
    }

    #[test]
    fn access_list_records_storage_keys_per_address() {
        let mut listener = Listener::default();
        listener.runtime_event(sload(address(0x1111), H256::from_low_u64_be(1)));
        listener.runtime_event(sstore(address(0x2222), H256::from_low_u64_be(2)));

        let access_list = listener.finish_transaction();

        assert_eq!(access_list.len(), 2);
        let mut items: Vec<_> = access_list.iter().collect();
        items.sort_by_key(|item| item.address);
        assert_eq!(items[0].address, address(0x1111));
        assert_eq!(items[0].storage_keys, vec![H256::from_low_u64_be(1)]);
        assert_eq!(items[1].address, address(0x2222));
        assert_eq!(items[1].storage_keys, vec![H256::from_low_u64_be(2)]);
    }

    #[test]
    fn access_list_deduplicates_repeated_keys() {
        let mut listener = Listener::default();
        listener.runtime_event(sload(address(0x1111), H256::from_low_u64_be(1)));
        listener.runtime_event(sload(address(0x1111), H256::from_low_u64_be(1)));
        listener.runtime_event(sstore(address(0x1111), H256::from_low_u64_be(1)));

        let access_list = listener.finish_transaction();

        assert_eq!(access_list.len(), 1);
        assert_eq!(access_list[0].storage_keys, vec![H256::from_low_u64_be(1)]);
    }

    #[test]
    fn access_list_ignores_non_storage_events() {
        let mut listener = Listener::default();
        listener.runtime_event(RuntimeEvent::Step {
            context: crate::trace::tracing::Context {
                address: address(0x1111),
                caller: address(0x2222),
                apparent_value: 0u64.into(),
            },
            opcode: vec![0x60],
            position: Ok(0),
            stack: None,
            memory: None,
        });

        assert!(listener.finish_transaction().is_empty());
    }
}
