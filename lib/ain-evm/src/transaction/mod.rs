pub mod cache;
pub mod system;

#[cfg(test)]
pub(crate) mod test_helpers;

use anyhow::format_err;
use ethereum::{
    AccessList, EnvelopedDecoderError, EnvelopedEncodable, LegacyTransaction, TransactionAction,
    TransactionSignature, TransactionV2,
};
use ethereum_types::{H160, H256, U256};
use rlp::RlpStream;
use secp256k1::{ecdsa::RecoverableSignature, Message, SecretKey};
use sha3::Digest;

use crate::{
    ecrecover::{public_key_to_address, recover_public_key},
    EVMError,
};

// Lowest acceptable value for r and s in sig.
pub const LOWER_H256: H256 = H256([
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
]);

#[derive(Clone, Debug)]
pub struct LegacyUnsignedTransaction {
    pub nonce: U256,
    pub gas_price: U256,
    pub gas_limit: U256,
    pub action: TransactionAction,
    pub value: U256,
    pub input: Vec<u8>,
    pub sig: TransactionSignature,
}

impl LegacyUnsignedTransaction {
    fn signing_rlp_append(&self, s: &mut RlpStream, chain_id: u64) {
        s.begin_list(9);
        s.append(&self.nonce);
        s.append(&self.gas_price);
        s.append(&self.gas_limit);
        s.append(&self.action);
        s.append(&self.value);
        s.append(&self.input);
        s.append(&chain_id);
        s.append(&0u8);
        s.append(&0u8);
    }

    fn signing_hash(&self, chain_id: u64) -> H256 {
        let mut stream = RlpStream::new();
        self.signing_rlp_append(&mut stream, chain_id);
        let mut output = [0u8; 32];
        output.copy_from_slice(sha3::Keccak256::digest(&stream.out()).as_slice());
        H256::from(output)
    }

    pub fn sign(
        &self,
        key: &[u8; 32],
        chain_id: u64,
    ) -> Result<LegacyTransaction, TransactionError> {
        self.sign_with_chain_id(key, chain_id)
    }

    pub fn sign_with_chain_id(
        &self,
        key: &[u8; 32],
        chain_id: u64,
    ) -> Result<LegacyTransaction, TransactionError> {
        let hash = self.signing_hash(chain_id);
        let msg = Message::from_digest(*hash.as_fixed_bytes());
        let sk = SecretKey::from_secret_bytes(*key)?;
        let signature = RecoverableSignature::sign_ecdsa_recoverable(msg, &sk);
        let (recovery_id, sig) = signature.serialize_compact();

        let sig = TransactionSignature::new(
            u64::from(recovery_id.to_u8() % 2) + chain_id * 2 + 35,
            H256::from_slice(&sig[0..32]),
            H256::from_slice(&sig[32..64]),
        )
        .ok_or(TransactionError::SignatureError)?;

        Ok(LegacyTransaction {
            nonce: self.nonce,
            gas_price: self.gas_price,
            gas_limit: self.gas_limit,
            action: self.action,
            value: self.value,
            input: self.input.clone(),
            signature: sig,
        })
    }
}

impl From<&LegacyTransaction> for LegacyUnsignedTransaction {
    fn from(src: &LegacyTransaction) -> LegacyUnsignedTransaction {
        LegacyUnsignedTransaction {
            nonce: src.nonce,
            gas_price: src.gas_price,
            gas_limit: src.gas_limit,
            action: src.action,
            value: src.value,
            input: src.input.clone(),
            sig: src.signature.clone(),
        }
    }
}

#[derive(Clone, PartialEq, Eq)]
pub struct SignedTx {
    pub transaction: TransactionV2,
    pub sender: H160,
    hash_cache: Cell<Option<H256>>,
}

impl fmt::Debug for SignedTx {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SignedTx")
            .field("hash", &self.hash())
            .field("nonce", &self.nonce())
            .field("to", &self.to())
            .field("action", &self.action())
            .field("value", &self.value())
            .field("gas_price", &self.gas_price())
            .field("gas_limit", &self.gas_limit())
            .field("max_fee_per_gas", &self.max_fee_per_gas())
            .field("max_priority_fee_per_gas", &self.max_priority_fee_per_gas())
            .field("access_list", &self.access_list())
            .field("input", &hex::encode(self.data()))
            .field("sender", &self.sender)
            .finish()
    }
}

impl TryFrom<TransactionV2> for SignedTx {
    type Error = TransactionError;

