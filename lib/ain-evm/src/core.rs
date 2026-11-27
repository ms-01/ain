use std::{
    collections::{BTreeSet, HashMap},
    path::PathBuf,
    sync::Arc,
};

use ain_contracts::{
    dst20_address_from_token_id, get_transfer_domain_contract,
    get_transferdomain_dst20_transfer_function, get_transferdomain_native_transfer_function,
    FixedContract,
};
use anyhow::format_err;
use ethereum::{AccessList, Account, Block, Log, PartialHeader, TransactionAction, TransactionV2};
use ethereum_types::{Bloom, BloomInput, H160, H256, U256};
use log::{info, trace};
use parking_lot::Mutex;

use crate::{
    backend::{BackendError, EVMBackend, Overlay, Vicinity},
    block::INITIAL_BASE_FEE,
    blocktemplate::BlockTemplate,
    executor::{AinExecutor, ExecutorContext, TxResponse},
    fee::calculate_max_prepay_gas_fee,
    gas::check_tx_intrinsic_gas,
    receipt::ReceiptService,
    storage::{traits::BlockStorage, Storage},
    transaction::cache::{TransactionCache, ValidateTxInfo},
    trie::{TrieDBStore, GENESIS_STATE_ROOT},
    weiamount::{try_from_satoshi, WeiAmount},
    Result,
};

pub type XHash = [u8; 32];
pub type XAddress = [u8; 20];

pub struct EVMCoreService {
    pub trie_store: Arc<TrieDBStore>,
    storage: Arc<Storage>,
    pub tx_cache: Arc<TransactionCache>,
    nonce_store: Mutex<HashMap<H160, BTreeSet<U256>>>,
}

pub struct EthCallArgs<'a> {
    pub caller: H160,
    pub to: Option<H160>,
    pub value: U256,
    pub data: &'a [u8],
    pub gas_limit: u64,
    pub gas_price: U256,
    pub access_list: AccessList,
    pub block_number: U256,
}

pub struct TransferDomainTxInfo {
    pub from: XAddress,
    pub to: XAddress,
    pub native_address: String,
    pub direction: bool,
    pub value: u64,
    pub token_id: u32,
}

impl EVMCoreService {
    pub fn restore(
        trie_store: Arc<TrieDBStore>,
        storage: Arc<Storage>,
        tx_cache: Arc<TransactionCache>,
    ) -> Self {
        Self {
            trie_store,
            storage,
            tx_cache,
            nonce_store: Mutex::new(HashMap::new()),
        }
    }

    pub fn new_from_json(
        trie_store: Arc<TrieDBStore>,
        storage: Arc<Storage>,
        tx_cache: Arc<TransactionCache>,
        genesis_path: PathBuf,
    ) -> Result<Self> {
        info!("Loading genesis state from {}", genesis_path.display());

        let handler = Self {
            trie_store: Arc::clone(&trie_store),
            storage: Arc::clone(&storage),
            tx_cache: Arc::clone(&tx_cache),
            nonce_store: Mutex::new(HashMap::new()),
        };
        let (state_root, genesis) = TrieDBStore::genesis_state_root_from_json(
            &handler.trie_store,
            &handler.storage,
            genesis_path,
        )?;

        let gas_limit = ain_cpp_imports::get_attribute_values(None).block_gas_limit;
        let block: Block<TransactionV2> = Block::new(
            PartialHeader {
                state_root,
                number: U256::zero(),
                beneficiary: H160::default(),
                receipts_root: ReceiptService::get_receipts_root(&Vec::new()),
                logs_bloom: Bloom::default(),
                gas_used: U256::default(),
                gas_limit: genesis.gas_limit.unwrap_or(U256::from(gas_limit)),
                extra_data: genesis.extra_data.unwrap_or_default().into(),
                parent_hash: genesis.parent_hash.unwrap_or_default(),
                mix_hash: genesis.mix_hash.unwrap_or_default(),
                nonce: genesis.nonce.unwrap_or_default(),
                timestamp: u64::try_from(genesis.timestamp.unwrap_or_default())?,
                difficulty: genesis.difficulty.unwrap_or_default(),
                base_fee: genesis.base_fee.unwrap_or(INITIAL_BASE_FEE),
            },
            Vec::new(),
            Vec::new(),
        );
        storage.put_latest_block(Some(&block))?;
        storage.put_block(&block)?;

        Ok(handler)
    }

    pub fn flush(&self) -> Result<()> {
        self.trie_store.flush()
    }
}

