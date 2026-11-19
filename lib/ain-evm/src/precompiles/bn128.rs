use ethereum_types::U256;
use evm::{
    executor::stack::{PrecompileFailure, PrecompileHandle, PrecompileOutput},
    ExitError, ExitSucceed,
};

use crate::precompiles::{Precompile, PrecompileResult};

/// Copy bytes from input to target.
fn read_input(source: &[u8], target: &mut [u8], offset: usize) {
    // Out of bounds, nothing to copy.
    if source.len() <= offset {
        return;
    }

    // Find len to copy up to target len, but not out of bounds.
    let len = core::cmp::min(target.len(), source.len() - offset);
    target[..len].copy_from_slice(&source[offset..][..len]);
}

fn read_fr(input: &[u8], start_inx: usize) -> Result<substrate_bn::Fr, PrecompileFailure> {
    let mut buf = [0u8; 32];
    read_input(input, &mut buf, start_inx);

    substrate_bn::Fr::from_slice(&buf).map_err(|_| PrecompileFailure::Error {
        exit_status: ExitError::Other("Invalid field element".into()),
    })
}

fn read_point(input: &[u8], start_inx: usize) -> Result<substrate_bn::G1, PrecompileFailure> {
    use substrate_bn::{AffineG1, Fq, Group, G1};

    let mut px_buf = [0u8; 32];
    let mut py_buf = [0u8; 32];
    read_input(input, &mut px_buf, start_inx);
    read_input(input, &mut py_buf, start_inx + 32);

    let px = Fq::from_slice(&px_buf).map_err(|_| PrecompileFailure::Error {
        exit_status: ExitError::Other("Invalid point x coordinate".into()),
    })?;

    let py = Fq::from_slice(&py_buf).map_err(|_| PrecompileFailure::Error {
        exit_status: ExitError::Other("Invalid point y coordinate".into()),
    })?;

    Ok(if px == Fq::zero() && py == Fq::zero() {
        G1::zero()
    } else {
        AffineG1::new(px, py)
            .map_err(|_| PrecompileFailure::Error {
                exit_status: ExitError::Other("Invalid curve point".into()),
            })?
            .into()
    })
}

/// The Bn128Add builtin
pub struct Bn128Add;

impl Bn128Add {
    const GAS_COST: u64 = 150; // https://eips.ethereum.org/EIPS/eip-1108
}

impl Precompile for Bn128Add {
    fn execute(handle: &mut impl PrecompileHandle) -> PrecompileResult {
        use substrate_bn::AffineG1;

        handle.record_cost(Bn128Add::GAS_COST)?;

        let input = handle.input();

        let p1 = read_point(input, 0)?;
        let p2 = read_point(input, 64)?;

        let mut buf = [0u8; 64];
        if let Some(sum) = AffineG1::from_jacobian(p1 + p2) {
            // point not at infinity
            sum.x()
                .to_big_endian(&mut buf[0..32])
                .map_err(|_| PrecompileFailure::Error {
                    exit_status: ExitError::Other(
                        "Cannot fail since 0..32 is 32-byte length".into(),
                    ),
                })?;
            sum.y()
                .to_big_endian(&mut buf[32..64])
                .map_err(|_| PrecompileFailure::Error {
                    exit_status: ExitError::Other(
                        "Cannot fail since 32..64 is 32-byte length".into(),
                    ),
                })?;
        }

        Ok(PrecompileOutput {
            exit_status: ExitSucceed::Returned,
            state_changes: None,
            output: buf.to_vec(),
        })
    }
}

/// The Bn128Mul builtin
pub struct Bn128Mul;

impl Bn128Mul {
    const GAS_COST: u64 = 6_000; // https://eips.ethereum.org/EIPS/eip-1108
}