    fn try_from(src: TransactionV2) -> Result<Self, Self::Error> {
        let pubkey = match &src {
            TransactionV2::Legacy(tx) => {
                let msg = ethereum::LegacyTransactionMessage {
                    nonce: tx.nonce,
                    gas_price: tx.gas_price,
                    gas_limit: tx.gas_limit,
                    action: tx.action,
                    value: tx.value,
                    input: tx.input.clone(),
                    chain_id: tx.signature.chain_id(),
                };
                let hash = msg.hash();
                recover_public_key(
                    &hash,
                    tx.signature.r(),
                    tx.signature.s(),
                    tx.signature.standard_v(),
                )
            }
            TransactionV2::EIP2930(tx) => {
                let msg = ethereum::EIP2930TransactionMessage {
                    chain_id: tx.chain_id,
                    nonce: tx.nonce,
                    gas_price: tx.gas_price,
                    gas_limit: tx.gas_limit,
                    action: tx.action,
                    value: tx.value,
                    input: tx.input.clone(),
                    access_list: tx.access_list.clone(),
                };
                let hash = msg.hash();
                recover_public_key(&hash, &tx.r, &tx.s, u8::from(tx.odd_y_parity))
            }
            TransactionV2::EIP1559(tx) => {
                let msg = ethereum::EIP1559TransactionMessage {
                    chain_id: tx.chain_id,
                    nonce: tx.nonce,
                    max_priority_fee_per_gas: tx.max_priority_fee_per_gas,
                    max_fee_per_gas: tx.max_fee_per_gas,
                    gas_limit: tx.gas_limit,
                    action: tx.action,
                    value: tx.value,
                    input: tx.input.clone(),
                    access_list: tx.access_list.clone(),
                };
                let hash = msg.hash();
                recover_public_key(&hash, &tx.r, &tx.s, u8::from(tx.odd_y_parity))
            }
        }?;
        Ok(SignedTx {
            transaction: src,
            sender: public_key_to_address(&pubkey),
            hash_cache: Cell::new(None),
        })
    }
}

use hex::FromHex;

impl TryFrom<&str> for SignedTx {
    type Error = TransactionError;

    fn try_from(src: &str) -> Result<Self, Self::Error> {
        let buffer = <Vec<u8>>::from_hex(src)?;
        let tx: TransactionV2 = ethereum::EnvelopedDecodable::decode(&buffer)?;

        tx.try_into()
    }
}

impl SignedTx {
    pub fn nonce(&self) -> U256 {
        match &self.transaction {
            TransactionV2::Legacy(t) => t.nonce,
            TransactionV2::EIP2930(t) => t.nonce,
            TransactionV2::EIP1559(t) => t.nonce,
        }
    }

    pub fn to(&self) -> Option<H160> {
        match self.action() {
            TransactionAction::Call(to) => Some(to),
            TransactionAction::Create => None,
        }
    }

    pub fn action(&self) -> TransactionAction {
        match &self.transaction {
            TransactionV2::Legacy(t) => t.action,
            TransactionV2::EIP2930(t) => t.action,
            TransactionV2::EIP1559(t) => t.action,
        }
    }

    pub fn access_list(&self) -> AccessList {
        match &self.transaction {
            TransactionV2::Legacy(_) => Vec::new(),
            TransactionV2::EIP2930(tx) => tx.access_list.clone(),
            TransactionV2::EIP1559(tx) => tx.access_list.clone(),
        }
    }