// EVM Tx validation methods
impl EVMCoreService {
    /// Validates a raw tx.
    ///
    /// The validation checks of the tx before we consider it to be valid are:
    /// 1. Gas price check: verify that the maximum gas price is minimally of the block initial base fee.
    /// 2. Gas price and tx value check: verify that amount is within money range.
    /// 3. Intrinsic gas limit check: verify that the tx intrinsic gas is within the tx gas limit.
    /// 4. Gas limit check: verify that the tx gas limit is not higher than the maximum gas per block.
    /// 5. Account balance check: verify that the account balance must minimally have the tx prepay gas fee.
    /// 6. Account nonce check: verify that the tx nonce must be more than or equal to the account nonce.
    ///
    /// # Arguments
    ///
    /// * `tx` - The raw tx.
    /// * `template` - The EVM BlockTemplate.
    ///
    /// # Returns
    ///
    /// Returns the signed tx, tx prepay gas fees and the gas used to call the tx.
    ///
    /// # Safety
    ///
    /// Result cannot be used safety unless cs_main lock is taken on C++ side
    /// across all usages. Note: To be replaced with a proper lock flow later.
    ///
    pub unsafe fn validate_raw_tx(
        &self,
        tx: &str,
        template: &BlockTemplate,
    ) -> Result<ValidateTxInfo> {
        trace!("[validate_raw_tx] raw transaction : {:#?}", tx);

        let ValidateTxInfo {
            signed_tx,
            max_prepay_fee,
        } = if let Some(validate_info) = self.tx_cache.get_stateless(tx) {
            validate_info
        } else {
            let signed_tx = self
                .tx_cache
                .try_get_or_create(tx)
                .map_err(|_| format_err!("Error: decoding raw tx to TransactionV2"))?;
            trace!("[validate_raw_tx] signed_tx : {:#?}", signed_tx);

            trace!(
                "[validate_raw_tx] signed_tx.sender : {:#?}",
                signed_tx.sender
            );

            // Validate tx gas price with initial block base fee
            let tx_gas_price = signed_tx.gas_price();
            if tx_gas_price < INITIAL_BASE_FEE {
                trace!("[validate_raw_tx] tx gas price is lower than initial block base fee");
                return Err(
                    format_err!("tx gas price is lower than initial block base fee").into(),
                );
            }

            // Validate tx gas price and tx value within money range
            if !WeiAmount(tx_gas_price).wei_range() || !WeiAmount(signed_tx.value()).wei_range() {
                trace!("[validate_raw_tx] value more than money range");
                return Err(format_err!("value more than money range").into());
            }

            // Validate tx gas limit with intrinsic gas
            check_tx_intrinsic_gas(&signed_tx)?;

            let max_prepay_fee = calculate_max_prepay_gas_fee(&signed_tx)?;
            trace!("[validate_raw_tx] max_prepay_fee : {:x?}", max_prepay_fee);

            self.tx_cache.set_stateless(
                String::from(tx),
                ValidateTxInfo {
                    signed_tx,
                    max_prepay_fee,
                },
            )
        };

        // Validate gas limit
        let gas_limit = signed_tx.gas_limit();
        let block_gas_limit = template.ctx.attrs.block_gas_limit;
        if gas_limit > U256::from(block_gas_limit) {
            trace!("[validate_raw_tx] gas limit higher than max_gas_per_block");
            return Err(format_err!("gas limit higher than max_gas_per_block").into());
        }

        // Start of stateful checks
        // Validate tx prepay fees with account balance
        let backend = &template.backend;
        let balance = backend.get_balance(&signed_tx.sender);
        trace!("[validate_raw_tx] Account balance : {:x?}", balance);
        if balance < max_prepay_fee {
            trace!("[validate_raw_tx] insufficient balance to pay fees");
            return Err(format_err!("insufficient balance to pay fees").into());
        }

        let nonce = backend.get_nonce(&signed_tx.sender);
        trace!(
            "[validate_raw_tx] signed_tx nonce : {:#?}",
            signed_tx.nonce()
        );
        trace!("[validate_raw_tx] nonce : {:#?}", nonce);
        // Validate tx nonce with account nonce
        if nonce > signed_tx.nonce() {
            return Err(format_err!(
                "invalid nonce. Account nonce {}, signed_tx nonce {}",
                nonce,
                signed_tx.nonce()
            )
            .into());
        }

        Ok(ValidateTxInfo {
            signed_tx,
            max_prepay_fee,
        })
    }

