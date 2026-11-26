use ain_contracts::{get_transfer_domain_contract, FixedContract};
use anyhow::format_err;
use ethereum::{AccessList, EIP658ReceiptData, Log, ReceiptV3};
use ethereum_types::{Bloom, H160, H256, U256};
use evm::{
    backend::{ApplyBackend, Backend},
    executor::stack::{MemoryStackState, StackExecutor, StackSubstateMetadata},
    Config, CreateScheme, ExitReason,
};

use log::trace;

use crate::{
    backend::EVMBackend,
    blocktemplate::ReceiptAndOptionalContractAddress,
    bytes::Bytes,
    contract::{
        bridge_dfi,
        dst20::{
            bridge_dst20_in, bridge_dst20_out, dst20_allowance, dst20_deploy_contract_tx,
            dst20_deploy_info, dst20_name_info, DST20BridgeInfo,
        },
        rename_contract_tx, DeployContractInfo,
    },
    core::EVMCoreService,
    evm::BlockContext,
    fee::{calculate_current_prepay_gas_fee, calculate_gas_fee},
    precompiles::MetachainPrecompiles,
    transaction::{
        system::{
            DST20Data, DeployContractData, ExecuteTx, SystemTx, TransferDirection,
            TransferDomainData, UpdateContractNameData,
        },
        SignedTx,
    },
    Result,
};

#[derive(Debug)]
pub struct ExecutorContext<'a> {
    pub caller: H160,
    pub to: Option<H160>,
    pub value: U256,
    pub data: &'a [u8],
    pub gas_limit: u64,
    pub access_list: AccessList,
}

pub struct AinExecutor<'backend> {
    pub backend: &'backend mut EVMBackend,
}

/// Resolves the effective DVM block height used by height-gated EVM rules.
///
/// The normal execution path always provides a [`BlockContext`] carrying the
/// DVM height of the block that is being processed. The RPC tracer replays
/// blocks without such a context and falls back to the backend vicinity, which
/// is derived from the traced block header (`"DFI: <height>"` extra data) and
/// therefore refers to the very same block.
pub(crate) fn resolve_dvm_block(
    ctx_dvm_block: Option<u64>,
    vicinity_dvm_block: Option<u64>,
) -> Option<u64> {
    ctx_dvm_block.or(vicinity_dvm_block)
}

/// Returns whether the DF25-gated rules are active at the given DVM height.
///
/// The activation height is passed in explicitly to keep this helper free of
/// any C++ FFI dependency and thus unit-testable in isolation.
pub(crate) fn is_df25_active(dvm_block: Option<u64>, df25_height: u64) -> bool {
    dvm_block.is_some_and(|height| height >= df25_height)
}

// State update methods
impl<'backend> AinExecutor<'backend> {
    pub fn new(backend: &'backend mut EVMBackend) -> Self {
        Self { backend }
    }

    pub fn add_balance(&mut self, address: H160, amount: U256) -> Result<()> {
        self.backend.add_balance(address, amount)
    }

    pub fn sub_balance(&mut self, address: H160, amount: U256) -> Result<()> {
        self.backend.sub_balance(address, amount)
    }

    pub fn deploy_contract(
        &mut self,
        address: H160,
        bytecode: Bytes,
        storage: Vec<(H256, H256)>,
    ) -> Result<()> {
        self.backend.deploy_contract(&address, bytecode.0, storage)
    }

    pub fn update_storage(&mut self, address: H160, storage: Vec<(H256, H256)>) -> Result<()> {
        self.backend.update_storage(&address, storage)
    }

    pub fn update_total_gas_used(&mut self, gas_used: U256) {
        self.backend.update_vicinity_with_gas_used(gas_used)
    }

    pub fn commit(&mut self, is_miner: bool) -> Result<H256> {
        self.backend.commit(is_miner)
    }

    pub fn get_nonce(&self, address: &H160) -> U256 {
        self.backend.get_nonce(address)
    }
}

// EVM executor methods
impl<'backend> AinExecutor<'backend> {
    const CONFIG: Config = Config::shanghai();

