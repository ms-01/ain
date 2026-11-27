mod block;
mod governance;
mod masternode;
mod masternode_stats;
pub mod oracle;
mod oracle_price_active;
mod oracle_price_aggregated;
mod oracle_price_aggregated_interval;
mod oracle_price_feed;
mod oracle_token_currency;
mod poolswap;
mod poolswap_aggregated;
mod price_ticker;
mod raw_block;
mod raw_tx;
mod script_activity;
mod script_aggregation;
mod script_unspent;
mod transaction;
mod transaction_vin;
mod transaction_vout;
mod tx_result;
mod vault_auction_batch_history;
pub use block::*;
pub use governance::*;
pub use masternode::*;
pub use masternode_stats::*;
pub use oracle::*;
pub use oracle_price_active::*;
pub use oracle_price_aggregated::*;
pub use oracle_price_aggregated_interval::*;
pub use oracle_price_feed::*;
pub use oracle_token_currency::*;
pub use poolswap::*;
pub use poolswap_aggregated::*;
pub use price_ticker::*;
pub use raw_tx::*;
// pub use raw_block::*;
pub use script_activity::*;
pub use script_aggregation::*;
pub use script_unspent::*;
pub use transaction::*;
pub use transaction_vin::*;
pub use transaction_vout::*;
pub use tx_result::*;
pub use vault_auction_batch_history::*;

#[cfg(test)]
mod tests {
    use bitcoin::{hashes::Hash, BlockHash, Txid};
    use defichain_rpc::json::{
        blockchain::{ScriptSig, Vin, VinCoinbase, VinStandard},
        governance::{ProposalInfo, ProposalStatus, ProposalType},
    };
    use rust_decimal_macros::dec;

    use super::*;

    #[test]
    fn test_block_context_default_and_block_serde() {
        let ctx = BlockContext::default();
        assert_eq!(ctx.hash, BlockHash::all_zeros());
        assert_eq!(ctx.height, 0);

        let block = Block {
            hash: BlockHash::all_zeros(),
            id: BlockHash::all_zeros(),
            previous_hash: None,
            height: 10,
            version: 1,
            time: 1000,
            median_time: 999,
            transaction_count: 2,
            difficulty: 1.5,
            masternode: None,
            minter: None,
            minter_block_count: 0,
            stake_modifier: "00".to_string(),
            merkleroot: "11".to_string(),
            size: 256,
            size_stripped: 200,
            weight: 800,
        };
        let encoded = bincode::serialize(&block).unwrap();
        let decoded: Block = bincode::deserialize(&encoded).unwrap();
        assert_eq!(decoded, block);
    }

    #[test]
    fn test_script_activity_type_display() {
        assert_eq!(ScriptActivityType::Vin.to_string(), "vin");
        assert_eq!(ScriptActivityType::Vout.to_string(), "vout");
        assert_eq!(ScriptActivityTypeHex::Vin.to_string(), "00");
        assert_eq!(ScriptActivityTypeHex::Vout.to_string(), "01");
    }

    #[test]
    fn test_oracle_interval_seconds_serde() {
        for (variant, expected) in [
            (OracleIntervalSeconds::FifteenMinutes, "\"900\""),
            (OracleIntervalSeconds::OneHour, "\"3600\""),
            (OracleIntervalSeconds::OneDay, "\"86400\""),
        ] {
            let s = serde_json::to_string(&variant).unwrap();
            assert_eq!(s, expected);
            let back: OracleIntervalSeconds = serde_json::from_str(&s).unwrap();
            assert_eq!(back, variant);
        }
    }

    #[test]
    fn test_transaction_vin_from_vin_and_txid() {
        let txid = Txid::from_byte_array([7u8; 32]);
        let cb = TransactionVin::from_vin_and_txid(
            Vin::Coinbase(VinCoinbase {
                coinbase: "abcd".to_string(),
                sequence: 42,
            }),
            txid,
            &[],
        );
        assert!(matches!(cb.r#type, TransactionVinType::Coinbase(ref s) if s == "abcd"));
        assert!(cb.vout.is_none());

        let prev_txid = Txid::from_byte_array([9u8; 32]);
        let vouts = vec![TransactionVout {
            vout: 0,
            txid: prev_txid,
            n: 0,
            value: 12.5,
            token_id: Some(0),
            script: TransactionVoutScript {
                r#type: "witness_v0_keyhash".to_string(),
                hex: vec![1, 2, 3],
            },
        }];
        let script_sig: ScriptSig =
            serde_json::from_value(serde_json::json!({ "asm": "", "hex": "aa" })).unwrap();
        let std_vin = TransactionVin::from_vin_and_txid(
            Vin::Standard(VinStandard {
                txid: prev_txid,
                vout: 0,
                script_sig,
                txinwitness: Some(vec!["bb".to_string()]),
                sequence: 1,
            }),
            txid,
            &vouts,
        );
        assert!(matches!(std_vin.r#type, TransactionVinType::Standard((id, 0)) if id == prev_txid));
        assert_eq!(std_vin.vout.unwrap().value, 12.5);
        assert_eq!(std_vin.script, Some("aa".to_string()));
    }

    #[test]
    fn test_raw_tx_default_max_fee_rate() {
        assert_eq!(default_max_fee_rate().to_sat(), 10_000_000);
        let dto = RawTxDto {
            hex: "01000000".to_string(),
            max_fee_rate: Some(dec!(0.05)),
        };
        let json = serde_json::to_string(&dto).unwrap();
        let back: RawTxDto = serde_json::from_str(&json).unwrap();
        assert_eq!(back.hex, "01000000");
        assert_eq!(back.max_fee_rate, Some(dec!(0.05)));
    }

    #[test]
    fn test_api_proposal_info_from_proposal_info() {
        let info = ProposalInfo {
            proposal_id: "prop1".to_string(),
            title: "Test Proposal".to_string(),
            context: "https://example.com".to_string(),
            context_hash: "00".to_string(),
            r#type: ProposalType::CommunityFundProposal,
            status: ProposalStatus::Voting,
            current_cycle: 1,
            total_cycles: 2,
            creation_height: 100,
            cycle_end_height: 200,
            proposal_end_height: 300,
            voting_period: 100,
            approval_threshold: "50%".to_string(),
            quorum: "1%".to_string(),
            amount: Some(1000.5),
            payout_address: Some("df1qtest".to_string()),
            votes_possible: Some(100),
            votes_present: Some(50),
            votes_present_pct: Some("50%".to_string()),
            votes_yes: Some(40),
            votes_invalid: Some(0),
            votes_neutral: Some(5),
            votes_no: Some(5),
            votes_yes_pct: Some("80%".to_string()),
            fee: 10.0,
            options: None,
            fee_redistribution_per_vote: None,
            fee_redistribution_total: None,
        };
        let api_info = ApiProposalInfo::from(info);
        assert_eq!(api_info.proposal_id, "prop1");
        assert_eq!(
            api_info.confidence_vote.amount,
            Some("1000.50000000".to_string())
        );
        let serialized = serde_json::to_string(&api_info).unwrap();
        assert!(serialized.contains("\"proposalId\":\"prop1\""));
    }
}