    /// Validates a raw transfer domain tx.
    ///
    /// The validation checks of the tx before we consider it to be valid are:
    /// 1. Verify that transferdomain tx is of legacy transaction type.
    /// 2. Verify that tx sender matches transferdomain from address.
    /// 3. Verify that tx value is set to zero.
    /// 4. Verify that gas price is set to zero.
    /// 5. Verify that gas limit is set to zero.
    /// 6. Verify that transaction action is a call to the transferdomain contract address.
    ///
    /// 7. If native token transfer:
    /// - Verify that tx function signature is transfer.
    /// - Verify that from address specified matches transferdomain tx information
    /// - Verify that to address specified matches transferdomain tx information
    /// - Verify that value specified matches transferdomain tx information
    /// - Verify that native address specified matches transferdomain tx information
    ///
    /// 8. If DST20 token transfer:
    /// - Verify that tx function signature is transferDST20.
    /// - Verify that contract address specified matches DST20 token ID address.
    /// - Verify that from address specified matches transferdomain tx information
    /// - Verify that to address specified matches transferdomain tx information
    /// - Verify that value specified matches transferdomain tx information
    /// - Verify that native address specified matches transferdomain tx information
    ///
    /// 9. Account nonce check: verify that the tx nonce must be more than or equal to the account nonce.
    ///
    /// # Arguments
    ///
    /// * `tx` - The raw tx.
    /// * `template` - The EVM BlockTemplate.
    ///
    /// # Returns
    ///
    /// Returns the validation result.
    ///
    /// # Safety
    ///
    /// Result cannot be used safety unless cs_main lock is taken on C++ side
    /// across all usages. Note: To be replaced with a proper lock flow later.
    ///
    pub unsafe fn validate_raw_transferdomain_tx(
        &self,
        tx: &str,
        template: &BlockTemplate,
        context: TransferDomainTxInfo,
    ) -> Result<ValidateTxInfo> {
        trace!(
            "[validate_raw_transferdomain_tx] raw transaction : {:#?}",
            tx
        );

        let ValidateTxInfo {
            signed_tx,
            max_prepay_fee,
        } = if let Some(validate_info) = self.tx_cache.get_stateless(tx) {
            validate_info
        } else {
            let signed_tx = self
                .tx_cache
                .try_get_or_create(tx)
                .map_err(|_| format_err!("Error: decoding raw tx to TransactionV2"))?;
            trace!(
                "[validate_raw_transferdomain_tx] signed_tx : {:#?}",
                signed_tx
            );

            // Validate tx is legacy tx
            if signed_tx.get_tx_type() != U256::zero() {
                return Err(format_err!(
                    "[validate_raw_transferdomain_tx] invalid evm tx type {:#?}",
                    signed_tx.get_tx_type()
                )
                .into());
            }

            // Validate tx sender with transferdomain sender
            let sender = H160::from(context.from);
            if signed_tx.sender != sender {
                return Err(format_err!(
                    "[validate_raw_transferdomain_tx] invalid sender, signed_tx.sender : {:#?}, transferdomain sender : {:#?}",
                    signed_tx.sender,
                    sender
                ).into());
            }

            // Validate tx value equal to zero
            if signed_tx.value() != U256::zero() {
                trace!("[validate_raw_transferdomain_tx] value not equal to zero");
                return Err(format_err!("value not equal to zero").into());
            }

            // Validate tx gas price equal to zero
            if signed_tx.gas_price() != U256::zero() {
                trace!("[validate_raw_transferdomain_tx] gas price not equal to zero");
                return Err(format_err!("gas price not equal to zero").into());
            }

            // Validate tx gas limit equal to zero
            if signed_tx.gas_limit() != U256::zero() {
                trace!("[validate_raw_transferdomain_tx] gas limit not equal to zero");
                return Err(format_err!("gas limit not equal to zero").into());
            }

            // Verify transaction action and transferdomain contract address
            let FixedContract { fixed_address, .. } = get_transfer_domain_contract();
            match signed_tx.action() {
                TransactionAction::Call(address) => {
                    if address != fixed_address {
                        return Err(format_err!(
                            "invalid call address. Fixed address: {:#?}, signed_tx call address: {:#?}",
                            fixed_address,
                            address
                    )
                        .into());
                    }
                }
                _ => {
                    return Err(format_err!(
                        "tx action not a call to transferdomain contract address"
                    )
                    .into())
                }
            }

            let (from_address, to_address) = if context.direction {
                // EvmIn
                let to_address = H160::from(context.to);
                (fixed_address, to_address)
            } else {
                // EvmOut
                let from_address = H160::from(context.from);
                (from_address, fixed_address)
            };
            let value = try_from_satoshi(U256::from(context.value))?.0;

            let is_native_token_transfer = context.token_id == 0;
            if is_native_token_transfer {
                // Validate function signature
                let function = get_transferdomain_native_transfer_function();
                let function_signature = function.short_signature();
                if function_signature != signed_tx.data()[..4] {
                    return Err(format_err!("invalid function signature input in evm tx").into());
                }

                let token_inputs = function.decode_input(&signed_tx.data()[4..])?;

                // Validate from address input
                let ethabi::Token::Address(input_from_address) = token_inputs[0] else {
                    return Err(format_err!("invalid from address input in evm tx").into());
                };
                if input_from_address != from_address {
                    return Err(format_err!("invalid from address input in evm tx").into());
                }

                // Validate to address input
                let ethabi::Token::Address(input_to_address) = token_inputs[1] else {
                    return Err(format_err!("invalid to address input in evm tx").into());
                };
                if input_to_address != to_address {
                    return Err(format_err!("invalid to address input in evm tx").into());
                }

                // Validate value input
                let ethabi::Token::Uint(input_value) = token_inputs[2] else {
                    return Err(format_err!("invalid value input in evm tx").into());
                };
                if input_value != value {
                    return Err(format_err!("invalid value input in evm tx").into());
                }

                // Validate native address input
                let ethabi::Token::String(ref input_native_address) = token_inputs[3] else {
                    return Err(format_err!("invalid native address input in evm tx").into());
                };
                if context.native_address != *input_native_address {
                    return Err(format_err!("invalid native address input in evm tx").into());
                }
            } else {
                let contract_address = {
                    let address = dst20_address_from_token_id(u64::from(context.token_id))?;
                    ethabi::Token::Address(address)
                };

                // Validate function signature
                let function = get_transferdomain_dst20_transfer_function();
                let function_signature = function.short_signature();
                if function_signature != signed_tx.data()[..4] {
                    return Err(format_err!("invalid function signature input in evm tx").into());
                }

                let token_inputs = function.decode_input(&signed_tx.data()[4..])?;

                // Validate contract address input
                if token_inputs[0] != contract_address {
                    return Err(format_err!("invalid contract address input in evm tx").into());
                }

                // Validate from address input
                let ethabi::Token::Address(input_from_address) = token_inputs[1] else {
                    return Err(format_err!("invalid from address input in evm tx").into());
                };
                if input_from_address != from_address {
                    return Err(format_err!("invalid from address input in evm tx").into());
                }

                // Validate to address input
                let ethabi::Token::Address(input_to_address) = token_inputs[2] else {
                    return Err(format_err!("invalid to address input in evm tx").into());
                };
                if input_to_address != to_address {
                    return Err(format_err!("invalid to address input in evm tx").into());
                }

                // Validate value input
                let ethabi::Token::Uint(input_value) = token_inputs[3] else {
                    return Err(format_err!("invalid value input in evm tx").into());
                };
                if input_value != value {
                    return Err(format_err!("invalid value input in evm tx").into());
                }

                // Validate native address input
                let ethabi::Token::String(ref input_native_address) = token_inputs[4] else {
                    return Err(format_err!("invalid native address input in evm tx").into());
                };
                if context.native_address != *input_native_address {
                    return Err(format_err!("invalid native address input in evm tx").into());
                }
            }

            ValidateTxInfo {
                signed_tx,
                max_prepay_fee: U256::zero(),
            }
        };

        let backend = &template.backend;

        let nonce = backend.get_nonce(&signed_tx.sender);
        trace!(
            "[validate_raw_transferdomain_tx] signed_tx.sender : {:#?}",
            signed_tx.sender
        );
        trace!(
            "[validate_raw_transferdomain_tx] signed_tx nonce : {:#?}",
            signed_tx.nonce()
        );
        trace!("[validate_raw_transferdomain_tx] nonce : {:#?}", nonce);

        // Validate tx nonce
        if nonce > signed_tx.nonce() {
            return Err(format_err!(
                "Invalid nonce. Account nonce {}, signed_tx nonce {}",
                nonce,
                signed_tx.nonce()
            )
            .into());
        }

        Ok(ValidateTxInfo {
            signed_tx,
            max_prepay_fee,
        })
    }

