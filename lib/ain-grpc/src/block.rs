use std::fmt;

use ain_evm::bytes::Bytes;
use ethereum::{BlockAny, Header, TransactionV2};
use ethereum_types::{H160, H256, H64, U256};
use rlp::Encodable;
use serde::{
    de::{Error, MapAccess, Visitor},
    Deserialize, Deserializer, Serialize, Serializer,
};
use sha3::Digest;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct RpcBlockHeader {
    // Parity specific - https://github.com/openethereum/parity-ethereum/issues/2401
    pub author: H160,
    pub base_fee_per_gas: U256,
    pub difficulty: U256,
    pub extra_data: Bytes,
    pub gas_limit: U256,
    pub gas_used: U256,
    pub hash: H256,
    pub logs_bloom: String,
    pub miner: H160,
    pub mix_hash: H256,
    pub nonce: H64,
    pub number: U256,
    pub parent_hash: H256,
    pub receipts_root: H256,
    // Parity specific - https://github.com/ethereum/EIPs/issues/95
    pub seal_fields: Vec<Bytes>,
    pub sha3_uncles: H256,
    pub size: String,
    pub state_root: H256,
    pub timestamp: U256,
    pub total_difficulty: U256,
    pub transactions_root: H256,
}

impl From<Header> for RpcBlockHeader {
    fn from(header: Header) -> Self {
        RpcBlockHeader {
            author: header.beneficiary,
            base_fee_per_gas: header.base_fee,
            difficulty: header.difficulty,
            extra_data: Bytes::from(header.extra_data.clone()),
            gas_limit: header.gas_limit,
            gas_used: header.gas_used,
            hash: header.hash(),
            logs_bloom: format!("{:#x}", header.logs_bloom),
            miner: header.beneficiary,
            mix_hash: header.mix_hash,
            nonce: header.nonce,
            number: header.number,
            parent_hash: header.parent_hash,
            receipts_root: header.receipts_root,
            seal_fields: vec![
                Bytes::from(header.mix_hash.as_fixed_bytes().to_vec()),
                Bytes::from(header.nonce.as_fixed_bytes().to_vec()),
            ],
            sha3_uncles: H256::from_slice(&sha3::Keccak256::digest(
                &rlp::RlpStream::new_list(0).out(),
            )),
            size: format!("{:#x}", header.rlp_bytes().len()),
            state_root: header.state_root,
            total_difficulty: U256::zero(),
            timestamp: header.timestamp.into(),
            transactions_root: header.transactions_root,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RpcBlock {
    #[serde(flatten)]
    pub header: RpcBlockHeader,
    pub transactions: BlockTransactions,
    pub uncles: Vec<H256>,
}

impl RpcBlock {
    pub fn from_block_with_tx(block: BlockAny, full_transactions: bool) -> Self {
        RpcBlock {
            header: RpcBlockHeader::from(block.header.clone()),
            transactions: if full_transactions {
                // Discard failed to retrieved transactions with flat_map.
                // Should not happen as the transaction should not make it in the block in the first place.
                BlockTransactions::Full(
                    block
                        .transactions
                        .iter()
                        .enumerate()
                        .flat_map(|(index, tx)| {
                            EthTransactionInfo::try_from_tx_block_and_index(tx, &block, index)
                        })
                        .collect(),
                )
            } else {
                BlockTransactions::Hashes(
                    block.transactions.iter().map(TransactionV2::hash).collect(),
                )
            },
            uncles: vec![],
        }
    }
}

/// Represents rpc api block number param.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash, Default)]
pub enum BlockNumber {
    /// Hash
    Hash {
        /// block hash
        hash: H256,
        /// only return blocks part of the canon chain
        require_canonical: bool,
    },
    /// Number
    Num(u64),
    /// Latest block
    #[default]
    Latest,
    /// Earliest block (genesis)
    Earliest,
    /// Pending block (being mined)
    Pending,
    /// The most recent crypto-economically secure block.
    /// There is no difference between Ethereum's `safe` and `finalized`
    /// in Substrate finality gadget.
    Safe,
    /// The most recent crypto-economically secure block.
    Finalized,
}

impl<'a> Deserialize<'a> for BlockNumber {
    fn deserialize<D>(deserializer: D) -> Result<BlockNumber, D::Error>
    where
        D: Deserializer<'a>,
    {
        deserializer.deserialize_any(BlockNumberVisitor)
    }
}

impl BlockNumber {
    /// Convert block number to min block target.
    #[must_use]
    pub fn convert_to_min_block_num(&self) -> Option<u64> {
        match *self {
            BlockNumber::Num(ref x) => Some(*x),
            BlockNumber::Earliest => Some(0),
            _ => None,
        }
    }
}

impl Serialize for BlockNumber {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            BlockNumber::Hash {
                hash,
                require_canonical,
            } => serializer.serialize_str(&format!(
                "{{ 'hash': '{hash}', 'requireCanonical': '{require_canonical}'  }}"
            )),
            BlockNumber::Num(ref x) => serializer.serialize_str(&format!("0x{x:x}")),
            BlockNumber::Latest => serializer.serialize_str("latest"),
            BlockNumber::Earliest => serializer.serialize_str("earliest"),
            BlockNumber::Pending => serializer.serialize_str("pending"),
            BlockNumber::Safe => serializer.serialize_str("safe"),
            BlockNumber::Finalized => serializer.serialize_str("finalized"),
        }
    }
}

struct BlockNumberVisitor;

impl<'a> Visitor<'a> for BlockNumberVisitor {
    type Value = BlockNumber;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(
            formatter,
            "a block number or 'latest', 'safe', 'finalized', 'earliest' or 'pending'"
        )
    }

