use std::collections::BTreeMap;
use std::fs;

use ethereum_types::{H160, H256};
use evm::{
    backend::Basic, executor::stack::PrecompileHandle, Context, ExitError, ExitReason, ExitSucceed,
    Transfer,
};

use crate::precompiles::Precompile;

#[derive(Debug, serde::Deserialize)]
#[serde(rename_all = "PascalCase")]
struct EthConsensusTest {
    input: String,
    expected: String,
    name: String,
    gas: Option<u64>,
}

pub struct MockHandle {
    pub input: Vec<u8>,
    pub gas_limit: Option<u64>,
    pub context: Context,
    pub is_static: bool,
    pub gas_used: u64,
    /// The address of the precompile that is being executed.
    pub code_address: H160,
    /// The storage of the mocked accounts, keyed by `(address, storage index)`.
    pub storage: BTreeMap<(H160, H256), H256>,
    /// The basic account information returned for every address.
    pub basic: Basic,
}

impl MockHandle {
    pub fn new(input: Vec<u8>, gas_limit: Option<u64>, context: Context) -> Self {
        Self {
            input,
            gas_limit,
            context,
            is_static: false,
            gas_used: 0,
            code_address: H160::zero(),
            storage: BTreeMap::new(),
            basic: Basic::default(),
        }
    }

    /// Sets the address of the precompile that is being executed.
    pub fn with_code_address(mut self, code_address: H160) -> Self {
        self.code_address = code_address;
        self
    }

    /// Sets the basic account information returned for every address.
    pub fn with_basic(mut self, basic: Basic) -> Self {
        self.basic = basic;
        self
    }

    /// Sets a single storage entry of the given account.
    pub fn with_storage_entry(mut self, address: H160, index: H256, value: H256) -> Self {
        self.storage.insert((address, index), value);
        self
    }
}

impl PrecompileHandle for MockHandle {
    /// Perform subcall in provided context.
    /// Precompile specifies in which context the subcall is executed.
    fn call(
        &mut self,
        _: H160,
        _: Option<Transfer>,
        _: Vec<u8>,
        _: Option<u64>,
        _: bool,
        _: &Context,
    ) -> (ExitReason, Vec<u8>) {
        unimplemented!()
    }

    fn record_cost(&mut self, cost: u64) -> Result<(), ExitError> {
        self.gas_used += cost;
        Ok(())
    }

    fn log(&mut self, _: H160, _: Vec<H256>, _: Vec<u8>) -> Result<(), ExitError> {
        unimplemented!()
    }

    fn remaining_gas(&self) -> u64 {
        unimplemented!()
    }

    fn code_address(&self) -> H160 {
        self.code_address
    }

    fn input(&self) -> &[u8] {
        &self.input
    }

    fn context(&self) -> &Context {
        &self.context
    }

    fn is_static(&self) -> bool {
        self.is_static
    }

    fn gas_limit(&self) -> Option<u64> {
        self.gas_limit
    }

    fn storage(&self, address: H160, index: H256) -> H256 {
        self.storage
            .get(&(address, index))
            .copied()
            .unwrap_or_else(H256::zero)
    }

    fn basic(&self, _: H160) -> Basic {
        self.basic.clone()
    }
}

/// Tests a precompile against the ethereum consensus tests defined in the given file at filepath.
/// The file is expected to be in JSON format and contain an array of test vectors, where each
/// vector can be deserialized into an "EthConsensusTest".
pub fn test_precompile_test_vectors<P: Precompile>(filepath: &str) -> Result<(), String> {
    let data =
        fs::read_to_string(filepath).unwrap_or_else(|_| panic!("Failed to read {}", filepath));

    let tests: Vec<EthConsensusTest> = serde_json::from_str(&data).expect("expected json array");

    for test in tests {
        let input: Vec<u8> = hex::decode(test.input).expect("Could not hex-decode test input data");

        let cost: u64 = 10000000;

        let context: Context = Context {
            address: Default::default(),
            caller: Default::default(),
            apparent_value: From::from(0),
        };

        let mut handle = MockHandle::new(input, Some(cost), context);

        match P::execute(&mut handle) {
            Ok(result) => {
                let as_hex: String = hex::encode(result.output);
                assert_eq!(
                    result.exit_status,
                    ExitSucceed::Returned,
                    "test '{}' returned {:?} (expected 'Returned')",
                    test.name,
                    result.exit_status
                );
                assert_eq!(
                    as_hex, test.expected,
                    "test '{}' failed (different output)",
                    test.name
                );
                if let Some(expected_gas) = test.gas {
                    assert_eq!(
                        handle.gas_used, expected_gas,
                        "test '{}' failed (different gas cost)",
                        test.name
                    );
                }
            }
            Err(err) => {
                return Err(format!("Test '{}' returned error: {:?}", test.name, err));
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use ethereum_types::U256;

    fn context() -> Context {
        Context {
            address: Default::default(),
            caller: Default::default(),
            apparent_value: From::from(0),
        }
    }

    #[test]
    fn test_mock_handle_code_address() {
        let address = H160::repeat_byte(0x42);
        let handle = MockHandle::new(vec![], Some(1), context()).with_code_address(address);

        assert_eq!(handle.code_address(), address);
    }

    #[test]
    fn test_mock_handle_storage() {
        let address = H160::repeat_byte(0x01);
        let other = H160::repeat_byte(0x02);
        let index = H256::from_low_u64_be(7);
        let handle = MockHandle::new(vec![], Some(1), context()).with_storage_entry(
            address,
            index,
            H256::from_low_u64_be(9),
        );

        assert_eq!(handle.storage(address, index), H256::from_low_u64_be(9));
        // Unknown entries default to zero.
        assert_eq!(handle.storage(address, H256::zero()), H256::zero());
        assert_eq!(handle.storage(other, index), H256::zero());
    }

    #[test]
    fn test_mock_handle_basic() {
        let basic = Basic {
            balance: U256::from(11u64),
            nonce: U256::from(22u64),
        };
        let handle = MockHandle::new(vec![], Some(1), context()).with_basic(basic.clone());

        assert_eq!(handle.basic(H160::zero()), basic);
    }
}