    /// Read-only call
    pub fn call(&mut self, ctx: ExecutorContext) -> TxResponse {
        let metadata = StackSubstateMetadata::new(ctx.gas_limit, &Self::CONFIG);
        let df25_active = self
            .backend
            .vicinity
            .dvm_block
            .is_some_and(|h| h >= ain_cpp_imports::get_df25_height());
        let state = MemoryStackState::new(metadata, self.backend);
        let precompiles = MetachainPrecompiles::default().with_df25_active(df25_active);
        let mut executor = StackExecutor::new_with_precompiles(state, &Self::CONFIG, &precompiles);
        let access_list = ctx
            .access_list
            .into_iter()
            .map(|x| (x.address, x.storage_keys))
            .collect::<Vec<_>>();

        let (exit_reason, data) = match ctx.to {
            Some(address) => executor.transact_call(
                ctx.caller,
                address,
                ctx.value,
                ctx.data.to_vec(),
                ctx.gas_limit,
                access_list,
            ),
            None => {
                let contract_address =
                    executor.create_address(CreateScheme::Legacy { caller: ctx.caller });
                let (exit_reason, _) = executor.transact_create(
                    ctx.caller,
                    ctx.value,
                    ctx.data.to_vec(),
                    ctx.gas_limit,
                    access_list,
                );
                let code = executor.state().code(contract_address);
                (exit_reason, code)
            }
        };

        TxResponse {
            exit_reason,
            data,
            logs: Vec::new(),
            used_gas: executor.used_gas(),
        }
    }

    /// Update state
    pub fn exec(
        &mut self,
        signed_tx: &SignedTx,
        gas_limit: U256,
        base_fee: U256,
        system_tx: bool,
        block_ctx: Option<&BlockContext>,
    ) -> Result<(TxResponse, ReceiptV3)> {
        self.backend.update_vicinity_from_tx(signed_tx)?;
        trace!(
            "[Executor] Executing EVM TX with vicinity : {:?}",
            self.backend.vicinity
        );
        let ctx = ExecutorContext {
            caller: signed_tx.sender,
            to: signed_tx.to(),
            value: signed_tx.value(),
            data: signed_tx.data(),
            gas_limit: u64::try_from(gas_limit).unwrap_or(u64::MAX), // Sets to u64 if overflows
            access_list: signed_tx.access_list(),
        };

        let prepay_fee = if system_tx {
            U256::zero()
        } else {
            calculate_current_prepay_gas_fee(signed_tx, base_fee)?
        };

        if !system_tx {
            self.backend
                .deduct_prepay_gas_fee(signed_tx.sender, prepay_fee)?;
        }

        let metadata = StackSubstateMetadata::new(ctx.gas_limit, &Self::CONFIG);
        let dvm_block = resolve_dvm_block(
            block_ctx.map(|ctx| ctx.dvm_block),
            self.backend.vicinity.dvm_block,
        );
        let df25_active = is_df25_active(dvm_block, ain_cpp_imports::get_df25_height());
        let state = MemoryStackState::new(metadata, self.backend);
        let precompiles = if let Some(block_ctx) = block_ctx {
            MetachainPrecompiles::new(block_ctx.mnview_ptr).with_df25_active(df25_active)
        } else {
            MetachainPrecompiles::default().with_df25_active(df25_active)
        };
        let mut executor = StackExecutor::new_with_precompiles(state, &Self::CONFIG, &precompiles);
        let access_list = ctx
            .access_list
            .into_iter()
            .map(|x| (x.address, x.storage_keys))
            .collect::<Vec<_>>();

        let (exit_reason, data) = match ctx.to {
            Some(address) => executor.transact_call(
                ctx.caller,
                address,
                ctx.value,
                ctx.data.to_vec(),
                ctx.gas_limit,
                access_list,
            ),
            None => executor.transact_create(
                ctx.caller,
                ctx.value,
                ctx.data.to_vec(),
                ctx.gas_limit,
                access_list,
            ),
        };

        let used_gas = if system_tx { 0u64 } else { executor.used_gas() };
        let total_gas_used = self.backend.vicinity.total_gas_used;
        let block_gas_limit = self.backend.vicinity.block_gas_limit;
        if !system_tx
            && total_gas_used
                .checked_add(U256::from(used_gas))
                .ok_or_else(|| format_err!("total_gas_used overflow"))?
                > block_gas_limit
        {
            return Err(format_err!(
                "[exec] block size limit exceeded, tx cannot make it into the block"
            )
            .into());
        }
        let (values, logs) = executor.into_state().deconstruct();
        let logs = logs.into_iter().collect::<Vec<_>>();

        ApplyBackend::apply(self.backend, values, logs.clone(), true);

        if !system_tx {
            self.backend
                .refund_unused_gas_fee(signed_tx, U256::from(used_gas), base_fee)?;
        }

        let receipt = ReceiptV3::EIP1559(EIP658ReceiptData {
            logs_bloom: {
                let mut bloom: Bloom = Bloom::default();
                EVMCoreService::logs_bloom(logs.clone(), &mut bloom);
                bloom
            },
            status_code: u8::from(exit_reason.is_succeed()),
            logs: logs.clone(),
            used_gas: U256::from(used_gas),
        });

        Ok((
            TxResponse {
                exit_reason,
                data,
                logs,
                used_gas,
            },
            receipt,
        ))
    }
}

