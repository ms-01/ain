use bitcoin::{hashes::Hash, Txid};
use defichain_rpc::json::blockchain::{Transaction, Vin};

pub fn check_if_evm_tx(txn: &Transaction) -> bool {
    txn.vin.len() == 2
        && txn.vin.iter().all(|vin| match vin {
            Vin::Coinbase(_) => true,
            Vin::Standard(tx) => tx.txid == Txid::all_zeros(),
        })
        && txn.vout.len() == 1
        && txn.vout[0]
            .script_pub_key
            .asm
            .starts_with("OP_RETURN 4466547839")
        && txn.vout[0].value == 0f64
}

#[cfg(test)]
mod tests {
    use bitcoin::{hashes::Hash, Txid};
    use defichain_rpc::json::blockchain::{
        ScriptPubKey, ScriptSig, Transaction, Vin, VinCoinbase, VinStandard, Vout,
    };
    use serde_json::json;

    use super::check_if_evm_tx;

    fn make_script_sig() -> ScriptSig {
        serde_json::from_value(json!({ "asm": "", "hex": null })).unwrap()
    }

    fn make_evm_tx(vin: Vec<Vin>, asm: &str, value: f64) -> Transaction {
        let script_pub_key: ScriptPubKey = serde_json::from_value(json!({
            "asm": asm,
            "hex": "",
            "type": "nulldata"
        }))
        .unwrap();
        Transaction {
            txid: Txid::all_zeros(),
            hash: Txid::all_zeros().to_string(),
            version: 4,
            size: 100,
            vsize: 100,
            weight: 400,
            locktime: 0,
            vin,
            vout: vec![Vout {
                value,
                n: 0,
                script_pub_key,
                token_id: Some(0),
            }],
            hex: String::new(),
        }
    }

    fn zero_vin() -> Vin {
        Vin::Standard(VinStandard {
            txid: Txid::all_zeros(),
            vout: 0,
            script_sig: make_script_sig(),
            txinwitness: None,
            sequence: 0,
        })
    }

    #[test]
    fn test_check_if_evm_tx_valid() {
        let tx = make_evm_tx(
            vec![
                Vin::Coinbase(VinCoinbase {
                    coinbase: "00".to_string(),
                    sequence: 0,
                }),
                zero_vin(),
            ],
            "OP_RETURN 4466547839deadbeef",
            0.0,
        );
        assert!(check_if_evm_tx(&tx));
    }

    #[test]
    fn test_check_if_evm_tx_rejects_invalid_cases() {
        // Wrong vin count
        let tx_one_vin = make_evm_tx(vec![zero_vin()], "OP_RETURN 4466547839deadbeef", 0.0);
        assert!(!check_if_evm_tx(&tx_one_vin));

        // Non-zero txid in vin
        let nonzero_vin = Vin::Standard(VinStandard {
            txid: Txid::from_byte_array([1u8; 32]),
            vout: 0,
            script_sig: make_script_sig(),
            txinwitness: None,
            sequence: 0,
        });
        let tx_nonzero_vin = make_evm_tx(
            vec![zero_vin(), nonzero_vin],
            "OP_RETURN 4466547839deadbeef",
            0.0,
        );
        assert!(!check_if_evm_tx(&tx_nonzero_vin));

        // Wrong OP_RETURN prefix
        let tx_wrong_asm = make_evm_tx(vec![zero_vin(), zero_vin()], "OP_RETURN 1234", 0.0);
        assert!(!check_if_evm_tx(&tx_wrong_asm));

        // Non-zero value
        let tx_nonzero_val = make_evm_tx(
            vec![zero_vin(), zero_vin()],
            "OP_RETURN 4466547839deadbeef",
            1.0,
        );
        assert!(!check_if_evm_tx(&tx_nonzero_val));
    }
}