    pub fn logs_bloom(logs: Vec<Log>, bloom: &mut Bloom) {
        for log in logs {
            bloom.accrue(BloomInput::Raw(&log.address[..]));
            for topic in log.topics {
                bloom.accrue(BloomInput::Raw(&topic[..]));
            }
        }
    }
}

// Block template methods
impl EVMCoreService {
    ///
    /// # Safety
    ///
    /// Result cannot be used safety unless `cs_main` lock is taken on C++ side
    /// across all usages. Note: To be replaced with a proper lock flow later.
    ///
    pub unsafe fn remove_txs_above_hash_in_block_template(
        &self,
        template: &mut BlockTemplate,
        target_hash: XHash,
    ) -> Result<Vec<XHash>> {
        let hashes = template.remove_txs_above_hash(target_hash)?;
        Ok(hashes)
    }

    /// Retrieves the next valid nonce for the specified account within a particular block template.
    /// # Arguments
    ///
    /// * `template` - The EVM BlockTemplate.
    /// * `address` - The EVM address of the account whose nonce we want to retrieve.
    ///
    /// # Returns
    ///
    /// Returns the next valid nonce as a `U256`. Defaults to U256::zero()
    ///
    /// # Safety
    ///
    /// Result cannot be used safety unless cs_main lock is taken on C++ side
    /// across all usages. Note: To be replaced with a proper lock flow later.
    ///
    pub unsafe fn get_next_valid_nonce_in_block_template(
        &self,
        template: &BlockTemplate,
        address: H160,
    ) -> Result<U256> {
        let nonce = template.backend.get_nonce(&address);
        trace!("[get_next_valid_nonce_in_block_template] Account {address:x?} nonce {nonce:x?}");
        Ok(nonce)
    }
}

// State methods
impl EVMCoreService {
    pub fn get_latest_state_root(&self) -> Result<H256> {
        let state_root = self
            .storage
            .get_latest_block()?
            .map_or(H256::default(), |block| block.header.state_root);
        Ok(state_root)
    }

    pub fn get_account(&self, address: H160, state_root: H256) -> Result<Option<Account>> {
        let backend = EVMBackend::from_root(
            state_root,
            Arc::clone(&self.trie_store),
            Arc::clone(&self.storage),
            Vicinity::default(),
            None,
        )?;
        Ok(backend.get_account(&address))
    }

    pub fn get_code(&self, address: H160, state_root: H256) -> Result<Option<Vec<u8>>> {
        self.get_account(address, state_root)?
            .map_or(Ok(None), |account| {
                self.storage.get_code_by_hash(address, account.code_hash)
            })
    }

    pub fn get_storage_at(
        &self,
        address: H160,
        position: U256,
        state_root: H256,
    ) -> Result<Option<Vec<u8>>> {
        self.get_account(address, state_root)?
            .map_or(Ok(None), |account| {
                let storage_trie = self
                    .trie_store
                    .trie_db
                    .trie_restore(address.as_bytes(), None, account.storage_root.into())
                    .map_err(|e| BackendError::TrieRestoreFailed(e.to_string()).into());

                let tmp: &mut [u8; 32] = &mut [0; 32];
                position.to_big_endian(tmp);
                storage_trie.and_then(|storage| {
                    storage
                        .get(tmp.as_slice())
                        .map_err(|e| BackendError::TrieError(e.to_string()).into())
                })
            })
    }