impl<'backend> AinExecutor<'backend> {
    /// System tx execution
    pub fn execute_tx(
        &mut self,
        tx: ExecuteTx,
        base_fee: U256,
        ctx: Option<&BlockContext>,
    ) -> Result<ApplyTxResult> {
        // Resolve the DVM block height once for all height-gated rules below.
        // The normal execution path provides the height via the block context,
        // while the RPC tracer replays blocks without one and falls back to the
        // backend vicinity, which holds the height of the replayed block.
        let dvm_block =
            resolve_dvm_block(ctx.map(|c| c.dvm_block), self.backend.vicinity.dvm_block);
        match tx {
            ExecuteTx::SignedTx(signed_tx) => {
                // Validate nonce
                let nonce = self.backend.get_nonce(&signed_tx.sender);
                if nonce != signed_tx.nonce() {
                    return Err(format_err!(
                        "[execute_tx] nonce check failed. Account nonce {}, signed_tx nonce {}",
                        nonce,
                        signed_tx.nonce(),
                    )
                    .into());
                }

                let (tx_response, receipt) =
                    self.exec(&signed_tx, signed_tx.gas_limit(), base_fee, false, ctx)?;

                trace!(
                    "[execute_tx] receipt : {:?}, exit_reason {:#?} for signed_tx : {:#x}",
                    receipt,
                    tx_response.exit_reason,
                    signed_tx.hash()
                );

                let gas_fee =
                    calculate_gas_fee(&signed_tx, U256::from(tx_response.used_gas), base_fee)?;

                Ok(ApplyTxResult {
                    tx: signed_tx,
                    exec_flag: tx_response.exit_reason.is_succeed(),
                    used_gas: U256::from(tx_response.used_gas),
                    logs: tx_response.logs,
                    gas_fee,
                    receipt: (receipt, None),
                })
            }
            ExecuteTx::SystemTx(SystemTx::TransferDomain(TransferDomainData {
                signed_tx,
                direction,
            })) => {
                // Validate nonce
                let nonce = self.backend.get_nonce(&signed_tx.sender);
                if nonce != signed_tx.nonce() {
                    return Err(format_err!(
                        "[execute_tx] nonce check failed. Account nonce {}, signed_tx nonce {}",
                        nonce,
                        signed_tx.nonce(),
                    )
                    .into());
                }

                let to = signed_tx.to().unwrap();
                let input = signed_tx.data();
                let amount = U256::from_big_endian(&input[68..100]);

                trace!(
                    "[execute_tx] Transfer domain: {} from address {:x?}, nonce {:x?}, to address {:x?}, amount: {}",
                    direction, signed_tx.sender, signed_tx.nonce(), to, amount,
                );

                let FixedContract {
                    contract,
                    fixed_address,
                    ..
                } = get_transfer_domain_contract();
                let mismatch = match self.backend.get_account(&fixed_address) {
                    None => false,
                    Some(account) => account.code_hash != contract.codehash,
                };
                if mismatch {
                    trace!(
                        "[execute_tx] {} failed with as transferdomain account codehash mismatch",
                        direction
                    );
                    return Err(format_err!(
                        "[execute_tx] {} failed with as transferdomain account codehash mismatch",
                        direction
                    )
                    .into());
                }

                if direction == TransferDirection::EvmIn {
                    let storage = bridge_dfi(self.backend, amount, direction)?;
                    self.update_storage(fixed_address, storage)?;
                    self.add_balance(fixed_address, amount)?;
                }

                let (tx_response, receipt) =
                    self.exec(&signed_tx, U256::MAX, U256::zero(), true, ctx)?;
                if !tx_response.exit_reason.is_succeed() {
                    return Err(format_err!(
                        "[execute_tx] Transfer domain failed VM execution {:?}",
                        tx_response.exit_reason
                    )
                    .into());
                }

                trace!(
                    "[execute_tx] receipt : {:?}, exit_reason {:#?} for signed_tx : {:#x}, logs: {:x?}",
                    receipt,
                    tx_response.exit_reason,
                    signed_tx.hash(),
                    tx_response.logs
                );

                if direction == TransferDirection::EvmOut {
                    let storage = bridge_dfi(self.backend, amount, direction)?;
                    self.update_storage(fixed_address, storage)?;
                    self.sub_balance(signed_tx.sender, amount)?;
                }

                Ok(ApplyTxResult {
                    tx: signed_tx,
                    exec_flag: true,
                    used_gas: U256::zero(),
                    logs: tx_response.logs,
                    gas_fee: U256::zero(),
                    receipt: (receipt, None),
                })
            }
            ExecuteTx::SystemTx(SystemTx::DST20Bridge(DST20Data {
                signed_tx,
                contract_address,
                direction,
            })) => {
                // Validate nonce
                let nonce = self.backend.get_nonce(&signed_tx.sender);
                if nonce != signed_tx.nonce() {
                    return Err(format_err!(
                        "[execute_tx] nonce check failed. Account nonce {}, signed_tx nonce {}",
                        nonce,
                        signed_tx.nonce(),
                    )
                    .into());
                }

                let input = signed_tx.data();
                let amount = U256::from_big_endian(&input[100..132]);

                trace!(
                    "[execute_tx] DST20Bridge from {}, contract_address {}, amount {}, direction {}",
                    signed_tx.sender, contract_address, amount, direction
                );

                let df25_active = is_df25_active(dvm_block, ain_cpp_imports::get_df25_height());

                if direction == TransferDirection::EvmIn {
                    let DST20BridgeInfo { address, storage } =
                        bridge_dst20_in(self.backend, contract_address, amount, df25_active)?;
                    self.update_storage(address, storage)?;
                }

                let allowance = dst20_allowance(direction, signed_tx.sender, amount);
                self.update_storage(contract_address, allowance)?;

                let (tx_response, receipt) =
                    self.exec(&signed_tx, U256::MAX, U256::zero(), true, ctx)?;
                if !tx_response.exit_reason.is_succeed() {
                    return Err(format_err!(
                        "[execute_tx] DST20 bridge failed VM execution {:?}, data {:?}",
                        tx_response.exit_reason,
                        tx_response.data
                    )
                    .into());
                }

                trace!(
                    "[execute_tx] receipt : {:?}, exit_reason {:#?} for signed_tx : {:#x}, logs: {:x?}",
                    receipt,
                    tx_response.exit_reason,
                    signed_tx.hash(),
                    tx_response.logs
                );

                if direction == TransferDirection::EvmOut {
                    let DST20BridgeInfo { address, storage } =
                        bridge_dst20_out(self.backend, contract_address, amount, df25_active)?;
                    self.update_storage(address, storage)?;
                }

                Ok(ApplyTxResult {
                    tx: signed_tx,
                    exec_flag: true,
                    used_gas: U256::zero(),
                    logs: tx_response.logs,
                    gas_fee: U256::zero(),
                    receipt: (receipt, None),
                })
            }
            ExecuteTx::SystemTx(SystemTx::DeployContract(DeployContractData {
                name,
                symbol,
                address,
                token_id,
            })) => {
                trace!(
                    "[execute_tx] DeployContract for address {:x?}, name {}, symbol {}",
                    address,
                    name,
                    symbol
                );

                let DeployContractInfo {
                    address,
                    bytecode,
                    storage,
                } = dst20_deploy_info(
                    self.backend,
                    dvm_block.unwrap_or(0),
                    address,
                    &name,
                    &symbol,
                )?;

                self.deploy_contract(address, bytecode, storage)?;
                let (tx, receipt) = dst20_deploy_contract_tx(token_id, &base_fee)?;

                Ok(ApplyTxResult {
                    tx,
                    exec_flag: true,
                    used_gas: U256::zero(),
                    logs: Vec::new(),
                    gas_fee: U256::zero(),
                    receipt: (receipt, Some(address)),
                })
            }
            ExecuteTx::SystemTx(SystemTx::UpdateContractName(UpdateContractNameData {
                name,
                symbol,
                address,
                token_id,
            })) => {
                trace!(
                    "[execute_tx] Rename contract for address {:x?}, name {}, symbol {}",
                    address,
                    name,
                    symbol
                );

                let storage = dst20_name_info(dvm_block.unwrap_or(0), &name, &symbol);

                self.update_storage(address, storage)?;
                let (tx, receipt) = rename_contract_tx(token_id, &base_fee)?;

                Ok(ApplyTxResult {
                    tx,
                    used_gas: U256::zero(),
                    logs: Vec::new(),
                    gas_fee: U256::zero(),
                    receipt: (receipt, Some(address)),
                    exec_flag: true,
                })
            }
        }
    }
}

