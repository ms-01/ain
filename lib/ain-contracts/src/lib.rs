use std::str::FromStr;

use anyhow::format_err;
use ethereum_types::{H160, H256, U256};
use sp_core::{Blake2Hasher, Hasher};

pub type Result<T> = std::result::Result<T, anyhow::Error>;

pub const DST20_ADDR_PREFIX_BYTE: u8 = 0xff;
const INTRINSICS_ADDR_PREFIX_BYTE: u8 = 0xdf;

// Impl slots used for proxies: 0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc
pub const IMPLEMENTATION_SLOT: H256 = H256([
    0x36, 0x08, 0x94, 0xa1, 0x3b, 0xa1, 0xa3, 0x21, 0x06, 0x67, 0xc8, 0x28, 0x49, 0x2d, 0xb9, 0x8d,
    0xca, 0x3e, 0x20, 0x76, 0xcc, 0x37, 0x35, 0xa9, 0x20, 0xa3, 0xca, 0x50, 0x5d, 0x38, 0x2b, 0xbc,
]);

macro_rules! solc_artifact_path {
    ($project_name:literal, $artifact:literal) => {
        concat!(
            env!("CARGO_TARGET_DIR"),
            "/sol_artifacts/",
            $project_name,
            "/",
            $artifact
        )
    };
}

macro_rules! solc_artifact_content_str {
    ($project_name:literal, $artifact:literal) => {
        include_str!(solc_artifact_path!($project_name, $artifact))
    };
}

macro_rules! solc_artifact_bytecode_str {
    ($project_name:literal, $artifact:literal) => {
        get_bytecode(solc_artifact_content_str!($project_name, $artifact)).unwrap()
    };
}

macro_rules! legacy_artifact_path {
    ($project_name:literal, $artifact:literal) => {
        concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/legacy_0_8_20/",
            $project_name,
            "/",
            $artifact
        )
    };
}

macro_rules! legacy_artifact_content_str {
    ($project_name:literal, $artifact:literal) => {
        include_str!(legacy_artifact_path!($project_name, $artifact))
    };
}

macro_rules! legacy_artifact_bytecode_str {
    ($project_name:literal, $artifact:literal) => {
        get_bytecode(legacy_artifact_content_str!($project_name, $artifact)).unwrap()
    };
}

macro_rules! slice_20b {
    ($byte0:tt, $byte19:tt) => {{
        let mut v = [0; 20];
        v[0] = $byte0;
        v[19] = $byte19;
        v
    }};
}

fn get_bytecode(input: &str) -> Result<Vec<u8>> {
    let bytecode_json: serde_json::Value = serde_json::from_str(input)?;
    let bytecode_raw = bytecode_json["object"]
        .as_str()
        .ok_or_else(|| format_err!("Bytecode object not available".to_string()))?;

    hex::decode(&bytecode_raw[2..]).map_err(|e| format_err!(e.to_string()))
}

pub fn generate_intrinsic_addr(prefix_byte: u8, suffix_num: u64) -> Result<H160> {
    let s = format!("{prefix_byte:x}{suffix_num:0>38x}");
    Ok(H160::from_str(&s)?)
}

pub fn dst20_address_from_token_id(token_id: u64) -> Result<H160> {
    generate_intrinsic_addr(DST20_ADDR_PREFIX_BYTE, token_id)
}

pub fn intrinsics_address_from_id(id: u64) -> Result<H160> {
    generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, id)
}

#[derive(Clone)]
pub struct Contract {
    pub codehash: H256,
    pub runtime_bytecode: Vec<u8>,
    pub init_bytecode: Vec<u8>,
}

#[derive(Clone)]
pub struct FixedContract {
    pub contract: Contract,
    pub fixed_address: H160,
}