    pub fn get_balance(&self, address: H160, state_root: H256) -> Result<U256> {
        let balance = self
            .get_account(address, state_root)?
            .map_or(U256::zero(), |account| account.balance);

        trace!("Account {:x?} balance {:x?}", address, balance);
        Ok(balance)
    }

    pub fn get_nonce(&self, address: H160, state_root: H256) -> Result<U256> {
        let backend = self.get_backend(state_root)?;
        let nonce = backend.get_nonce(&address);
        Ok(nonce)
    }

    pub fn get_latest_block_backend(&self) -> Result<EVMBackend> {
        let block_header = self
            .storage
            .get_latest_block()?
            .map(|block| block.header)
            .ok_or(format_err!(
                "[get_latest_block_backend] Latest block not found",
            ))?;
        let state_root = block_header.state_root;
        trace!(
            "[get_latest_block_backend] At block number : {:#x}, state_root : {:#x}",
            block_header.number,
            state_root,
        );

        let vicinity = Vicinity::from(block_header);
        EVMBackend::from_root(
            state_root,
            Arc::clone(&self.trie_store),
            Arc::clone(&self.storage),
            vicinity,
            None,
        )
    }

    // Note that backend instance returned is only suitable for getting state information,
    // and unsuitable for EVM execution.
    pub fn get_backend(&self, state_root: H256) -> Result<EVMBackend> {
        trace!("[get_backend] state_root : {:#x}", state_root);
        EVMBackend::from_root(
            state_root,
            Arc::clone(&self.trie_store),
            Arc::clone(&self.storage),
            Vicinity {
                block_gas_limit: U256::from(
                    ain_cpp_imports::get_attribute_values(None).block_gas_limit,
                ),
                ..Vicinity::default()
            },
            None,
        )
    }

    pub fn get_backend_from_block(
        &self,
        block_number: Option<U256>,
        caller: Option<H160>,
        gas_price: Option<U256>,
        overlay: Option<Overlay>,
    ) -> Result<EVMBackend> {
        let (state_root, vicinity) = if let Some(block_number) = block_number {
            let block_header = self
                .storage
                .get_block_by_number(&block_number)?
                .map(|block| block.header)
                .ok_or(format_err!("Block number {:x?} not found", block_number))?;
            let state_root = block_header.state_root;
            trace!(
                "Calling EVM at block number : {:#x}, state_root : {:#x}",
                block_number,
                state_root
            );

            let mut vicinity = Vicinity::from(block_header);
            if let Some(gas_price) = gas_price {
                vicinity.gas_price = gas_price;
            }
            if let Some(caller) = caller {
                vicinity.origin = caller;
            }
            trace!("Vicinity: {:?}", vicinity);
            (state_root, vicinity)
        } else {
            // Handle edge case of no genesis block
            let block_gas_limit =
                U256::from(ain_cpp_imports::get_attribute_values(None).block_gas_limit);
            let vicinity: Vicinity = Vicinity {
                block_number: U256::zero(),
                block_gas_limit,
                block_base_fee_per_gas: INITIAL_BASE_FEE,
                ..Vicinity::default()
            };
            (GENESIS_STATE_ROOT, vicinity)
        };
        EVMBackend::from_root(
            state_root,
            Arc::clone(&self.trie_store),
            Arc::clone(&self.storage),
            vicinity,
            overlay,
        )
    }

    pub fn get_next_account_nonce(&self, address: H160, state_root: H256) -> Result<U256> {
        let state_root_nonce = self.get_nonce(address, state_root)?;
        let mut nonce_store = self.nonce_store.lock();
        match nonce_store.entry(address) {
            std::collections::hash_map::Entry::Vacant(_) => Ok(state_root_nonce),
            std::collections::hash_map::Entry::Occupied(e) => {
                let nonce_set = e.get();
                if !nonce_set.contains(&state_root_nonce) {
                    return Ok(state_root_nonce);
                }

                let mut nonce = state_root_nonce;
                for elem in nonce_set.range(state_root_nonce..) {
                    if elem
                        .checked_sub(nonce)
                        .ok_or_else(|| format_err!("elem underflow"))?
                        > U256::one()
                    {
                        break;
                    } else {
                        nonce = *elem;
                    }
                }
                nonce = nonce
                    .checked_add(U256::one())
                    .ok_or_else(|| format_err!("nonce overflow"))?;
                Ok(nonce)
            }
        }
    }

    pub fn store_account_nonce(&self, address: H160, nonce: U256) -> bool {
        let mut nonce_store = self.nonce_store.lock();
        nonce_store.entry(address).or_default();

        match nonce_store.entry(address) {
            std::collections::hash_map::Entry::Occupied(mut e) => {
                e.get_mut().insert(nonce);
                true
            }
            _ => false,
        }
    }

    pub fn clear_account_nonce(&self) {
        let mut nonce_store = self.nonce_store.lock();
        nonce_store.clear()
    }
}

