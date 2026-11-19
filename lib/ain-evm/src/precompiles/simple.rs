// SPDX-License-Identifier: Apache-2.0
// This file is part of Frontier.
//
// Copyright (c) 2020-2022 Parity Technologies (UK) Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// 	http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use core::cmp::min;

use evm::{executor::stack::PrecompileFailure, ExitError, ExitSucceed};

use crate::precompiles::LinearCostPrecompile;

/// The identity precompile.
pub struct Identity;

impl LinearCostPrecompile for Identity {
    const BASE: u64 = 15;
    const WORD: u64 = 3;

    fn execute(input: &[u8], _: u64) -> Result<(ExitSucceed, Vec<u8>), PrecompileFailure> {
        Ok((ExitSucceed::Returned, input.to_vec()))
    }
}

/// The ecrecover precompile.
pub struct ECRecover;

impl LinearCostPrecompile for ECRecover {
    const BASE: u64 = 3000;
    const WORD: u64 = 0;

    fn execute(i: &[u8], _: u64) -> Result<(ExitSucceed, Vec<u8>), PrecompileFailure> {
        let mut input = [0u8; 128];
        input[..min(i.len(), 128)].copy_from_slice(&i[..min(i.len(), 128)]);

        let mut msg = [0u8; 32];
        let mut sig = [0u8; 65];

        msg[0..32].copy_from_slice(&input[0..32]);
        sig[0..32].copy_from_slice(&input[64..96]); // r
        sig[32..64].copy_from_slice(&input[96..128]); // s
        sig[64] = input[63]; // v

        // v can only be 27 or 28 on the full 32 bytes value.
        // https://github.com/ethereum/go-ethereum/blob/a907d7e81aaeea15d80b2d3209ad8e08e3bf49e0/core/vm/contracts.go#L177
        if input[32..63] != [0u8; 31] || ![27, 28].contains(&input[63]) {
            return Ok((ExitSucceed::Returned, [0u8; 0].to_vec()));
        }

        let result = match sp_io::crypto::secp256k1_ecdsa_recover(&sig, &msg) {
            Ok(pubkey) => {
                let mut address = sp_io::hashing::keccak_256(&pubkey);
                address[0..12].copy_from_slice(&[0u8; 12]);
                address.to_vec()
            }
            Err(_) => [0u8; 0].to_vec(),
        };

        Ok((ExitSucceed::Returned, result))
    }
}

/// The ripemd precompile.
pub struct Ripemd160;

impl LinearCostPrecompile for Ripemd160 {
    const BASE: u64 = 600;
    const WORD: u64 = 120;

    fn execute(input: &[u8], _cost: u64) -> Result<(ExitSucceed, Vec<u8>), PrecompileFailure> {
        use ripemd::Digest;

        let mut ret = [0u8; 32];
        ret[12..32].copy_from_slice(&ripemd::Ripemd160::digest(input));
        Ok((ExitSucceed::Returned, ret.to_vec()))
    }
}

/// The sha256 precompile.
pub struct Sha256;

impl LinearCostPrecompile for Sha256 {
    const BASE: u64 = 60;
    const WORD: u64 = 12;

    fn execute(input: &[u8], _cost: u64) -> Result<(ExitSucceed, Vec<u8>), PrecompileFailure> {
        let ret = sp_io::hashing::sha2_256(input);
        Ok((ExitSucceed::Returned, ret.to_vec()))
    }
}

/// The ECRecoverPublicKey precompile.
/// Similar to `ECRecover`, but returns the pubkey (not the corresponding Ethereum address)
pub struct ECRecoverPublicKey;

impl LinearCostPrecompile for ECRecoverPublicKey {
    const BASE: u64 = 3000;
    const WORD: u64 = 0;