impl Precompile for Bn128Mul {
    fn execute(handle: &mut impl PrecompileHandle) -> PrecompileResult {
        use substrate_bn::AffineG1;

        handle.record_cost(Bn128Mul::GAS_COST)?;

        let input = handle.input();

        let p = read_point(input, 0)?;
        let fr = read_fr(input, 64)?;

        let mut buf = [0u8; 64];
        if let Some(sum) = AffineG1::from_jacobian(p * fr) {
            // point not at infinity
            sum.x()
                .to_big_endian(&mut buf[0..32])
                .map_err(|_| PrecompileFailure::Error {
                    exit_status: ExitError::Other(
                        "Cannot fail since 0..32 is 32-byte length".into(),
                    ),
                })?;
            sum.y()
                .to_big_endian(&mut buf[32..64])
                .map_err(|_| PrecompileFailure::Error {
                    exit_status: ExitError::Other(
                        "Cannot fail since 32..64 is 32-byte length".into(),
                    ),
                })?;
        }

        Ok(PrecompileOutput {
            exit_status: ExitSucceed::Returned,
            state_changes: None,
            output: buf.to_vec(),
        })
    }
}

/// The Bn128Pairing builtin
pub struct Bn128Pairing;

impl Bn128Pairing {
    // https://eips.ethereum.org/EIPS/eip-1108
    const BASE_GAS_COST: u64 = 45_000;
    const GAS_COST_PER_PAIRING: u64 = 34_000;
}

