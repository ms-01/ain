mod blake2;
mod bn128;
mod modexp;
mod simple;
mod token_split;

#[cfg(test)]
mod test_vector_support;

use ain_contracts::DST20_ADDR_PREFIX_BYTE;
use blake2::Blake2F;
use bn128::{Bn128Add, Bn128Mul, Bn128Pairing};
use ethereum_types::H160;
use evm::executor::stack::IsPrecompileResult;
pub use evm::{
    executor::stack::{PrecompileFailure, PrecompileHandle, PrecompileOutput, PrecompileSet},
    ExitError, ExitSucceed,
};
use modexp::Modexp;
use simple::{ECRecover, Identity, Ripemd160, Sha256};

use self::token_split::TokenSplit;

type PrecompileResult = Result<PrecompileOutput, PrecompileFailure>;

/// One single precompile used by EVM engine.
pub trait Precompile {
    /// Try to execute the precompile with given `handle` which provides all call data
    /// and allow to register costs and logs.
    fn execute(handle: &mut impl PrecompileHandle) -> PrecompileResult;
}

// Precompile with access to mnview
pub trait DVMStatePrecompile {
    fn execute(handle: &mut impl PrecompileHandle, mnview_ptr: usize) -> PrecompileResult;
}

pub trait LinearCostPrecompile {
    const BASE: u64;
    const WORD: u64;

    fn execute(
        input: &[u8],
        cost: u64,
    ) -> core::result::Result<(ExitSucceed, Vec<u8>), PrecompileFailure>;
}

impl<T: LinearCostPrecompile> Precompile for T {
    fn execute(handle: &mut impl PrecompileHandle) -> PrecompileResult {
        let target_gas = handle.gas_limit();
        let cost = ensure_linear_cost(target_gas, handle.input().len() as u64, T::BASE, T::WORD)?;

        handle.record_cost(cost)?;
        let (exit_status, output) = T::execute(handle.input(), cost)?;
        Ok(PrecompileOutput {
            exit_status,
            output,
            state_changes: None,
        })
    }
}

/// Linear gas cost
fn ensure_linear_cost(
    target_gas: Option<u64>,
    len: u64,
    base: u64,
    word: u64,
) -> Result<u64, PrecompileFailure> {
    let cost = base
        .checked_add(word.checked_mul(len.saturating_add(31) / 32).ok_or(
            PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas,
            },
        )?)
        .ok_or(PrecompileFailure::Error {
            exit_status: ExitError::OutOfGas,
        })?;

    if let Some(target_gas) = target_gas {
        if cost > target_gas {
            return Err(PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas,
            });
        }
    }

    Ok(cost)
}

#[derive(Default)]
pub struct MetachainPrecompiles {
    mnview_ptr: Option<usize>,
    df25_active: bool,
}

// Ethereum precompiles available as of shangai fork :
// Ref: Ethereum Yellow Paper (https://ethereum.github.io/yellowpaper/paper.pdf) Page 12
impl MetachainPrecompiles {
    pub fn new(mnview_ptr: usize) -> Self {
        Self {
            mnview_ptr: Some(mnview_ptr),
            df25_active: false,
        }
    }

    pub fn with_df25_active(mut self, df25_active: bool) -> Self {
        self.df25_active = df25_active;
        self
    }

    pub fn used_addresses() -> [H160; 10] {
        [
            hash(1),
            hash(2),
            hash(3),
            hash(4),
            hash(5),
            hash(6),
            hash(7),
            hash(8),
            hash(9),
            hash(10),
        ]
    }
}

impl PrecompileSet for MetachainPrecompiles {
    fn execute(&self, handle: &mut impl PrecompileHandle) -> Option<PrecompileResult> {
        match handle.code_address() {
            a if a == hash(1) => Some(<ECRecover as Precompile>::execute(handle)),
            a if a == hash(2) => Some(<Sha256 as Precompile>::execute(handle)),
            a if a == hash(3) => Some(<Ripemd160 as Precompile>::execute(handle)),
            a if a == hash(4) => Some(<Identity as Precompile>::execute(handle)),
            a if a == hash(5) => Some(Modexp::execute_with_even_mod_penalty(
                handle,
                !self.df25_active,
            )),
            a if a == hash(6) => Some(Bn128Add::execute(handle)),
            a if a == hash(7) => Some(Bn128Mul::execute(handle)),
            a if a == hash(8) => Some(Bn128Pairing::execute(handle)),
            a if a == hash(9) => Some(Blake2F::execute(handle)),

            a if a == hash(10) && is_dst20(handle.context().caller) => {
                let mnview_ptr = self.mnview_ptr.unwrap_or_default(); // If None, should fetch from global view
                Some(TokenSplit::execute(handle, mnview_ptr))
            }
            _ => None,
        }
    }