lazy_static::lazy_static! {
    pub static ref DFI_RESERVED_CONTRACT: Contract = {
        let bytecode = legacy_artifact_bytecode_str!(
            "dfi_reserved",
            "deployed_bytecode.json"
        );

        Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: Vec::new(),
        }
    };

    pub static ref DFI_INSTRINICS_REGISTRY_CONTRACT : FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!("dfi_intrinsics_registry", "deployed_bytecode.json");
        let input = legacy_artifact_bytecode_str!(
            "dfi_intrinsics_registry",
            "bytecode.json"
        );

        FixedContract {
            contract: Contract {
                codehash: Blake2Hasher::hash(&bytecode),
                runtime_bytecode: bytecode,
                init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x0)),
        }
    };

    pub static ref DFI_INTRINSICS_V1_CONTRACT: FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!("dfi_intrinsics_v1", "deployed_bytecode.json");
        let input = legacy_artifact_bytecode_str!(
            "dfi_intrinsics_v1",
            "bytecode.json"
        );

        FixedContract {
            contract: Contract {
                codehash: Blake2Hasher::hash(&bytecode),
                runtime_bytecode: bytecode,
                init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x2)),
        }
    };

    pub static ref TRANSFERDOMAIN_CONTRACT : FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!("transfer_domain", "deployed_bytecode.json");
        let input = legacy_artifact_bytecode_str!(
            "transfer_domain",
            "bytecode.json"
        );

        FixedContract {
            contract: Contract {
                codehash: Blake2Hasher::hash(&bytecode),
                runtime_bytecode: bytecode,
                init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x1)),
        }
    };

    pub static ref TRANSFERDOMAIN_V1_CONTRACT: FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!("transfer_domain_v1", "deployed_bytecode.json");
        let input = legacy_artifact_bytecode_str!(
            "transfer_domain_v1",
            "bytecode.json"
        );

        FixedContract {
            contract: Contract {
                codehash: Blake2Hasher::hash(&bytecode),
                runtime_bytecode: bytecode,
                init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x3)),
        }
    };

    pub static ref DST20_CONTRACT: Contract = {
        let bytecode = legacy_artifact_bytecode_str!(
            "dst20",
            "deployed_bytecode.json"
        );

        Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: Vec::new(),
        }
    };

    pub static ref DST20_DF25_CONTRACT: Contract = {
        let bytecode = solc_artifact_bytecode_str!(
            "dst20",
            "deployed_bytecode.json"
        );

        Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: Vec::new(),
        }
    };

    pub static ref DST20_V1_CONTRACT: FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!(
            "dst20_v1", "deployed_bytecode.json"
        );
        let input = legacy_artifact_bytecode_str!(
            "dst20_v1", "bytecode.json"
        );

        FixedContract {
            contract: Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x4))
        }
    };

    pub static ref DST20_V2_CONTRACT: FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!(
            "dst20_v2", "deployed_bytecode.json"
        );
        let input = legacy_artifact_bytecode_str!(
            "dst20_v2", "bytecode.json"
        );

        FixedContract {
            contract: Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x5))
        }
    };

    pub static ref DST20_V3_CONTRACT: FixedContract = {
        let bytecode = legacy_artifact_bytecode_str!(
            "dst20_v3", "deployed_bytecode.json"
        );
        let input = legacy_artifact_bytecode_str!(
            "dst20_v3", "bytecode.json"
        );

        FixedContract {
            contract: Contract {
            codehash: Blake2Hasher::hash(&bytecode),
            runtime_bytecode: bytecode,
            init_bytecode: input,
            },
            fixed_address: H160(slice_20b!(INTRINSICS_ADDR_PREFIX_BYTE, 0x6))
        }
    };
}