    pub fn value(&self) -> U256 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.value,
            TransactionV2::EIP2930(tx) => tx.value,
            TransactionV2::EIP1559(tx) => tx.value,
        }
    }

    pub fn gas_limit(&self) -> U256 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.gas_limit,
            TransactionV2::EIP2930(tx) => tx.gas_limit,
            TransactionV2::EIP1559(tx) => tx.gas_limit,
        }
    }

    pub fn gas_price(&self) -> U256 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.gas_price,
            TransactionV2::EIP2930(tx) => tx.gas_price,
            TransactionV2::EIP1559(tx) => tx.max_fee_per_gas,
        }
    }

    pub fn effective_gas_price(&self, base_fee: U256) -> Result<U256, EVMError> {
        match &self.transaction {
            TransactionV2::Legacy(tx) => Ok(tx.gas_price),
            TransactionV2::EIP2930(tx) => Ok(tx.gas_price),
            TransactionV2::EIP1559(tx) => Ok(min(
                tx.max_fee_per_gas,
                tx.max_priority_fee_per_gas
                    .checked_add(base_fee)
                    .ok_or_else(|| format_err!("effective_gas_price overflow"))?,
            )),
        }
    }

    pub fn effective_priority_fee_per_gas(&self, base_fee: U256) -> Result<U256, EVMError> {
        match &self.transaction {
            TransactionV2::Legacy(tx) => Ok(tx.gas_price.checked_sub(base_fee).unwrap_or_default()),
            TransactionV2::EIP2930(tx) => {
                Ok(tx.gas_price.checked_sub(base_fee).unwrap_or_default())
            }
            TransactionV2::EIP1559(tx) => {
                let max_priority_fee = tx.max_fee_per_gas.checked_sub(base_fee).unwrap_or_default();
                Ok(min(tx.max_priority_fee_per_gas, max_priority_fee))
            }
        }
    }

    pub fn data(&self) -> &[u8] {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.input.as_ref(),
            TransactionV2::EIP2930(tx) => tx.input.as_ref(),
            TransactionV2::EIP1559(tx) => tx.input.as_ref(),
        }
    }

    pub fn v(&self) -> u64 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.signature.v(),
            TransactionV2::EIP2930(tx) => u64::from(tx.odd_y_parity),
            TransactionV2::EIP1559(tx) => u64::from(tx.odd_y_parity),
        }
    }

    pub fn r(&self) -> H256 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => *tx.signature.r(),
            TransactionV2::EIP2930(tx) => tx.r,
            TransactionV2::EIP1559(tx) => tx.r,
        }
    }

    pub fn s(&self) -> H256 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => *tx.signature.s(),
            TransactionV2::EIP2930(tx) => tx.s,
            TransactionV2::EIP1559(tx) => tx.s,
        }
    }

    pub fn max_fee_per_gas(&self) -> Option<U256> {
        match &self.transaction {
            TransactionV2::Legacy(_) | TransactionV2::EIP2930(_) => None,
            TransactionV2::EIP1559(tx) => Some(tx.max_fee_per_gas),
        }
    }

    pub fn max_priority_fee_per_gas(&self) -> Option<U256> {
        match &self.transaction {
            TransactionV2::Legacy(_) | TransactionV2::EIP2930(_) => None,
            TransactionV2::EIP1559(tx) => Some(tx.max_priority_fee_per_gas),
        }
    }

    pub fn chain_id(&self) -> u64 {
        match &self.transaction {
            TransactionV2::Legacy(tx) => tx.signature.chain_id().unwrap_or_default(),
            TransactionV2::EIP2930(tx) => tx.chain_id,
            TransactionV2::EIP1559(tx) => tx.chain_id,
        }
    }

    pub fn hash(&self) -> H256 {
        let h = &self.hash_cache;
        if h.get().is_none() {
            let val = match &self.transaction {
                TransactionV2::Legacy(tx) => tx.hash(),
                TransactionV2::EIP2930(tx) => tx.hash(),
                TransactionV2::EIP1559(tx) => tx.hash(),
            };
            h.set(Some(val));
        }
        h.get().unwrap()
    }

    pub fn get_tx_type(&self) -> U256 {
        U256::from(EnvelopedEncodable::type_id(&self.transaction).unwrap_or_default())
    }
}

use std::{
    cell::Cell,
    cmp::min,
    convert::{TryFrom, TryInto},
    fmt,
};

#[derive(Debug)]
pub enum TransactionError {
    Secp256k1Error(secp256k1::Error),
    DecodingError,
    SignatureError,
    FromHexError(hex::FromHexError),
    ConstructionError,
}

impl fmt::Display for TransactionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            TransactionError::Secp256k1Error(ref e) => write!(f, "Secp256k1 error: {e}"),
            TransactionError::DecodingError => {
                write!(f, "Error decoding raw transaction")
            }
            TransactionError::SignatureError => {
                write!(f, "Error creating new signature")
            }
            TransactionError::FromHexError(ref e) => {
                write!(f, "Error parsing hex: {e}")
            }
            TransactionError::ConstructionError => {
                write!(f, "Error constructing Transaction")
            }
        }
    }
}

impl std::error::Error for TransactionError {}

use std::convert::From;

impl From<secp256k1::Error> for TransactionError {
    fn from(e: secp256k1::Error) -> Self {
        TransactionError::Secp256k1Error(e)
    }
}

impl From<hex::FromHexError> for TransactionError {
    fn from(e: hex::FromHexError) -> Self {
        TransactionError::FromHexError(e)
    }
}

impl<T> From<EnvelopedDecoderError<T>> for TransactionError {
    fn from(_: EnvelopedDecoderError<T>) -> Self {
        TransactionError::DecodingError
    }
}

#[cfg(test)]
mod tests {
    use std::{error::Error, fs, path::Path};

    use ethereum::{AccessListItem, EnvelopedEncodable, TransactionAction};
    use ethereum_types::{H160, H256, U256, U64};
    use serde::Deserialize;

    use crate::{
        bytes::Bytes,
        transaction::{
            test_helpers::{
                eip1559_tx, eip2930_tx, legacy_tx, legacy_tx_with, TxFields, TEST_CHAIN_ID,
            },
            SignedTx, TransactionError,
        },
    };