    fn visit_map<V>(self, mut visitor: V) -> Result<Self::Value, V::Error>
    where
        V: MapAccess<'a>,
    {
        let (mut require_canonical, mut block_number, mut block_hash) =
            (false, None::<u64>, None::<H256>);

        loop {
            let key_str: Option<String> = visitor.next_key()?;

            match key_str {
                Some(key) => match key.as_str() {
                    "blockNumber" => {
                        let value: String = visitor.next_value()?;
                        if let Some(stripped) = value.strip_prefix("0x") {
                            let number = u64::from_str_radix(stripped, 16)
                                .map_err(|e| Error::custom(format!("Invalid block number: {e}")))?;

                            block_number = Some(number);
                            break;
                        } else {
                            return Err(Error::custom(
                                "Invalid block number: missing 0x prefix".to_string(),
                            ));
                        }
                    }
                    "blockHash" => {
                        block_hash = Some(visitor.next_value()?);
                    }
                    "requireCanonical" => {
                        require_canonical = visitor.next_value()?;
                    }
                    key => return Err(Error::custom(format!("Unknown key: {key}"))),
                },
                None => break,
            };
        }

        if let Some(number) = block_number {
            return Ok(BlockNumber::Num(number));
        }

        if let Some(hash) = block_hash {
            return Ok(BlockNumber::Hash {
                hash,
                require_canonical,
            });
        }

        Err(Error::custom("Invalid input"))
    }

    fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
    where
        E: Error,
    {
        match value {
            "latest" => Ok(BlockNumber::Latest),
            "earliest" => Ok(BlockNumber::Earliest),
            "pending" => Ok(BlockNumber::Pending),
            "safe" => Ok(BlockNumber::Safe),
            "finalized" => Ok(BlockNumber::Finalized),
            _ if value.starts_with("0x") => u64::from_str_radix(&value[2..], 16)
                .map(BlockNumber::Num)
                .map_err(|e| Error::custom(format!("Invalid block number: {e}"))),
            _ => value.parse::<u64>().map(BlockNumber::Num).map_err(|_| {
                Error::custom("Invalid block number: non-decimal or missing 0x prefix".to_string())
            }),
        }
    }

    fn visit_string<E>(self, value: String) -> Result<Self::Value, E>
    where
        E: Error,
    {
        self.visit_str(value.as_ref())
    }

    fn visit_u64<E>(self, value: u64) -> Result<Self::Value, E>
    where
        E: Error,
    {
        Ok(BlockNumber::Num(value))
    }
}

use std::str::FromStr;

use ain_evm::block::FeeHistoryData;

use crate::codegen::types::EthTransactionInfo;

impl FromStr for BlockNumber {
    type Err = serde_json::Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let visitor = BlockNumberVisitor;
        let result = visitor.visit_str(s).map_err(|e: serde::de::value::Error| {
            serde_json::Error::custom(format!("Error while parsing BlockNumber: {e}"))
        });
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn match_block_number(block_number: BlockNumber) -> Option<u64> {
        match block_number {
            BlockNumber::Num(number) => Some(number),
            BlockNumber::Earliest => Some(0),
            BlockNumber::Latest => Some(1000),
            BlockNumber::Safe => Some(999),
            BlockNumber::Finalized => Some(999),
            BlockNumber::Pending => Some(1001),
            _ => None,
        }
    }