    fn is_precompile(&self, address: H160, _gas: u64) -> IsPrecompileResult {
        // The pinned `evm` crate consumes `is_precompile()` exclusively as the switch
        // between its "precompiles are always warm" pricing and its access list based
        // EIP-2929 pricing (`StackExecutor::is_cold`:
        // `!is_precompile && self.state.is_cold(address)`).  go-ethereum charges the
        // cold account access cost (2600 gas) for the *first* access to any address,
        // precompiles included, and only the warm cost (100 gas) for the subsequent
        // ones.
        //
        // Therefore the metachain precompile addresses are reported as *not*
        // precompiles once `DF25Height` is active, which switches the crate to the
        // access list based (go-ethereum like) pricing.  The precompile dispatch
        // itself is unaffected: `execute()` still resolves the precompiles, and the
        // addresses stay precompiles.
        //
        // NOTE: a future dependency bump must re-check that `is_precompile()` is still
        // consumed only for the pricing decision.
        IsPrecompileResult::Answer {
            is_precompile: !self.df25_active && Self::used_addresses().contains(&address),
            extra_cost: 0,
        }
    }
}

fn hash(a: u64) -> H160 {
    H160::from_low_u64_be(a)
}

fn is_dst20(addr: H160) -> bool {
    matches!(addr.as_fixed_bytes(), [prefix, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ..] if prefix == &DST20_ADDR_PREFIX_BYTE)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ethereum_types::H256;
    use evm::{backend::Basic, Context, ExitReason, Transfer};

    /// Builds an address from the given hex string.
    fn address(hex: &str) -> H160 {
        H160::from_slice(&hex::decode(hex).expect("valid hex address"))
    }

    #[test]
    fn test_used_addresses() {
        let used = MetachainPrecompiles::used_addresses();

        assert_eq!(used.len(), 10);
        for (index, address) in used.iter().enumerate() {
            assert_eq!(*address, hash(index as u64 + 1));
        }
    }

    #[test]
    fn test_is_precompile() {
        let precompiles = MetachainPrecompiles::new(0);

        for address in MetachainPrecompiles::used_addresses() {
            match <MetachainPrecompiles as PrecompileSet>::is_precompile(&precompiles, address, 0) {
                IsPrecompileResult::Answer {
                    is_precompile,
                    extra_cost,
                } => {
                    assert!(is_precompile, "{address:x} must be a precompile");
                    assert_eq!(extra_cost, 0);
                }
                _ => panic!("unexpected result"),
            }
        }

        for address in [H160::zero(), hash(0), hash(11)] {
            match <MetachainPrecompiles as PrecompileSet>::is_precompile(&precompiles, address, 0) {
                IsPrecompileResult::Answer {
                    is_precompile,
                    extra_cost,
                } => {
                    assert!(!is_precompile, "{address:x} must not be a precompile");
                    assert_eq!(extra_cost, 0);
                }
                _ => panic!("unexpected result"),
            }
        }
    }

    /// Once `DF25Height` is active the metachain precompile addresses are reported as
    /// *not* precompiles: the pinned `evm` crate uses the answer exclusively as the switch
    /// between its "precompiles are always warm" pricing and its access list based
    /// EIP-2929 pricing, and go-ethereum charges the cold account access cost (2600 gas)
    /// for the first access to a precompile address.
    #[test]
    fn test_is_precompile_when_df25_active() {
        let precompiles = MetachainPrecompiles::new(0).with_df25_active(true);

        for address in MetachainPrecompiles::used_addresses() {
            match <MetachainPrecompiles as PrecompileSet>::is_precompile(&precompiles, address, 0) {
                IsPrecompileResult::Answer {
                    is_precompile,
                    extra_cost,
                } => {
                    assert!(
                        !is_precompile,
                        "{address:x} must not be reported as a precompile"
                    );
                    assert_eq!(extra_cost, 0);
                }
                _ => panic!("unexpected result"),
            }
        }

        for address in [H160::zero(), hash(0), hash(11)] {
            match <MetachainPrecompiles as PrecompileSet>::is_precompile(&precompiles, address, 0) {
                IsPrecompileResult::Answer {
                    is_precompile,
                    extra_cost,
                } => {
                    assert!(!is_precompile, "{address:x} must not be a precompile");
                    assert_eq!(extra_cost, 0);
                }
                _ => panic!("unexpected result"),
            }
        }
    }

    /// The inverted `is_precompile()` answer must not affect the precompile dispatch:
    /// `execute()` still resolves the precompile addresses once `DF25Height` is active.
    #[test]
    fn test_execute_resolves_precompiles_when_df25_active() {
        let precompiles = MetachainPrecompiles::new(0).with_df25_active(true);
        let context = Context {
            address: H160::zero(),
            caller: H160::zero(),
            apparent_value: Default::default(),
        };

        for (index, address) in MetachainPrecompiles::used_addresses().iter().enumerate() {
            // The DST20 token split (0x..0a) requires a DST20 caller, it is skipped here.
            if index == 9 {
                continue;
            }
            let mut handle = MockHandle::new(vec![], *address, context.clone());
            assert!(
                <MetachainPrecompiles as PrecompileSet>::execute(&precompiles, &mut handle)
                    .is_some(),
                "{address:x} must still be dispatched as a precompile"
            );
            if index == 3 {
                // The identity precompile (0x..04) costs 15 gas for an empty input.
                assert_eq!(handle.gas_used, 15);
            }
        }

        // An unregistered address is still not dispatched.
        let mut handle = MockHandle::new(vec![], hash(11), context);
        assert!(
            <MetachainPrecompiles as PrecompileSet>::execute(&precompiles, &mut handle).is_none(),
            "an unregistered address must not be dispatched as a precompile"
        );
    }

    /// A minimal `PrecompileHandle` implementation for the tests above.
    struct MockHandle {
        input: Vec<u8>,
        code_address: H160,
        context: Context,
        gas_used: u64,
    }

    impl MockHandle {
        fn new(input: Vec<u8>, code_address: H160, context: Context) -> Self {
            Self {
                input,
                code_address,
                context,
                gas_used: 0,
            }
        }
    }

    impl PrecompileHandle for MockHandle {
        fn call(
            &mut self,
            _to: H160,
            _transfer: Option<Transfer>,
            _input: Vec<u8>,
            _gas_limit: Option<u64>,
            _is_static: bool,
            _context: &Context,
        ) -> (ExitReason, Vec<u8>) {
            (ExitReason::Succeed(ExitSucceed::Returned), Vec::new())
        }

        fn record_cost(&mut self, cost: u64) -> Result<(), ExitError> {
            self.gas_used += cost;
            Ok(())
        }

        fn remaining_gas(&self) -> u64 {
            u64::MAX
        }

        fn log(
            &mut self,
            _address: H160,
            _topics: Vec<H256>,
            _data: Vec<u8>,
        ) -> Result<(), ExitError> {
            Ok(())
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
            false
        }

        fn gas_limit(&self) -> Option<u64> {
            Some(1_000_000)
        }

        fn storage(&self, _address: H160, _index: H256) -> H256 {
            H256::zero()
        }

        fn basic(&self, _address: H160) -> Basic {
            Basic {
                balance: Default::default(),
                nonce: Default::default(),
            }
        }
    }

    #[test]
    fn test_is_dst20() {
        // The DST20 intrinsic addresses are `0xff` followed by 17 zero bytes, i.e. the token
        // id (which is stored in the last two bytes) must be below 0x10000.
        assert!(is_dst20(address(
            "ff00000000000000000000000000000000000000"
        )));
        assert!(is_dst20(address(
            "ff0000000000000000000000000000000000000a"
        )));
        assert!(is_dst20(address(
            "ff0000000000000000000000000000000000ffff"
        )));
        // A non zero byte in front of the token id does not match.
        assert!(!is_dst20(address(
            "ff00000000000000000000000000000000010000"
        )));
        assert!(!is_dst20(address(
            "fe00000000000000000000000000000000000000"
        )));
        assert!(!is_dst20(address(
            "0000000000000000000000000000000000000000"
        )));
    }
}