    #[test]
    fn test_signed_tx_from_raw_tx() {
        // Legacy
        let signed_tx = crate::transaction::SignedTx::try_from("f86b8085689451eee18252089434c1ca09a2dc717d89baef2f30ff6a6b2975e17e872386f26fc10000802da0ae5c76f8073460cbc7a911d3cc1b367072db64848a9532343559ce6917c51a46a01d2e4928450c59acca3de8340eb15b7446b37936265a51ab35e63f749a048002").unwrap();

        assert_eq!(
            hex::encode(signed_tx.sender.as_fixed_bytes()),
            "f829754bae400b679febefdcfc9944c323e1f94e"
        );

        // EIP-1559
        let signed_tx = crate::transaction::SignedTx::try_from("02f871018302fe7f80850735ebc84f827530942f777e9f26aa138ed21c404079e80656b448c7118774ab8279a9e27380c001a0f97db05e9814734c6d7bcca5ce644fc1b780c28e8617eec4a3142712777cabe7a01ad8667f28d7cc1b2e0884340c67d73644fac314da4bab3bfc068cf00c622774").unwrap();

        assert_eq!(
            hex::encode(signed_tx.sender.as_fixed_bytes()),
            "95222290dd7278aa3ddd389cc1e1d165cc4bafe5"
        );

        // EIP-2930
        let signed_tx = crate::transaction::SignedTx::try_from("01f86d050185689451eee18252089434c1ca09a2dc717d89baef2f30ff6a6b2975e17e872386f26fc1000080c080a0632502442f6bd0dbe14c087798277ce04bdede53c4642559a0a7d7e20fc7e8f1a0517c7504cb9adfe67f58dd43e00e77b4b2159e9f2c378b7616ba30dfa711ec8f").unwrap();

        assert_eq!(
            hex::encode(signed_tx.sender.as_fixed_bytes()),
            "f829754bae400b679febefdcfc9944c323e1f94e"
        );
    }

    #[derive(Deserialize, Debug)]
    #[serde(rename_all = "camelCase")]
    struct ExpectedTx {
        hash: H256,
        from: H160,
        to: Option<H160>,
        gas: U256,
        gas_price: U256,
        value: U256,
        input: Bytes,
        nonce: U256,
        v: U64,
        r: H256,
        s: H256,
        _block_hash: H256,
        _block_number: U256,
        _transaction_index: U256,
        #[serde(rename = "type")]
        r#type: U64,
        max_fee_per_gas: Option<U256>,
        max_priority_fee_per_gas: Option<U256>,
        #[serde(default)]
        access_list: Vec<AccessListItem>,
        chain_id: Option<U64>,
    }

    #[cfg(test)]
    fn get_test_data(path: &Path) -> Result<Vec<(String, ExpectedTx)>, Box<dyn Error>> {
        let content = fs::read_to_string(path)?;

        let r = serde_json::from_str(&content)?;
        Ok(r)
    }

    fn assert_raw_hash_matches_expected_tx((raw_hash, tx): (String, ExpectedTx)) {
        let signed_tx: SignedTx = (&raw_hash[2..]).try_into().unwrap();

        assert_eq!(signed_tx.sender, tx.from);
        assert_eq!(signed_tx.gas_limit(), tx.gas);
        assert_eq!(signed_tx.to(), tx.to);
        assert_eq!(signed_tx.hash(), tx.hash);
        assert_eq!(signed_tx.data().to_vec(), tx.input.0);
        assert_eq!(signed_tx.nonce(), tx.nonce);
        assert_eq!(signed_tx.value(), tx.value);
        assert_eq!(signed_tx.access_list(), tx.access_list);
        assert_eq!(signed_tx.max_fee_per_gas(), tx.max_fee_per_gas);
        assert_eq!(
            signed_tx.max_priority_fee_per_gas(),
            tx.max_priority_fee_per_gas
        );
        assert_eq!(U64::from(signed_tx.v()), tx.v);
        assert_eq!(signed_tx.r(), tx.r);
        assert_eq!(signed_tx.s(), tx.s);

        if let Some(chain_id) = tx.chain_id {
            assert_eq!(U64::from(signed_tx.chain_id()), chain_id);
        }

        let r#type =
            U64::from(EnvelopedEncodable::type_id(&signed_tx.transaction).unwrap_or_default());
        assert_eq!(r#type, tx.r#type);

        // Can't get gas_price without block base fee
        if r#type == U64::zero() || r#type == U64::one() {
            assert_eq!(signed_tx.gas_price(), tx.gas_price);
        }
    }

    #[test]
    fn test_transfer_txs() -> Result<(), Box<dyn Error>> {
        get_test_data(Path::new("./testdata/transfer_txs.json"))?
            .into_iter()
            .for_each(assert_raw_hash_matches_expected_tx);
        Ok(())
    }

    #[test]
    fn test_call_smart_contract_txs() -> Result<(), Box<dyn Error>> {
        get_test_data(Path::new("./testdata/call_smart_contract_txs.json"))?
            .into_iter()
            .for_each(assert_raw_hash_matches_expected_tx);
        Ok(())
    }

    #[test]
    fn test_deploy_smart_contract_txs() -> Result<(), Box<dyn Error>> {
        get_test_data(Path::new("./testdata/deploy_smart_contract_txs.json"))?
            .into_iter()
            .for_each(assert_raw_hash_matches_expected_tx);
        Ok(())
    }