// RPC methods
impl EVMCoreService {
    pub fn call(&self, arguments: EthCallArgs, overlay: Option<Overlay>) -> Result<TxResponse> {
        let EthCallArgs {
            caller,
            to,
            value,
            data,
            gas_limit,
            gas_price,
            access_list,
            block_number,
        } = arguments;
        let mut backend = self
            .get_backend_from_block(Some(block_number), Some(caller), Some(gas_price), overlay)
            .map_err(|e| format_err!("Could not restore backend {}", e))?;
        Ok(AinExecutor::new(&mut backend).call(ExecutorContext {
            caller,
            to,
            value,
            data,
            gas_limit,
            access_list,
        }))
    }
}

#[cfg(test)]
mod tests {
    use ethereum::Log;
    use ethereum_types::{Bloom, H160, H256};

    use crate::core::EVMCoreService;

    /// The exact bloom of a single ERC-20 `Transfer(address,address,uint256)` log
    /// emitted by the contract `0x1111...1111`, i.e. the address and the topic0
    /// `0xddf252ad...3b3ef` accrued into an empty bloom.
    ///
    /// Recomputed with `ethbloom` 0.13.0 (the crate backing the in-tree
    /// `ethereum_types::Bloom`).
    const BLOOM_TRANSFER: &str = "0x00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000008000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000100000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000";

    /// The exact bloom of two logs emitted by the same contract: the
    /// `Transfer(address,address,uint256)` log and a second log carrying the
    /// additional topic `0x...02`.
    const BLOOM_TRANSFER_AND_SECOND_LOG: &str = "0x04000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000008000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000100000000000000000000000000000000000000000000000000000000000000000000000000000000100000100000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000008000000000000000000000";

    fn transfer_log() -> Log {
        Log {
            address: H160::from_slice(
                &hex::decode("1111111111111111111111111111111111111111").unwrap(),
            ),
            topics: vec![H256::from_slice(
                &hex::decode("ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef")
                    .unwrap(),
            )],
            data: vec![],
        }
    }

    fn second_log() -> Log {
        Log {
            address: H160::from_slice(
                &hex::decode("1111111111111111111111111111111111111111").unwrap(),
            ),
            topics: vec![H256::from_slice(
                &hex::decode("0000000000000000000000000000000000000000000000000000000000000002")
                    .unwrap(),
            )],
            data: vec![],
        }
    }

    #[test]
    fn logs_bloom_of_an_empty_log_set_is_zero() {
        let mut bloom = Bloom::default();
        EVMCoreService::logs_bloom(vec![], &mut bloom);
        assert_eq!(bloom, Bloom::default());
        assert_eq!(format!("{:#x}", bloom), format!("0x{}", "0".repeat(512)));
    }

    #[test]
    fn logs_bloom_accrues_the_address_and_every_topic() {
        let mut bloom = Bloom::default();
        EVMCoreService::logs_bloom(vec![transfer_log()], &mut bloom);
        assert_eq!(format!("{:#x}", bloom), BLOOM_TRANSFER);
        assert_eq!(
            hex::encode(bloom.data()),
            BLOOM_TRANSFER.trim_start_matches("0x")
        );
    }

    #[test]
    fn logs_bloom_accumulates_over_multiple_logs() {
        let mut bloom = Bloom::default();
        EVMCoreService::logs_bloom(vec![transfer_log(), second_log()], &mut bloom);
        assert_eq!(format!("{:#x}", bloom), BLOOM_TRANSFER_AND_SECOND_LOG);
        assert_eq!(
            hex::encode(bloom.data()),
            BLOOM_TRANSFER_AND_SECOND_LOG.trim_start_matches("0x")
        );
    }

    mod validation_tests {
        use std::{
            path::PathBuf,
            sync::{
                atomic::{AtomicU64, Ordering},
                Arc,
            },
        };

        use ain_contracts::{
            dst20_address_from_token_id, get_transfer_domain_contract,
            get_transferdomain_dst20_transfer_function,
            get_transferdomain_native_transfer_function,
        };
        use ethereum::{
            EIP1559Transaction, EnvelopedEncodable, LegacyTransaction, TransactionAction,
            TransactionSignature, TransactionV2,
        };
        use ethereum_types::{H160, U256};
        use evm::backend::Basic;

        use crate::{
            backend::{EVMBackend, Vicinity},
            block::INITIAL_BASE_FEE,
            blocktemplate::BlockTemplate,
            core::{EVMCoreService, TransferDomainTxInfo},
            evm::BlockContext,
            storage::Storage,
            transaction::{
                cache::{SignedTxCache, TransactionCache, TxValidationCache},
                SignedTx, LOWER_H256,
            },
            trie::GENESIS_STATE_ROOT,
            weiamount::try_from_satoshi,
        };

        static COUNTER: AtomicU64 = AtomicU64::new(0);

