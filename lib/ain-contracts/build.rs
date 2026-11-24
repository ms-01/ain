use std::{env, fs, path::PathBuf};

use anyhow::{bail, Context, Result};
use ethers_solc::{
    artifacts::Optimizer, EvmVersion, Project, ProjectPathsConfig, Solc, SolcConfig,
};

fn main() -> Result<()> {
    let manifest_path = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);

    // We use CARGO_TARGET_DIR from Makefile instead of OUT_DIR.
    // Reason: Currently setting --out-dir is nightly only, so there's no way to get OUT_DIR
    // out of cargo reliably for pointing non cargo deps (eg: python test files) determinisitcally.
    let target_dir: PathBuf = PathBuf::from(env::var("CARGO_TARGET_DIR")?);
    let solc_artifact_dir = target_dir.join("sol_artifacts");

    // Solidity project root and contract names relative to our project
    let contracts = vec![
        ("dfi_reserved", "DFIReserved"),
        ("dfi_intrinsics_registry", "DFIIntrinsicsRegistry"),
        ("dfi_intrinsics_v1", "DFIIntrinsicsV1"),
        ("transfer_domain", "TransferDomain"),
        ("transfer_domain_v1", "TransferDomainV1"),
        ("dst20", "DST20"),
        ("dst20_v1", "DST20V1"),
        ("dst20_v2", "DST20V2"),
        ("dst20_v3", "DST20V3"),
    ];

    for (sol_project_name, contract_name) in contracts {
        let solc = Solc::default();

        let sol_project_root = manifest_path.join(sol_project_name);
        if !sol_project_root.exists() {
            bail!("Solidity project missing: {sol_project_root:?}");
        }

        let paths = ProjectPathsConfig::builder()
            .root(&sol_project_root)
            .sources(&sol_project_root)
            .build()?;

        let mut solc_config = SolcConfig::builder().build();

        solc_config.settings.optimizer = Optimizer {
            enabled: Some(true),
            runs: Some(u32::MAX as usize),
            details: None,
        };

        // Pin the EVM target explicitly.
        //
        // The node implements the `shanghai` EVM (see `lib/ain-evm/src/executor.rs` and
        // `lib/ain-evm/src/gas.rs`), while newer `solc` releases default to a *newer* target
        // (e.g. `cancun`). Compiling the intrinsic contracts for an unsupported target would
        // emit opcodes the node cannot execute (e.g. `MCOPY`) and would change the embedded
        // code hash, which is compared against the on-chain code hash.
        //
        // The pin matches the current effective default of `ethers-solc` (`Shanghai`), so the
        // emitted bytecode stays byte-identical.
        solc_config.settings.evm_version = Some(EvmVersion::Shanghai);
        let effective_settings_json = serde_json::to_string(&solc_config.settings)?;

        let project = Project::builder()
            .solc(solc)
            .solc_config(solc_config)
            .paths(paths)
            .set_auto_detect(true)
            .no_artifacts()
            .build()?;

        let output = project.compile()?;
        let artifacts = output.into_artifacts();
        let sol_project_outdir = solc_artifact_dir.join(sol_project_name);

        for (id, artifact) in artifacts {
            if id.name != contract_name {
                continue;
            }

            let abi = artifact.abi.context("ABI not found")?;
            let bytecode = artifact.bytecode.context("Bytecode not found")?;
            let deployed_bytecode = artifact
                .deployed_bytecode
                .context("Deployed bytecode not found")?;

            // Only the per-token `dst20` proxy facade uses solc 0.8.28 bytecode (at/above
            // DF25Height). All 8 fixed-address / reserved-namespace contracts stay frozen to
            // the legacy 0.8.20 artifacts so `sol_artifacts/` matches on-chain deployments.
            let is_df25_upgraded = sol_project_name == "dst20";
            let legacy_dir = manifest_path.join("legacy_0_8_20").join(sol_project_name);

            let mut items = vec![("abi.json", serde_json::to_string(&abi)?)];
            if is_df25_upgraded {
                items.push(("bytecode.json", serde_json::to_string(&bytecode)?));
                items.push((
                    "deployed_bytecode.json",
                    serde_json::to_string(&deployed_bytecode)?,
                ));
                items.push(("settings.json", effective_settings_json.clone()));
            } else {
                items.push((
                    "bytecode.json",
                    fs::read_to_string(legacy_dir.join("bytecode.json"))?,
                ));
                items.push((
                    "deployed_bytecode.json",
                    fs::read_to_string(legacy_dir.join("deployed_bytecode.json"))?,
                ));
            }

            fs::create_dir_all(&sol_project_outdir)?;
            for (file_name, contents) in items {
                fs::write(sol_project_outdir.join(file_name), contents.as_bytes())?;
            }
        }

        project.rerun_if_sources_changed();
    }
    Ok(())
}