impl Precompile for Bn128Pairing {
    fn execute(handle: &mut impl PrecompileHandle) -> PrecompileResult {
        use substrate_bn::{pairing_batch, AffineG1, AffineG2, Fq, Fq2, Group, Gt, G1, G2};

        let ret_val = if handle.input().is_empty() {
            handle.record_cost(Bn128Pairing::BASE_GAS_COST)?;
            U256::one()
        } else {
            if handle.input().len() % 192 > 0 {
                return Err(PrecompileFailure::Error {
                    exit_status: ExitError::Other("bad elliptic curve pairing size".into()),
                });
            }

            // (a, b_a, b_b - each 64-byte affine coordinates)
            let elements = handle.input().len() / 192;

            let gas_cost: u64 = Bn128Pairing::BASE_GAS_COST
                + (elements as u64 * Bn128Pairing::GAS_COST_PER_PAIRING);

            handle.record_cost(gas_cost)?;

            let input = handle.input();

            let mut vals = Vec::new();
            for idx in 0..elements {
                let a_x = Fq::from_slice(&input[idx * 192..idx * 192 + 32]).map_err(|_| {
                    PrecompileFailure::Error {
                        exit_status: ExitError::Other("Invalid a argument x coordinate".into()),
                    }
                })?;

                let a_y = Fq::from_slice(&input[idx * 192 + 32..idx * 192 + 64]).map_err(|_| {
                    PrecompileFailure::Error {
                        exit_status: ExitError::Other("Invalid a argument y coordinate".into()),
                    }
                })?;

                let b_a_y =
                    Fq::from_slice(&input[idx * 192 + 64..idx * 192 + 96]).map_err(|_| {
                        PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid b argument imaginary coeff x coordinate".into(),
                            ),
                        }
                    })?;

                let b_a_x =
                    Fq::from_slice(&input[idx * 192 + 96..idx * 192 + 128]).map_err(|_| {
                        PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid b argument imaginary coeff y coordinate".into(),
                            ),
                        }
                    })?;

                let b_b_y =
                    Fq::from_slice(&input[idx * 192 + 128..idx * 192 + 160]).map_err(|_| {
                        PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid b argument real coeff x coordinate".into(),
                            ),
                        }
                    })?;

                let b_b_x =
                    Fq::from_slice(&input[idx * 192 + 160..idx * 192 + 192]).map_err(|_| {
                        PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid b argument real coeff y coordinate".into(),
                            ),
                        }
                    })?;

                let b_a = Fq2::new(b_a_x, b_a_y);
                let b_b = Fq2::new(b_b_x, b_b_y);
                let b = if b_a.is_zero() && b_b.is_zero() {
                    G2::zero()
                } else {
                    G2::from(
                        AffineG2::new(b_a, b_b).map_err(|_| PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid b argument - not on curve".into(),
                            ),
                        })?,
                    )
                };
                let a = if a_x.is_zero() && a_y.is_zero() {
                    G1::zero()
                } else {
                    G1::from(
                        AffineG1::new(a_x, a_y).map_err(|_| PrecompileFailure::Error {
                            exit_status: ExitError::Other(
                                "Invalid a argument - not on curve".into(),
                            ),
                        })?,
                    )
                };
                vals.push((a, b));
            }

            let mul = pairing_batch(&vals);

            if mul == Gt::one() {
                U256::one()
            } else {
                U256::zero()
            }
        };

        let mut buf = [0u8; 32];
        ret_val.to_big_endian(&mut buf);

        Ok(PrecompileOutput {
            exit_status: ExitSucceed::Returned,
            state_changes: None,
            output: buf.to_vec(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::precompiles::test_vector_support::{test_precompile_test_vectors, MockHandle};
    use evm::Context;

    fn context() -> Context {
        Context {
            address: Default::default(),
            caller: Default::default(),
            apparent_value: From::from(0),
        }
    }

    fn handle(input: Vec<u8>) -> MockHandle {
        MockHandle::new(input, Some(10_000_000), context())
    }

    fn error_message(failure: &PrecompileFailure) -> String {
        match failure {
            PrecompileFailure::Error { exit_status } => match exit_status {
                ExitError::Other(msg) => msg.to_string(),
                other => format!("{other:?}"),
            },
            PrecompileFailure::Revert { exit_status, .. } => format!("revert: {exit_status:?}"),
            PrecompileFailure::Fatal { exit_status } => format!("fatal: {exit_status:?}"),
        }
    }

    #[test]
    fn process_consensus_tests_for_add() -> Result<(), String> {
        test_precompile_test_vectors::<Bn128Add>("./testdata/common_bnadd.json")?;
        Ok(())
    }

    #[test]
    fn process_consensus_tests_for_mul() -> Result<(), String> {
        test_precompile_test_vectors::<Bn128Mul>("./testdata/common_bnmul.json")?;
        Ok(())
    }

    #[test]
    fn process_consensus_tests_for_pair() -> Result<(), String> {
        test_precompile_test_vectors::<Bn128Pairing>("./testdata/common_bnpair.json")?;
        Ok(())
    }

    // --- bn128 add ----------------------------------------------------------------------

    #[test]
    fn test_bn128_add_gas_cost() {
        // The point at infinity is encoded as two zero coordinates.
        let mut handle = handle(vec![0u8; 128]);

        let result = Bn128Add::execute(&mut handle).expect("bn128 add must not fail");

        assert_eq!(result.output, vec![0u8; 64]);
        // https://eips.ethereum.org/EIPS/eip-1108
        assert_eq!(handle.gas_used, 150);
        assert_eq!(handle.gas_used, Bn128Add::GAS_COST);
    }

    #[test]
    fn test_bn128_add_invalid_curve_point() {
        // (1, 1) is not a point on the curve (`y^2 = x^3 + 3`).
        let mut input = vec![0u8; 128];
        input[31] = 1;
        input[63] = 1;
        let mut handle = handle(input);

        let failure = Bn128Add::execute(&mut handle).expect_err("invalid curve point");

        assert_eq!(error_message(&failure), "Invalid curve point");
        // The gas is charged before the points are read.
        assert_eq!(handle.gas_used, 150);
    }

    #[test]
    fn test_bn128_add_invalid_point_x_coordinate() {
        // The x coordinate is not a member of the field.
        let mut input = vec![0u8; 128];
        input[..32].iter_mut().for_each(|byte| *byte = 0xff);
        let mut handle = handle(input);

        let failure = Bn128Add::execute(&mut handle).expect_err("invalid x coordinate");

        assert_eq!(error_message(&failure), "Invalid point x coordinate");
        assert_eq!(handle.gas_used, 150);
    }

    // --- bn128 mul ----------------------------------------------------------------------

    #[test]
    fn test_bn128_mul_gas_cost() {
        // The point at infinity multiplied by zero.
        let mut handle = handle(vec![0u8; 96]);

        let result = Bn128Mul::execute(&mut handle).expect("bn128 mul must not fail");

        assert_eq!(result.output, vec![0u8; 64]);
        // https://eips.ethereum.org/EIPS/eip-1108
        assert_eq!(handle.gas_used, 6_000);
        assert_eq!(handle.gas_used, Bn128Mul::GAS_COST);
    }

    #[test]
    fn test_bn128_mul_invalid_curve_point() {
        // (1, 1) is not a point on the curve (`y^2 = x^3 + 3`).
        let mut input = vec![0u8; 96];
        input[31] = 1;
        input[63] = 1;
        let mut handle = handle(input);

        let failure = Bn128Mul::execute(&mut handle).expect_err("invalid curve point");

        assert_eq!(error_message(&failure), "Invalid curve point");
        assert_eq!(handle.gas_used, 6_000);
    }

    #[test]
    fn test_bn128_mul_scalar_is_reduced_modulo_the_field_order() {
        // The scalar is reduced modulo the order of the field, hence a scalar that is larger
        // than the modulus is accepted (the "Invalid field element" error is not reachable,
        // because the scalar is always read into a 32 byte buffer).
        let mut input = vec![0u8; 96];
        input[64..96].iter_mut().for_each(|byte| *byte = 0xff);
        let mut handle = handle(input);

        let result = Bn128Mul::execute(&mut handle).expect("bn128 mul must not fail");

        assert_eq!(result.output.len(), 64);
        assert_eq!(handle.gas_used, 6_000);
    }

    // --- bn128 pairing ------------------------------------------------------------------

    #[test]
    fn test_bn128_pairing_gas_cost() {
        // An empty input is the empty pairing and costs the base price.
        let mut handle = handle(vec![]);

        let result = Bn128Pairing::execute(&mut handle).expect("bn128 pairing must not fail");

        assert_eq!(result.output.len(), 32);
        assert_eq!(result.output[31], 1);
        assert_eq!(handle.gas_used, 45_000);
    }

    #[test]
    fn test_bn128_pairing_gas_cost_per_pairing() {
        // One pair of points (the point at infinity) costs the base price plus the price of
        // a single pairing.
        let mut handle = handle(vec![0u8; 192]);

        let result = Bn128Pairing::execute(&mut handle).expect("bn128 pairing must not fail");

        assert_eq!(result.output.len(), 32);
        assert_eq!(result.output[31], 1);
        assert_eq!(handle.gas_used, 45_000 + 34_000);
        assert_eq!(handle.gas_used, 79_000);
    }

    #[test]
    fn test_bn128_pairing_invalid_input_size() {
        // The input size must be a multiple of 192 bytes.
        let mut handle = handle(vec![0u8; 100]);

        let failure = Bn128Pairing::execute(&mut handle).expect_err("invalid input size");

        assert_eq!(error_message(&failure), "bad elliptic curve pairing size");
        // The gas is charged after the input size has been validated.
        assert_eq!(handle.gas_used, 0);
    }

    #[test]
    fn test_bn128_pairing_invalid_a_argument() {
        // (1, 1) is not a point on the curve (`y^2 = x^3 + 3`).
        let mut input = vec![0u8; 192];
        input[31] = 1;
        input[63] = 1;
        let mut handle = handle(input);

        let failure = Bn128Pairing::execute(&mut handle).expect_err("invalid a argument");

        assert_eq!(error_message(&failure), "Invalid a argument - not on curve");
        assert_eq!(handle.gas_used, 79_000);
    }
}