        fn make_core_and_template(
            block_gas_limit: u64,
        ) -> (
            parking_lot::MutexGuard<'static, ()>,
            EVMCoreService,
            BlockTemplate,
            PathBuf,
        ) {
            let guard = crate::evm_unit_tests::vsdb_lock();
            let trie_store = crate::evm_unit_tests::test_trie_store();
            let id = COUNTER.fetch_add(1, Ordering::SeqCst);
            let dir = std::env::temp_dir().join(format!(
                "ain_evm_core_tests_storage_{}_{}",
                std::process::id(),
                id
            ));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            let storage = Arc::new(Storage::new(&dir).unwrap());
            let tx_cache = Arc::new(TransactionCache {
                signed_tx_cache: SignedTxCache::new(64),
                tx_validation_cache: TxValidationCache::new(64),
            });
            let core = EVMCoreService::restore(
                Arc::clone(&trie_store),
                Arc::clone(&storage),
                Arc::clone(&tx_cache),
            );
            let backend = EVMBackend::from_root(
                GENESIS_STATE_ROOT,
                trie_store,
                storage,
                Vicinity {
                    block_gas_limit: U256::from(block_gas_limit),
                    ..Vicinity::default()
                },
                None,
            )
            .unwrap();
            let template = BlockTemplate::new(
                Vicinity::default(),
                BlockContext::new_for_test(1, block_gas_limit),
                backend,
            );
            (guard, core, template, dir)
        }

        fn encode_legacy_tx(
            nonce: u64,
            gas_price: U256,
            gas_limit: u64,
            action: TransactionAction,
            value: U256,
            input: Vec<u8>,
        ) -> (String, SignedTx) {
            let tx = TransactionV2::Legacy(LegacyTransaction {
                nonce: U256::from(nonce),
                gas_price,
                gas_limit: U256::from(gas_limit),
                action,
                value,
                input,
                signature: TransactionSignature::new(27, LOWER_H256, LOWER_H256).unwrap(),
            });
            let raw = hex::encode(tx.encode());
            let signed = SignedTx::try_from(tx).unwrap();
            (raw, signed)
        }

        #[test]
        fn validate_raw_tx_covers_all_checks_and_cache_hit() {
            let (_guard, core, mut template, dir) = make_core_and_template(30_000_000);
            let to = TransactionAction::Call(H160::from_low_u64_be(0x1234));

            // 1. Invalid hex / RLP
            assert!(unsafe { core.validate_raw_tx("not_valid_hex", &template) }.is_err());

            // 2. Gas price below INITIAL_BASE_FEE
            let (low_gp, _) = encode_legacy_tx(0, INITIAL_BASE_FEE - 1, 21_000, to, U256::zero(), vec![]);
            assert!(unsafe { core.validate_raw_tx(&low_gp, &template) }.is_err());

            // 3. Value out of money range
            let (huge_val, _) = encode_legacy_tx(0, INITIAL_BASE_FEE, 21_000, to, U256::MAX, vec![]);
            assert!(unsafe { core.validate_raw_tx(&huge_val, &template) }.is_err());

            // 4. Intrinsic gas too low
            let (low_gas, _) = encode_legacy_tx(0, INITIAL_BASE_FEE, 100, to, U256::zero(), vec![]);
            assert!(unsafe { core.validate_raw_tx(&low_gas, &template) }.is_err());

            // 5. Gas limit > block_gas_limit
            let (high_gas, _) = encode_legacy_tx(0, INITIAL_BASE_FEE, 40_000_000, to, U256::zero(), vec![]);
            assert!(unsafe { core.validate_raw_tx(&high_gas, &template) }.is_err());

            // 6. Insufficient balance
            let (valid_raw, signed) = encode_legacy_tx(0, INITIAL_BASE_FEE, 21_000, to, U256::zero(), vec![]);
            assert!(unsafe { core.validate_raw_tx(&valid_raw, &template) }.is_err());

            // Fund sender and set nonce=1 so nonce=0 fails
            template
                .backend
                .apply(
                    signed.sender,
                    Some(Basic {
                        balance: INITIAL_BASE_FEE * U256::from(100_000u64),
                        nonce: U256::from(1),
                    }),
                    None,
                    Vec::new(),
                    false,
                )
                .unwrap();
            assert!(unsafe { core.validate_raw_tx(&valid_raw, &template) }.is_err());

            // Valid tx with nonce=1 succeeds (and second call hits stateless cache)
            let (valid_raw_1, signed_1) = encode_legacy_tx(1, INITIAL_BASE_FEE, 21_000, to, U256::zero(), vec![]);
            template
                .backend
                .apply(
                    signed_1.sender,
                    Some(Basic {
                        balance: INITIAL_BASE_FEE * U256::from(100_000u64),
                        nonce: U256::from(1),
                    }),
                    None,
                    Vec::new(),
                    false,
                )
                .unwrap();
            let info1 = unsafe { core.validate_raw_tx(&valid_raw_1, &template) }.unwrap();
            let info2 = unsafe { core.validate_raw_tx(&valid_raw_1, &template) }.unwrap();
            assert_eq!(info1.max_prepay_fee, info2.max_prepay_fee);

            // Template helper methods
            assert_eq!(
                unsafe { core.get_next_valid_nonce_in_block_template(&template, signed.sender) }.unwrap(),
                U256::from(1)
            );
            assert!(unsafe { core.remove_txs_above_hash_in_block_template(&mut template, [0u8; 32]) }.unwrap().is_empty());

            let _ = std::fs::remove_dir_all(&dir);
        }