    #[test]
    fn test_tx_type() -> Result<(), Box<dyn Error>> {
        let raw_legacy_tx = "0xf90152808522ecb25c008307a1208080b8fe608060405234801561001057600080fd5b5060df8061001f6000396000f3fe6080604052348015600f57600080fd5b506004361060285760003560e01c8063165c4a1614602d575b600080fd5b603c6038366004605f565b604e565b60405190815260200160405180910390f35b600060588284607f565b9392505050565b600080604083850312156070578182fd5b50508035926020909101359150565b600081600019048311821515161560a457634e487b7160e01b81526011600452602481fd5b50029056fea2646970667358221220223df7833fd08eb1cd3ce363a9c4cb4619c1068a5f5517ea8bb862ed45d994f764736f6c634300080200338208fda0f6d889435dbfe9ea49b984fe1cab94cae59fa774b602256abc9f657c2abdd5bea0609176ffd6023896913cd9e12b61a004a970187cc53623ef49afb0417cb44929";
        let raw_eip2930_tx = "0x01f9018d82046d018522ecb25c008307a1208080b8fe608060405234801561001057600080fd5b5060df8061001f6000396000f3fe6080604052348015600f57600080fd5b506004361060285760003560e01c8063165c4a1614602d575b600080fd5b603c6038366004605f565b604e565b60405190815260200160405180910390f35b600060588284607f565b9392505050565b600080604083850312156070578182fd5b50508035926020909101359150565b600081600019048311821515161560a457634e487b7160e01b81526011600452602481fd5b50029056fea2646970667358221220223df7833fd08eb1cd3ce363a9c4cb4619c1068a5f5517ea8bb862ed45d994f764736f6c63430008020033f838f7949b8a4af42140d8a4c153a822f02571a1dd037e89e1a0000000000000000000000000000000000000000000000000000000000000000001a0f69240ab7127d845ad35a450d377f9d7d81c3e9d350b12bf53d14dc44a9e7bf2a067c2e6d1d30f20c670fbb9b79296c26ff372086f2ca3aead851bcc3510b638a7";
        let raw_eip1559_tx = "0x02f9015a82046d02852363e7f0008522ecb25c008307a1208080b8fe608060405234801561001057600080fd5b5060df8061001f6000396000f3fe6080604052348015600f57600080fd5b506004361060285760003560e01c8063165c4a1614602d575b600080fd5b603c6038366004605f565b604e565b60405190815260200160405180910390f35b600060588284607f565b9392505050565b600080604083850312156070578182fd5b50508035926020909101359150565b600081600019048311821515161560a457634e487b7160e01b81526011600452602481fd5b50029056fea2646970667358221220223df7833fd08eb1cd3ce363a9c4cb4619c1068a5f5517ea8bb862ed45d994f764736f6c63430008020033c080a05eceb8ff0ce33c95106051d82896aaf41bc899f625189922d12edeb7ce6fd56da07c37f48b1f4dfbf89a81cbe62c93ab23eec00808f7daca0cc5cf29860d112759";

        // Test legacy tx
        let signed_tx: SignedTx = (raw_legacy_tx[2..]).try_into().unwrap();
        assert_eq!(signed_tx.get_tx_type(), U256::zero());

        // Test EIP2930 tx
        let signed_tx: SignedTx = (raw_eip2930_tx[2..]).try_into().unwrap();
        assert_eq!(signed_tx.get_tx_type(), U256::from(1));

        // Test EIP1559 tx
        let signed_tx: SignedTx = (raw_eip1559_tx[2..]).try_into().unwrap();
        assert_eq!(signed_tx.get_tx_type(), U256::from(2));
        Ok(())
    }

    /// Builds an access list with a single address and a single storage key.
    fn test_access_list() -> Vec<AccessListItem> {
        vec![AccessListItem {
            address: H160::from_low_u64_be(1),
            storage_keys: vec![H256::from_low_u64_be(2)],
        }]
    }

    #[test]
    fn access_list_is_empty_for_legacy_transactions() {
        let tx = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert!(tx.access_list().is_empty());
    }

    #[test]
    fn access_list_is_preserved_for_eip2930_transactions() {
        let access_list = test_access_list();
        let tx = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            access_list.clone(),
        );
        assert_eq!(tx.access_list(), access_list);