#[derive(Debug)]
pub struct ApplyTxResult {
    pub tx: Box<SignedTx>,
    pub exec_flag: bool,
    pub used_gas: U256,
    pub logs: Vec<Log>,
    pub gas_fee: U256,
    pub receipt: ReceiptAndOptionalContractAddress,
}

#[derive(Debug)]
pub struct TxResponse {
    pub exit_reason: ExitReason,
    pub data: Vec<u8>,
    pub logs: Vec<Log>,
    pub used_gas: u64,
}

#[cfg(test)]
mod tests {
    use ain_contracts::{
        get_dst20_contract, get_dst20_contract_for_height, is_valid_dst20_codehash,
    };
    use ethereum_types::H256;

    use super::{is_df25_active, resolve_dvm_block};

    const DF25_HEIGHT: u64 = 100;

    #[test]
    fn resolve_dvm_block_prefers_the_block_context() {
        assert_eq!(resolve_dvm_block(Some(120), Some(110)), Some(120));
    }

    #[test]
    fn resolve_dvm_block_falls_back_to_the_vicinity() {
        // The RPC tracer replays blocks without a block context and relies on
        // the backend vicinity, which holds the height of the replayed block.
        assert_eq!(resolve_dvm_block(None, Some(110)), Some(110));
    }