    fn execute(i: &[u8], _: u64) -> Result<(ExitSucceed, Vec<u8>), PrecompileFailure> {
        let mut input = [0u8; 128];
        input[..min(i.len(), 128)].copy_from_slice(&i[..min(i.len(), 128)]);

        let mut msg = [0u8; 32];
        let mut sig = [0u8; 65];

        msg[0..32].copy_from_slice(&input[0..32]);
        sig[0..32].copy_from_slice(&input[64..96]);
        sig[32..64].copy_from_slice(&input[96..128]);
        sig[64] = input[63];

        let pubkey = sp_io::crypto::secp256k1_ecdsa_recover(&sig, &msg).map_err(|_| {
            PrecompileFailure::Error {
                exit_status: ExitError::Other("Public key recover failed".into()),
            }
        })?;

        Ok((ExitSucceed::Returned, pubkey.to_vec()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::precompiles::test_vector_support::{test_precompile_test_vectors, MockHandle};
    use crate::precompiles::Precompile;
    use evm::Context;

    fn context() -> Context {
        Context {
            address: Default::default(),
            caller: Default::default(),
            apparent_value: From::from(0),
        }
    }

    fn handle(input: Vec<u8>, gas_limit: u64) -> MockHandle {
        MockHandle::new(input, Some(gas_limit), context())
    }

    /// The linear gas cost of a precompile: `base + word * ceil(len / 32)`.
    fn linear_cost(len: usize, base: u64, word: u64) -> u64 {
        base + word * ((len as u64 + 31) / 32)
    }

    // TODO: this fails on the test "InvalidHighV-bits-1" where it is expected to return ""
    #[test]
    fn process_consensus_tests_for_ecrecover() -> Result<(), String> {
        test_precompile_test_vectors::<ECRecover>("./testdata/ecRecover.json")?;
        Ok(())
    }

    #[test]
    fn process_consensus_tests_for_sha256() -> Result<(), String> {
        test_precompile_test_vectors::<Sha256>("./testdata/common_sha256.json")?;
        Ok(())
    }

    #[test]
    fn process_consensus_tests_for_ripemd160() -> Result<(), String> {
        test_precompile_test_vectors::<Ripemd160>("./testdata/common_ripemd.json")?;
        Ok(())
    }

    // --- identity -----------------------------------------------------------------------

    #[test]
    fn test_identity_returns_the_input() {
        let input = hex::decode("00112233445566778899aabbccddeeff").expect("valid hex");
        let mut handle = handle(input.clone(), 100_000);

        let result =
            <Identity as Precompile>::execute(&mut handle).expect("identity must not fail");

        assert_eq!(result.output, input);
        assert!(result.state_changes.is_none());
    }

    #[test]
    fn test_identity_gas_cost() {
        for len in [0usize, 1, 31, 32, 33, 64, 65] {
            let input = vec![0u8; len];
            let mut handle = handle(input, 100_000);

            <Identity as Precompile>::execute(&mut handle).expect("identity must not fail");

            assert_eq!(
                handle.gas_used,
                linear_cost(len, Identity::BASE, Identity::WORD),
                "unexpected gas cost for {len} bytes"
            );
            assert_eq!(handle.gas_used, 15 + 3 * ((len as u64 + 31) / 32));
        }
    }

    #[test]
    fn test_identity_out_of_gas() {
        let input = vec![0u8; 32];
        // The linear cost of 32 bytes is 18, a gas limit of 17 must fail.
        let mut handle = handle(input, 17);

        let failure = <Identity as Precompile>::execute(&mut handle).expect_err("out of gas");

        assert!(matches!(
            failure,
            PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas
            }
        ));
        assert_eq!(handle.gas_used, 0);
    }

    // --- sha256 -------------------------------------------------------------------------

    #[test]
    fn test_sha256_known_digest() {
        // sha256("abc")
        let mut handle = handle(b"abc".to_vec(), 100_000);

        let result = <Sha256 as Precompile>::execute(&mut handle).expect("sha256 must not fail");

        assert_eq!(
            hex::encode(result.output),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn test_sha256_gas_cost() {
        for len in [0usize, 1, 31, 32, 33, 64, 65] {
            let input = vec![0u8; len];
            let mut handle = handle(input, 100_000);

            <Sha256 as Precompile>::execute(&mut handle).expect("sha256 must not fail");

            assert_eq!(
                handle.gas_used,
                linear_cost(len, Sha256::BASE, Sha256::WORD),
                "unexpected gas cost for {len} bytes"
            );
            assert_eq!(handle.gas_used, 60 + 12 * ((len as u64 + 31) / 32));
        }
    }

    #[test]
    fn test_sha256_out_of_gas() {
        // The linear cost of 32 bytes is 72, a gas limit of 71 must fail.
        let mut handle = handle(vec![0u8; 32], 71);

        let failure = <Sha256 as Precompile>::execute(&mut handle).expect_err("out of gas");

        assert!(matches!(
            failure,
            PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas
            }
        ));
    }

    // --- ripemd160 ----------------------------------------------------------------------

    #[test]
    fn test_ripemd160_known_digest() {
        // ripemd160("abc")
        let mut handle = handle(b"abc".to_vec(), 100_000);

        let result =
            <Ripemd160 as Precompile>::execute(&mut handle).expect("ripemd160 must not fail");

        assert_eq!(result.output.len(), 32);
        assert_eq!(&result.output[..12], &[0u8; 12]);
        assert_eq!(
            hex::encode(&result.output[12..]),
            "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"
        );
    }

    #[test]
    fn test_ripemd160_gas_cost() {
        for len in [0usize, 1, 31, 32, 33, 64, 65] {
            let input = vec![0u8; len];
            let mut handle = handle(input, 100_000);

            <Ripemd160 as Precompile>::execute(&mut handle).expect("ripemd160 must not fail");

            assert_eq!(
                handle.gas_used,
                linear_cost(len, Ripemd160::BASE, Ripemd160::WORD),
                "unexpected gas cost for {len} bytes"
            );
            assert_eq!(handle.gas_used, 600 + 120 * ((len as u64 + 31) / 32));
        }
    }

    #[test]
    fn test_ripemd160_out_of_gas() {
        // The linear cost of 32 bytes is 720, a gas limit of 719 must fail.
        let mut handle = handle(vec![0u8; 32], 719);

        let failure = <Ripemd160 as Precompile>::execute(&mut handle).expect_err("out of gas");

        assert!(matches!(
            failure,
            PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas
            }
        ));
    }

    // --- ecrecover ----------------------------------------------------------------------

    #[test]
    fn test_ecrecover_gas_cost() {
        let mut handle = handle(vec![0u8; 128], 100_000);

        <ECRecover as Precompile>::execute(&mut handle).expect("ecrecover must not fail");

        // The ecrecover precompile has a flat cost of 3000 gas.
        assert_eq!(handle.gas_used, 3000);
        assert_eq!(handle.gas_used, ECRecover::BASE);
    }

    #[test]
    fn test_ecrecover_invalid_v_returns_an_empty_output() {
        // `v` must be 27 or 28, otherwise an empty output is returned.
        let mut input = vec![0u8; 128];
        input[63] = 29;
        let mut handle = handle(input, 100_000);

        let result =
            <ECRecover as Precompile>::execute(&mut handle).expect("ecrecover must not fail");

        assert!(result.output.is_empty());
        assert_eq!(handle.gas_used, 3000);
    }

    #[test]
    fn test_ecrecover_out_of_gas() {
        // The ecrecover precompile costs 3000 gas, a gas limit of 2999 must fail.
        let mut handle = handle(vec![0u8; 128], 2999);

        let failure = <ECRecover as Precompile>::execute(&mut handle).expect_err("out of gas");

        assert!(matches!(
            failure,
            PrecompileFailure::Error {
                exit_status: ExitError::OutOfGas
            }
        ));
    }
}