        // An empty access list is allowed as well.
        let tx = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert!(tx.access_list().is_empty());
    }

    #[test]
    fn access_list_is_preserved_for_eip1559_transactions() {
        let access_list = test_access_list();
        let tx = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            access_list.clone(),
        );
        assert_eq!(tx.access_list(), access_list);

        // An empty access list is allowed as well.
        let tx = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert!(tx.access_list().is_empty());
    }

    #[test]
    fn action_and_to_are_derived_from_the_transaction_type() {
        let to = H160::from_low_u64_be(0xdead);

        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(to),
            Vec::new(),
        );
        assert_eq!(legacy.action(), TransactionAction::Call(to));
        assert_eq!(legacy.to(), Some(to));

        let legacy_create = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Create,
            Vec::new(),
        );
        assert_eq!(legacy_create.action(), TransactionAction::Create);
        assert_eq!(legacy_create.to(), None);

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(to),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930.action(), TransactionAction::Call(to));
        assert_eq!(eip2930.to(), Some(to));

        let eip2930_create = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Create,
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930_create.action(), TransactionAction::Create);
        assert_eq!(eip2930_create.to(), None);

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(to),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip1559.action(), TransactionAction::Call(to));
        assert_eq!(eip1559.to(), Some(to));

        let eip1559_create = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Create,
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip1559_create.action(), TransactionAction::Create);
        assert_eq!(eip1559_create.to(), None);
    }

    #[test]
    fn gas_limit_is_passed_through_for_all_transaction_types() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(legacy.gas_limit(), U256::from(21_000));

        let eip2930 = eip2930_tx(
            U256::from(123_456),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930.gas_limit(), U256::from(123_456));

        let eip1559 = eip1559_tx(
            U256::from(654_321),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip1559.gas_limit(), U256::from(654_321));
    }

    #[test]
    fn effective_gas_price_ignores_the_base_fee_for_legacy_and_eip2930() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(
            legacy.effective_gas_price(U256::zero()).unwrap(),
            U256::from(100)
        );
        assert_eq!(
            legacy.effective_gas_price(U256::from(30)).unwrap(),
            U256::from(100)
        );
        assert_eq!(
            legacy.effective_gas_price(U256::MAX).unwrap(),
            U256::from(100)
        );

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(
            eip2930.effective_gas_price(U256::zero()).unwrap(),
            U256::from(100)
        );
        assert_eq!(
            eip2930.effective_gas_price(U256::from(30)).unwrap(),
            U256::from(100)
        );
        assert_eq!(
            eip2930.effective_gas_price(U256::MAX).unwrap(),
            U256::from(100)
        );
    }

    #[test]
    fn effective_gas_price_uses_min_of_max_fee_and_base_fee_plus_priority_fee() {
        let tx = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        // base fee + priority fee below the max fee
        assert_eq!(
            tx.effective_gas_price(U256::from(20)).unwrap(),
            U256::from(30)
        );
        // base fee + priority fee exactly at the max fee
        assert_eq!(
            tx.effective_gas_price(U256::from(90)).unwrap(),
            U256::from(100)
        );
        // base fee + priority fee above the max fee
        assert_eq!(
            tx.effective_gas_price(U256::from(95)).unwrap(),
            U256::from(100)
        );
        assert_eq!(
            tx.effective_gas_price(U256::from(200)).unwrap(),
            U256::from(100)
        );
    }

    #[test]
    fn effective_gas_price_overflow_is_reported() {
        let tx = eip1559_tx(
            U256::from(21_000),
            U256::MAX,
            U256::MAX,
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        let error = tx.effective_gas_price(U256::MAX).unwrap_err();
        assert_eq!(error.to_string(), "effective_gas_price overflow");
    }

    #[test]
    fn effective_priority_fee_per_gas_uses_gas_price_minus_base_fee() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(
            legacy
                .effective_priority_fee_per_gas(U256::from(30))
                .unwrap(),
            U256::from(70)
        );
        // base fee equal to the gas price
        assert_eq!(
            legacy
                .effective_priority_fee_per_gas(U256::from(100))
                .unwrap(),
            U256::zero()
        );

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(
            eip2930
                .effective_priority_fee_per_gas(U256::from(30))
                .unwrap(),
            U256::from(70)
        );
        assert_eq!(
            eip2930
                .effective_priority_fee_per_gas(U256::from(100))
                .unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn effective_priority_fee_per_gas_saturates_to_zero() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        // base fee above the gas price
        assert_eq!(
            legacy
                .effective_priority_fee_per_gas(U256::from(200))
                .unwrap(),
            U256::zero()
        );

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(
            eip2930
                .effective_priority_fee_per_gas(U256::from(200))
                .unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn effective_priority_fee_per_gas_uses_min_of_priority_fee_and_max_fee_minus_base_fee() {
        let tx = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        // the priority fee is the limiting factor
        assert_eq!(
            tx.effective_priority_fee_per_gas(U256::from(20)).unwrap(),
            U256::from(10)
        );
        // the max fee minus the base fee is the limiting factor
        assert_eq!(
            tx.effective_priority_fee_per_gas(U256::from(95)).unwrap(),
            U256::from(5)
        );
        // base fee equal to the max fee
        assert_eq!(
            tx.effective_priority_fee_per_gas(U256::from(100)).unwrap(),
            U256::zero()
        );
        // base fee above the max fee saturates to zero
        assert_eq!(
            tx.effective_priority_fee_per_gas(U256::from(150)).unwrap(),
            U256::zero()
        );
    }

    #[test]
    fn gas_price_and_fee_accessors_match_the_transaction_type() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(legacy.gas_price(), U256::from(100));
        assert_eq!(legacy.max_fee_per_gas(), None);
        assert_eq!(legacy.max_priority_fee_per_gas(), None);

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930.gas_price(), U256::from(100));
        assert_eq!(eip2930.max_fee_per_gas(), None);
        assert_eq!(eip2930.max_priority_fee_per_gas(), None);

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        // for EIP-1559 the gas price is the max fee per gas
        assert_eq!(eip1559.gas_price(), U256::from(100));
        assert_eq!(eip1559.max_fee_per_gas(), Some(U256::from(100)));
        assert_eq!(eip1559.max_priority_fee_per_gas(), Some(U256::from(10)));
    }

    #[test]
    fn value_data_and_nonce_are_passed_through() {
        let input = vec![1u8, 2, 3];
        let legacy = legacy_tx_with(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            input.clone(),
            TxFields::new(U256::from(1_234), U256::from(7)),
        );
        assert_eq!(legacy.value(), U256::from(1_234));
        assert_eq!(legacy.data(), input.as_slice());
        assert_eq!(legacy.nonce(), U256::from(7));

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            input.clone(),
            Vec::new(),
        );
        assert_eq!(eip2930.value(), U256::zero());
        assert_eq!(eip2930.data(), input.as_slice());
        assert_eq!(eip2930.nonce(), U256::zero());

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            input.clone(),
            Vec::new(),
        );
        assert_eq!(eip1559.value(), U256::zero());
        assert_eq!(eip1559.data(), input.as_slice());
        assert_eq!(eip1559.nonce(), U256::zero());
    }

    #[test]
    fn signature_fields_are_passed_through() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        // the legacy `v` is derived from the chain id and the parity of the signature
        assert!(legacy.v() == TEST_CHAIN_ID * 2 + 35 || legacy.v() == TEST_CHAIN_ID * 2 + 36);
        assert_ne!(legacy.r(), H256::zero());
        assert_ne!(legacy.s(), H256::zero());

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert!(eip2930.v() == 0 || eip2930.v() == 1);
        assert_ne!(eip2930.r(), H256::zero());
        assert_ne!(eip2930.s(), H256::zero());

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert!(eip1559.v() == 0 || eip1559.v() == 1);
        assert_ne!(eip1559.r(), H256::zero());
        assert_ne!(eip1559.s(), H256::zero());
    }

    #[test]
    fn chain_id_is_recovered_from_the_signature() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(legacy.chain_id(), TEST_CHAIN_ID);

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930.chain_id(), TEST_CHAIN_ID);

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip1559.chain_id(), TEST_CHAIN_ID);
    }

    #[test]
    fn hash_is_deterministic_and_transaction_specific() {
        let tx = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        // the hash is cached, but always returns the same value
        assert_eq!(tx.hash(), tx.hash());
        assert_eq!(tx.hash(), tx.transaction.hash());

        // a different transaction has a different hash
        let other = legacy_tx_with(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            TxFields::new(U256::zero(), U256::from(1)),
        );
        assert_ne!(tx.hash(), other.hash());
    }

    #[test]
    fn get_tx_type_returns_the_envelope_type() {
        let legacy = legacy_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
        );
        assert_eq!(legacy.get_tx_type(), U256::zero());

        let eip2930 = eip2930_tx(
            U256::from(21_000),
            U256::from(100),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip2930.get_tx_type(), U256::from(1));

        let eip1559 = eip1559_tx(
            U256::from(21_000),
            U256::from(100),
            U256::from(10),
            TransactionAction::Call(H160::zero()),
            Vec::new(),
            Vec::new(),
        );
        assert_eq!(eip1559.get_tx_type(), U256::from(2));
    }

    #[test]
    fn transaction_error_display_messages_are_stable() {
        assert_eq!(
            TransactionError::Secp256k1Error(secp256k1::Error::InvalidMessage).to_string(),
            "Secp256k1 error: message was not 32 bytes (do you need to hash?)"
        );
        assert_eq!(
            TransactionError::DecodingError.to_string(),
            "Error decoding raw transaction"
        );
        assert_eq!(
            TransactionError::SignatureError.to_string(),
            "Error creating new signature"
        );
        assert_eq!(
            TransactionError::FromHexError(hex::FromHexError::OddLength).to_string(),
            "Error parsing hex: Odd number of digits"
        );
        assert_eq!(
            TransactionError::ConstructionError.to_string(),
            "Error constructing Transaction"
        );
    }

    #[test]
    fn transaction_error_conversions_are_available() {
        let error: TransactionError = secp256k1::Error::InvalidSecretKey.into();
        assert!(matches!(error, TransactionError::Secp256k1Error(_)));

        let error: TransactionError = hex::FromHexError::OddLength.into();
        assert!(matches!(error, TransactionError::FromHexError(_)));

        let error: TransactionError = ethereum::EnvelopedDecoderError::<()>::UnknownTypeId.into();
        assert!(matches!(error, TransactionError::DecodingError));
    }

    #[test]
    fn signed_tx_from_invalid_hex_reports_a_hex_error() {
        let error = SignedTx::try_from("zz").unwrap_err();
        assert!(matches!(error, TransactionError::FromHexError(_)));
        assert_eq!(
            error.to_string(),
            "Error parsing hex: Invalid character 'z' at position 0"
        );
    }

    #[test]
    fn signed_tx_from_invalid_payload_reports_a_decoding_error() {
        let error = SignedTx::try_from("deadbeef").unwrap_err();
        assert!(matches!(error, TransactionError::DecodingError));
        assert_eq!(error.to_string(), "Error decoding raw transaction");
    }

    #[test]
    fn signed_tx_with_unrecoverable_signature_reports_a_secp256k1_error() {
        // EIP-2930 transaction with an `r` value that is not a valid secp256k1
        // signature component, the sender cannot be recovered from it
        let raw_tx = "01f86382046780648252089400000000000000000000000000000000000000008080c080a0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa00100000000000000000000000000000000000000000000000000000000000000";
        let error = SignedTx::try_from(raw_tx).unwrap_err();
        assert!(matches!(error, TransactionError::Secp256k1Error(_)));
        assert_eq!(error.to_string(), "Secp256k1 error: malformed signature");
    }
}