pub fn get_split_tokens_function() -> ethabi::Function {
    #[allow(deprecated)] // constant field is deprecated since Solidity 0.5.0
    ethabi::Function {
        name: String::from("migrateTokensOnAddress"),
        inputs: vec![
            ethabi::Param {
                name: String::from("sender"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("tokenContract"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("amount"),
                kind: ethabi::ParamType::Uint(256),
                internal_type: None,
            },
        ],
        outputs: vec![],
        constant: None,
        state_mutability: ethabi::StateMutability::NonPayable,
    }
}

pub struct TokenSplitParams {
    pub sender: H160,
    pub token_contract: H160,
    pub amount: U256,
}

pub fn validate_split_tokens_input(input: &[u8]) -> Result<TokenSplitParams> {
    let function = get_split_tokens_function();
    let token_inputs = function.decode_input(input)?;

    let Some(ethabi::Token::Address(sender)) = token_inputs.first() else {
        return Err(format_err!("invalid from address input in evm tx"));
    };
    let Some(ethabi::Token::Address(token_contract)) = token_inputs.get(1).cloned() else {
        return Err(format_err!("invalid from address input in evm tx"));
    };
    let Some(ethabi::Token::Uint(amount)) = token_inputs.get(2).cloned() else {
        return Err(format_err!("invalid value input in evm tx"));
    };

    Ok(TokenSplitParams {
        sender: *sender,
        token_contract,
        amount,
    })
}

pub fn get_transferdomain_native_transfer_function() -> ethabi::Function {
    #[allow(deprecated)] // constant field is deprecated since Solidity 0.5.0
    ethabi::Function {
        name: String::from("transfer"),
        inputs: vec![
            ethabi::Param {
                name: String::from("from"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("to"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("amount"),
                kind: ethabi::ParamType::Uint(256),
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("vmAddress"),
                kind: ethabi::ParamType::String,
                internal_type: None,
            },
        ],
        outputs: vec![],
        constant: None,
        state_mutability: ethabi::StateMutability::NonPayable,
    }
}

pub fn get_transferdomain_dst20_transfer_function() -> ethabi::Function {
    #[allow(deprecated)] // constant field is deprecated since Solidity 0.5.0
    ethabi::Function {
        name: String::from("transferDST20"),
        inputs: vec![
            ethabi::Param {
                name: String::from("contractAddress"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("from"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("to"),
                kind: ethabi::ParamType::Address,
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("amount"),
                kind: ethabi::ParamType::Uint(256),
                internal_type: None,
            },
            ethabi::Param {
                name: String::from("vmAddress"),
                kind: ethabi::ParamType::String,
                internal_type: None,
            },
        ],
        outputs: vec![],
        constant: None,
        state_mutability: ethabi::StateMutability::NonPayable,
    }
}

pub fn get_dfi_reserved_contract() -> Contract {
    DFI_RESERVED_CONTRACT.clone()
}

pub fn get_dfi_instrinics_registry_contract() -> FixedContract {
    DFI_INSTRINICS_REGISTRY_CONTRACT.clone()
}

pub fn get_dfi_intrinsics_v1_contract() -> FixedContract {
    DFI_INTRINSICS_V1_CONTRACT.clone()
}

pub fn get_transfer_domain_contract() -> FixedContract {
    TRANSFERDOMAIN_CONTRACT.clone()
}

pub fn get_transfer_domain_v1_contract() -> FixedContract {
    TRANSFERDOMAIN_V1_CONTRACT.clone()
}

pub fn get_dst20_contract() -> Contract {
    DST20_CONTRACT.clone()
}

pub fn get_dst20_contract_for_height(df25_active: bool) -> Contract {
    if df25_active {
        DST20_DF25_CONTRACT.clone()
    } else {
        DST20_CONTRACT.clone()
    }
}

pub fn is_valid_dst20_codehash(codehash: H256, df25_active: bool) -> bool {
    codehash == DST20_CONTRACT.codehash || (df25_active && codehash == DST20_DF25_CONTRACT.codehash)
}

pub fn get_dst20_v1_contract() -> FixedContract {
    DST20_V1_CONTRACT.clone()
}

pub fn get_dst20_v2_contract() -> FixedContract {
    DST20_V2_CONTRACT.clone()
}

pub fn get_dst20_v3_contract() -> FixedContract {
    DST20_V3_CONTRACT.clone()
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_addr_gen() {
        let items: Vec<(Result<H160>, &str)> = vec![
            (
                generate_intrinsic_addr(DST20_ADDR_PREFIX_BYTE, 0x0),
                "ff00000000000000000000000000000000000000",
            ),
            (
                generate_intrinsic_addr(DST20_ADDR_PREFIX_BYTE, 0x1),
                "ff00000000000000000000000000000000000001",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0x0),
                "df00000000000000000000000000000000000000",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0x1),
                "df00000000000000000000000000000000000001",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0x2),
                "df00000000000000000000000000000000000002",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0xff),
                "df000000000000000000000000000000000000ff",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0xfe),
                "df000000000000000000000000000000000000fe",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0x1fffe),
                "df0000000000000000000000000000000001fffe",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0xffff),
                "df0000000000000000000000000000000000ffff",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0xffff_ffff_ffff_ffff),
                "df0000000000000000000000ffffffffffffffff",
            ),
            (
                generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0xefff_ffff_ffff_ffff),
                "df0000000000000000000000efffffffffffffff",
            ),
            // Should not compile
            // (generate_intrinsic_addr(INTRINSICS_ADDR_PREFIX_BYTE, 0x1ffff_ffff_ffff_ffff), "df0000000000000000000001ffffffffffffffff"),
        ];

        for x in items {
            assert_eq!(x.0.unwrap(), H160::from_str(x.1).unwrap());
        }
    }

    #[test]
    fn test_legacy_0_8_20_codehashes() {
        let expected: Vec<(&str, H256, &str)> = vec![
            (
                "dfi_intrinsics_registry",
                get_dfi_instrinics_registry_contract().contract.codehash,
                "26bce352a949081ab296a169e857aa54d5770b53ec8c708d50c27a7cfbe3dec4",
            ),
            (
                "dfi_intrinsics_v1",
                get_dfi_intrinsics_v1_contract().contract.codehash,
                "7be69135a84b4de6fe4e24233ba259267d2ec4f7e6a186cc53d72d065e42705c",
            ),
            (
                "dfi_reserved",
                get_dfi_reserved_contract().codehash,
                "7ba4d6a9e9e0a5b8d3f1f9a776f336c424e47e1efb7bf52a15faacff7c48cb1f",
            ),
            (
                "dst20",
                get_dst20_contract().codehash,
                "dfffa60996ce02e0ddc0d4bc3850eac114e05bf42f492444cd0f7d7c5c7d4e47",
            ),
            (
                "dst20_v1",
                get_dst20_v1_contract().contract.codehash,
                "0a97b99384145cffbe13925cc8a31cb214fc26f51b69c826eadbefd2562c4e91",
            ),
            (
                "dst20_v2",
                get_dst20_v2_contract().contract.codehash,
                "6066bd92817b363d68072a769f163e9d09cb33bc589025c2a2cb65e11e05505e",
            ),
            (
                "dst20_v3",
                get_dst20_v3_contract().contract.codehash,
                "f0fa26c1d53d658c1d015d30694e22380dcbae2571f1e6b41b0fca7d506907c5",
            ),
            (
                "transfer_domain",
                get_transfer_domain_contract().contract.codehash,
                "d7bbe05c66dad95ae472baede92010034253bea633f818bda5a81c9d90ba4f9e",
            ),
            (
                "transfer_domain_v1",
                get_transfer_domain_v1_contract().contract.codehash,
                "053bc823bb40b3a36f7bdc930a5472454e9cae0d3a918ce89426466d81ff4db6",
            ),
        ];

        for (name, actual, expected_hex) in expected {
            let expected_hash = H256::from_str(expected_hex).unwrap();
            assert_eq!(
                actual, expected_hash,
                "legacy 0.8.20 codehash mismatch for {name}"
            );
        }
    }

    fn strip_cbor_metadata(bytecode: &[u8]) -> &[u8] {
        if bytecode.len() >= 2 {
            let meta_len =
                u16::from_be_bytes([bytecode[bytecode.len() - 2], bytecode[bytecode.len() - 1]])
                    as usize;
            if meta_len + 2 <= bytecode.len() {
                return &bytecode[..bytecode.len() - meta_len - 2];
            }
        }
        bytecode
    }

    #[test]
    fn test_dst20_df25_no_cancun_opcodes_and_shanghai_metadata() {
        // Verify effective evmVersion passed to solc via standard-json is shanghai
        let settings: serde_json::Value =
            serde_json::from_str(solc_artifact_content_str!("dst20", "settings.json")).unwrap();
        assert_eq!(
            settings["evmVersion"].as_str(),
            Some("shanghai"),
            "DST20 DF25 artifact must be compiled with evmVersion=shanghai"
        );

        // Opcode-aware scan skipping PUSH1..PUSH32 immediate payloads
        const CANCUN_OPCODES: &[u8] = &[
            0x49, // BLOBHASH
            0x4a, // BLOBBASEFEE
            0x5c, // TLOAD
            0x5d, // TSTORE
            0x5e, // MCOPY
        ];
        let code = strip_cbor_metadata(&DST20_DF25_CONTRACT.runtime_bytecode);
        let mut pc = 0usize;
        while pc < code.len() {
            let op = code[pc];
            assert!(
                !CANCUN_OPCODES.contains(&op),
                "Found Cancun-only opcode 0x{op:02x} at pc={pc} in DST20_DF25_CONTRACT"
            );
            if (0x60..=0x7f).contains(&op) {
                let push_bytes = (op - 0x5f) as usize;
                pc += 1 + push_bytes;
            } else {
                pc += 1;
            }
        }
    }

    #[test]
    fn test_is_valid_dst20_codehash_height_gating() {
        let legacy_hash = DST20_CONTRACT.codehash;
        let df25_hash = DST20_DF25_CONTRACT.codehash;
        let unknown_hash = H256::from_low_u64_be(0xdeadbeef);

        // Below DF25Height: only legacy 0.8.20 facade is deployed and accepted
        assert_eq!(get_dst20_contract_for_height(false).codehash, legacy_hash);
        assert!(is_valid_dst20_codehash(legacy_hash, false));
        assert!(!is_valid_dst20_codehash(unknown_hash, false));
        if df25_hash != legacy_hash {
            assert!(!is_valid_dst20_codehash(df25_hash, false));
        }

        // At/above DF25Height: new facade is deployed, both legacy and new are accepted
        assert_eq!(get_dst20_contract_for_height(true).codehash, df25_hash);
        assert!(is_valid_dst20_codehash(legacy_hash, true));
        assert!(is_valid_dst20_codehash(df25_hash, true));
        assert!(!is_valid_dst20_codehash(unknown_hash, true));
    }
}