    #[test]
    fn block_number_deserialize() {
        let bn_dec: BlockNumber = serde_json::from_str(r#""42""#).unwrap();
        let bn_hex: BlockNumber = serde_json::from_str(r#""0x45""#).unwrap();
        let bn_u64: BlockNumber = serde_json::from_str(r#"420"#).unwrap();
        let bn_tag_earliest: BlockNumber = serde_json::from_str(r#""earliest""#).unwrap();
        let bn_tag_latest: BlockNumber = serde_json::from_str(r#""latest""#).unwrap();
        let bn_tag_safe: BlockNumber = serde_json::from_str(r#""safe""#).unwrap();
        let bn_tag_finalized: BlockNumber = serde_json::from_str(r#""finalized""#).unwrap();
        let bn_tag_pending: BlockNumber = serde_json::from_str(r#""pending""#).unwrap();

        assert_eq!(match_block_number(bn_dec).unwrap(), 42);
        assert_eq!(match_block_number(bn_hex).unwrap(), 69);
        assert_eq!(match_block_number(bn_u64).unwrap(), 420);
        assert_eq!(match_block_number(bn_tag_earliest).unwrap(), 0);
        assert_eq!(match_block_number(bn_tag_latest).unwrap(), 1000);
        assert_eq!(match_block_number(bn_tag_safe).unwrap(), 999);
        assert_eq!(match_block_number(bn_tag_finalized).unwrap(), 999);
        assert_eq!(match_block_number(bn_tag_pending).unwrap(), 1001);
    }

    #[test]
    fn block_number_convert_to_min_and_serialize_and_from_str() {
        assert_eq!(BlockNumber::Num(7).convert_to_min_block_num(), Some(7));
        assert_eq!(BlockNumber::Earliest.convert_to_min_block_num(), Some(0));
        assert_eq!(BlockNumber::Latest.convert_to_min_block_num(), None);
        assert_eq!(BlockNumber::Pending.convert_to_min_block_num(), None);
        assert_eq!(BlockNumber::Safe.convert_to_min_block_num(), None);
        assert_eq!(BlockNumber::Finalized.convert_to_min_block_num(), None);
        let hash = H256::from_low_u64_be(0xaa);
        assert_eq!(
            BlockNumber::Hash {
                hash,
                require_canonical: true,
            }
            .convert_to_min_block_num(),
            None
        );

        assert_eq!(serde_json::to_string(&BlockNumber::Num(0x1a)).unwrap(), r#""0x1a""#);
        assert_eq!(serde_json::to_string(&BlockNumber::Latest).unwrap(), r#""latest""#);
        assert_eq!(serde_json::to_string(&BlockNumber::Earliest).unwrap(), r#""earliest""#);
        assert_eq!(serde_json::to_string(&BlockNumber::Pending).unwrap(), r#""pending""#);
        assert_eq!(serde_json::to_string(&BlockNumber::Safe).unwrap(), r#""safe""#);
        assert_eq!(serde_json::to_string(&BlockNumber::Finalized).unwrap(), r#""finalized""#);
        let hash_ser = serde_json::to_string(&BlockNumber::Hash {
            hash,
            require_canonical: true,
        })
        .unwrap();
        assert!(hash_ser.contains("requireCanonical"));

        assert_eq!(BlockNumber::from_str("latest").unwrap(), BlockNumber::Latest);
        assert_eq!(BlockNumber::from_str("0x10").unwrap(), BlockNumber::Num(16));
        assert_eq!(BlockNumber::from_str("42").unwrap(), BlockNumber::Num(42));
        assert!(BlockNumber::from_str("not_a_tag").is_err());
        assert!(BlockNumber::from_str("0xzzzz").is_err());
    }

    #[test]
    fn block_number_deserialize_map_and_invalid_inputs() {
        let from_num: BlockNumber = serde_json::from_str(r#"{"blockNumber":"0x10"}"#).unwrap();
        assert_eq!(from_num, BlockNumber::Num(16));

        let hash = H256::from_low_u64_be(0x1234);
        let from_hash: BlockNumber = serde_json::from_str(&format!(
            r#"{{"blockHash":"{hash:#x}","requireCanonical":true}}"#
        ))
        .unwrap();
        assert_eq!(
            from_hash,
            BlockNumber::Hash {
                hash,
                require_canonical: true,
            }
        );

        // Invalid map inputs
        assert!(serde_json::from_str::<BlockNumber>(r#"{"blockNumber":"16"}"#).is_err());
        assert!(serde_json::from_str::<BlockNumber>(r#"{"blockNumber":"0xzz"}"#).is_err());
        assert!(serde_json::from_str::<BlockNumber>(r#"{"unknownKey":"0x1"}"#).is_err());
        assert!(serde_json::from_str::<BlockNumber>(r#"{}"#).is_err());

        // Invalid primitive types
        assert!(serde_json::from_str::<BlockNumber>(r#"true"#).is_err());
        assert!(serde_json::from_str::<BlockNumber>(r#"[]"#).is_err());
        assert!(serde_json::from_str::<BlockNumber>(r#""invalid_tag""#).is_err());
    }

    #[test]
    fn rpc_block_header_block_transactions_and_fee_history_conversions() {
        let partial = ethereum::PartialHeader {
            parent_hash: H256::from_low_u64_be(1),
            beneficiary: H160::from_low_u64_be(2),
            state_root: H256::from_low_u64_be(3),
            receipts_root: H256::from_low_u64_be(4),
            logs_bloom: Default::default(),
            difficulty: U256::from(5),
            number: U256::from(10),
            gas_limit: U256::from(30_000_000),
            gas_used: U256::from(21_000),
            timestamp: 1_700_000_000,
            extra_data: vec![0xab],
            mix_hash: H256::from_low_u64_be(6),
            nonce: H64::from_low_u64_be(7),
            base_fee: U256::from(1_000_000_000),
        };
        let block = ethereum::Block::new(partial, Vec::new(), Vec::new());

        let rpc_block_hashes = RpcBlock::from_block_with_tx(block.clone(), false);
        assert_eq!(rpc_block_hashes.header.number, U256::from(10));
        assert_eq!(rpc_block_hashes.header.author, H160::from_low_u64_be(2));
        assert_eq!(rpc_block_hashes.header.miner, H160::from_low_u64_be(2));
        assert_eq!(rpc_block_hashes.header.seal_fields.len(), 2);
        assert_eq!(rpc_block_hashes.transactions, BlockTransactions::Hashes(vec![]));

        let rpc_block_full = RpcBlock::from_block_with_tx(block, true);
        assert_eq!(rpc_block_full.transactions, BlockTransactions::Full(vec![]));

        // BlockTransactions serialization
        assert_eq!(
            serde_json::to_string(&BlockTransactions::None).unwrap(),
            "[]"
        );
        assert_eq!(
            serde_json::to_string(&BlockTransactions::Hashes(vec![H256::zero()])).unwrap(),
            format!(r#"["{:#x}"]"#, H256::zero())
        );

        // RpcFeeHistory conversion
        let fee_history = FeeHistoryData {
            oldest_block: U256::from(5),
            base_fee_per_gas: vec![U256::from(100)],
            gas_used_ratio: vec![0.5],
            reward: Some(vec![vec![U256::from(10)]]),
        };
        let rpc_fh = RpcFeeHistory::from(fee_history);
        assert_eq!(rpc_fh.oldest_block, U256::from(5));
        assert_eq!(rpc_fh.base_fee_per_gas, vec![U256::from(100)]);
        assert_eq!(rpc_fh.gas_used_ratio, vec![0.5]);
        assert!(rpc_fh.reward.is_some());
    }
}

/// Block Transactions
#[derive(Debug, Deserialize, Clone, PartialEq)]
pub enum BlockTransactions {
    /// Only hashes
    Hashes(Vec<H256>),
    /// Full transactions
    Full(Vec<EthTransactionInfo>),
    None,
}

impl Serialize for BlockTransactions {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            BlockTransactions::Hashes(ref hashes) => hashes.serialize(serializer),
            BlockTransactions::Full(ref ts) => ts.serialize(serializer),
            BlockTransactions::None => Vec::<()>::new().serialize(serializer),
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RpcFeeHistory {
    pub oldest_block: U256,
    pub base_fee_per_gas: Vec<U256>,
    pub gas_used_ratio: Vec<f64>,
    pub reward: Option<Vec<Vec<U256>>>,
}

impl From<FeeHistoryData> for RpcFeeHistory {
    fn from(value: FeeHistoryData) -> Self {
        Self {
            oldest_block: value.oldest_block,
            base_fee_per_gas: value.base_fee_per_gas,
            gas_used_ratio: value.gas_used_ratio,
            reward: value.reward,
        }
    }
}