#[cfg(test)]
mod secp256k1_tests {
    use ethereum::TransactionAction;
    use ethereum_types::{H160, H256, U256};
    use hex_literal::hex;
    use secp256k1::{ecdsa::RecoverableSignature, Message, SecretKey};

    use crate::{
        ecrecover::public_key_to_address,
        transaction::{
            test_helpers::{TEST_CHAIN_ID, TEST_SECRET_KEY},
            LegacyUnsignedTransaction,
        },
    };

    /// Message hash used by the `ecrecover` recovery test.
    const MESSAGE_HASH: [u8; 32] =
        hex!("6091be99153563845f8af2ff710a9854a70551a5a6b914a296931444fa360d40");

    /// Independent RFC6979 (HMAC-SHA256) reference vector.
    ///
    /// The expected values were computed with a self-contained RFC6979 implementation
    /// (not with the `secp256k1` crate) that was cross-checked against the well-known
    /// Bitcoin test vector (private key `1`, `SHA256("Satoshi Nakamoto")` -> `r = 934b1ea1…3d8`,
    /// `s = 2442ce9d…9e5`), which the same implementation reproduces exactly. The test is
    /// therefore an *independent* regression test of the deterministic nonce derivation
    /// (RFC6979) and not a self-fulfilling snapshot of the current implementation.
    ///
    /// Note that the test secret key is `0x0101…01` (32 bytes of `0x01`), *not* the scalar
    /// `1`; its address is `0x1a642f0e3c3af545e7acbd38b07251b3990914f1`.
    #[test]
    fn signature_matches_the_independent_rfc6979_reference_vector() {
        let secret_key = SecretKey::from_secret_bytes(TEST_SECRET_KEY).unwrap();
        let message = Message::from_digest(MESSAGE_HASH);
        let signature = RecoverableSignature::sign_ecdsa_recoverable(message, &secret_key);
        let (recovery_id, compact) = signature.serialize_compact();

        assert_eq!(
            compact[0..32],
            hex!("380ccd9da90774f73a1edf736ae54c182a4814f03b11df5f0af1038da7396f2f")
        );
        assert_eq!(
            compact[32..64],
            hex!("16715bd0dd9b58102f9f8ecfec0aaed326e623abff06217311bc22084e0d58e5")
        );
        assert_eq!(recovery_id.to_u8(), 0);

        // The signature must recover the public key of the test secret key.
        let public_key = signature.recover_ecdsa(message).unwrap();
        assert_eq!(
            public_key_to_address(&public_key),
            H160::from_slice(&hex!("1a642f0e3c3af545e7acbd38b07251b3990914f1"))
        );
    }