        #[test]
        fn validate_raw_transferdomain_tx_native_and_dst20() {
            let (_guard, core, mut template, dir) = make_core_and_template(30_000_000);
            let td_addr = get_transfer_domain_contract().fixed_address;
            let to_addr = H160::from_low_u64_be(0xabcd);
            let amount_sat: u64 = 100_000_000;
            let amount_wei = try_from_satoshi(U256::from(amount_sat)).unwrap().0;

            // Build a valid native EvmIn transferdomain tx
            let native_fn = get_transferdomain_native_transfer_function();
            let native_input = native_fn
                .encode_input(&[
                    ethabi::Token::Address(td_addr),
                    ethabi::Token::Address(to_addr),
                    ethabi::Token::Uint(amount_wei),
                    ethabi::Token::String("df1qtest".into()),
                ])
                .unwrap();

            let (raw_native, signed_native) = encode_legacy_tx(
                0,
                U256::zero(),
                0,
                TransactionAction::Call(td_addr),
                U256::zero(),
                native_input.clone(),
            );
            let sender = signed_native.sender;

            let ctx_native = || TransferDomainTxInfo {
                from: sender.0,
                to: to_addr.0,
                native_address: "df1qtest".into(),
                direction: true,
                value: amount_sat,
                token_id: 0,
            };

            // Valid native EvmIn
            let ok = unsafe { core.validate_raw_transferdomain_tx(&raw_native, &template, ctx_native()) };
            assert!(ok.is_ok());

            // Non-legacy tx type fails
            let eip1559 = TransactionV2::EIP1559(EIP1559Transaction {
                chain_id: 1,
                nonce: U256::zero(),
                max_priority_fee_per_gas: U256::zero(),
                max_fee_per_gas: U256::zero(),
                gas_limit: U256::zero(),
                action: TransactionAction::Call(td_addr),
                value: U256::zero(),
                input: native_input.clone(),
                access_list: Vec::new(),
                odd_y_parity: false,
                r: LOWER_H256,
                s: LOWER_H256,
            });
            let raw_1559 = hex::encode(eip1559.encode());
            assert!(unsafe { core.validate_raw_transferdomain_tx(&raw_1559, &template, ctx_native()) }.is_err());

            // Wrong sender
            let mut wrong_sender = ctx_native();
            wrong_sender.from = [0x11; 20];
            assert!(unsafe { core.validate_raw_transferdomain_tx(&raw_native, &template, wrong_sender) }.is_err());

            // Non-zero value / gas_price / gas_limit / wrong address / Create action
            let (nz_val, _) = encode_legacy_tx(0, U256::zero(), 0, TransactionAction::Call(td_addr), U256::from(1), native_input.clone());
            assert!(unsafe { core.validate_raw_transferdomain_tx(&nz_val, &template, ctx_native()) }.is_err());

            let (nz_gp, _) = encode_legacy_tx(0, U256::from(1), 0, TransactionAction::Call(td_addr), U256::zero(), native_input.clone());
            assert!(unsafe { core.validate_raw_transferdomain_tx(&nz_gp, &template, ctx_native()) }.is_err());

            let (nz_gl, _) = encode_legacy_tx(0, U256::zero(), 1, TransactionAction::Call(td_addr), U256::zero(), native_input.clone());
            assert!(unsafe { core.validate_raw_transferdomain_tx(&nz_gl, &template, ctx_native()) }.is_err());

            let (wrong_addr, _) = encode_legacy_tx(0, U256::zero(), 0, TransactionAction::Call(to_addr), U256::zero(), native_input.clone());
            assert!(unsafe { core.validate_raw_transferdomain_tx(&wrong_addr, &template, ctx_native()) }.is_err());

            let (create_tx, _) = encode_legacy_tx(0, U256::zero(), 0, TransactionAction::Create, U256::zero(), native_input);
            assert!(unsafe { core.validate_raw_transferdomain_tx(&create_tx, &template, ctx_native()) }.is_err());

            // Valid DST20 EvmIn
            let token_id: u32 = 5;
            let dst20_addr = dst20_address_from_token_id(u64::from(token_id)).unwrap();
            let dst20_fn = get_transferdomain_dst20_transfer_function();
            let dst20_input = dst20_fn
                .encode_input(&[
                    ethabi::Token::Address(dst20_addr),
                    ethabi::Token::Address(td_addr),
                    ethabi::Token::Address(to_addr),
                    ethabi::Token::Uint(amount_wei),
                    ethabi::Token::String("df1qin".into()),
                ])
                .unwrap();
            let (raw_dst20, signed_dst20) = encode_legacy_tx(
                0,
                U256::zero(),
                0,
                TransactionAction::Call(td_addr),
                U256::zero(),
                dst20_input,
            );
            let ctx_dst20 = TransferDomainTxInfo {
                from: signed_dst20.sender.0,
                to: to_addr.0,
                native_address: "df1qin".into(),
                direction: true,
                value: amount_sat,
                token_id,
            };
            assert!(unsafe { core.validate_raw_transferdomain_tx(&raw_dst20, &template, ctx_dst20) }.is_ok());

            // Nonce mismatch fails
            template
                .backend
                .apply(
                    sender,
                    Some(Basic {
                        balance: U256::zero(),
                        nonce: U256::from(2),
                    }),
                    None,
                    Vec::new(),
                    false,
                )
                .unwrap();
            assert!(unsafe { core.validate_raw_transferdomain_tx(&raw_native, &template, ctx_native()) }.is_err());

            let _ = std::fs::remove_dir_all(&dir);
        }
    }
}