    #[test]
    fn resolve_dvm_block_without_any_height() {
        assert_eq!(resolve_dvm_block(None, None), None);
    }

    #[test]
    fn df25_is_inactive_below_the_activation_height() {
        assert!(!is_df25_active(Some(DF25_HEIGHT - 1), DF25_HEIGHT));
    }

    #[test]
    fn df25_is_active_at_and_above_the_activation_height() {
        assert!(is_df25_active(Some(DF25_HEIGHT), DF25_HEIGHT));
        assert!(is_df25_active(Some(DF25_HEIGHT + 1), DF25_HEIGHT));
    }

    #[test]
    fn df25_is_inactive_without_any_height() {
        assert!(!is_df25_active(None, DF25_HEIGHT));
    }

    #[test]
    fn dst20_codehash_gate_follows_the_resolved_height() {
        let legacy_hash = get_dst20_contract().codehash;
        let df25_hash = get_dst20_contract_for_height(true).codehash;
        let foreign_hash = H256::from_low_u64_be(0xdeadbeef);

        // A foreign code hash is never accepted, no matter which height is resolved.
        assert!(!is_valid_dst20_codehash(foreign_hash, false));
        assert!(!is_valid_dst20_codehash(foreign_hash, true));

        // The legacy facade stays valid at any height.
        assert!(is_valid_dst20_codehash(legacy_hash, false));
        assert!(is_valid_dst20_codehash(legacy_hash, true));

        if df25_hash != legacy_hash {
            // Below DF25Height the DF25 facade is not accepted yet ...
            let below = resolve_dvm_block(None, Some(DF25_HEIGHT - 1));
            assert!(!is_df25_active(below, DF25_HEIGHT));
            assert!(!is_valid_dst20_codehash(df25_hash, false));

            // ... at/above DF25Height it is.
            let above = resolve_dvm_block(None, Some(DF25_HEIGHT));
            assert!(is_df25_active(above, DF25_HEIGHT));
            assert!(is_valid_dst20_codehash(df25_hash, true));
        }
    }
}