    /// The legacy (EIP-155) `v` component is derived from the recovery id as
    /// `v = 2 * chain_id + 35 + (recid & 1)`.
    #[test]
    fn legacy_v_encoding_follows_the_recovery_id() {
        let unsigned = LegacyUnsignedTransaction {
            nonce: U256::zero(),
            gas_price: U256::from(100),
            gas_limit: U256::from(21_000),
            action: TransactionAction::Call(H160::zero()),
            value: U256::zero(),
            input: Vec::new(),
            sig: ethereum::TransactionSignature::new(
                27,
                H256::from_low_u64_be(1),
                H256::from_low_u64_be(1),
            )
            .unwrap(),
        };

        let message = Message::from_digest(*unsigned.signing_hash(TEST_CHAIN_ID).as_fixed_bytes());
        let secret_key = SecretKey::from_secret_bytes(TEST_SECRET_KEY).unwrap();
        let signature = RecoverableSignature::sign_ecdsa_recoverable(message, &secret_key);
        let (recovery_id, compact) = signature.serialize_compact();

        let signed = unsigned
            .sign_with_chain_id(&TEST_SECRET_KEY, TEST_CHAIN_ID)
            .unwrap();
        assert_eq!(
            signed.signature.v(),
            TEST_CHAIN_ID * 2 + 35 + u64::from(recovery_id.to_u8() & 1)
        );
        assert_eq!(*signed.signature.r(), H256::from_slice(&compact[0..32]));
        assert_eq!(*signed.signature.s(), H256::from_slice(&compact[32..64]));
    }
}
