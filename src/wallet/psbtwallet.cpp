// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/psbtwallet.h>

TransactionError FillPSBT(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool& complete, int sighash_type, bool sign, bool bip32derivs)
{
    LOCK(pwallet->cs_wallet);
    if (!psbtx.tx) {
        return TransactionError::INVALID_PSBT;
    }
    // Get all of the previous transactions
    complete = true;
    for (unsigned int i = 0; i < psbtx.inputs.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // Verify input looks sane. This will check that we have at most one uxto, witness or non-witness.
        if (!input.IsSane()) {
            return TransactionError::INVALID_PSBT;
        }

        // If we have no utxo, grab it from the wallet.
        if (!input.non_witness_utxo && input.witness_utxo.IsNull()) {
            const uint256& txhash = txin.prevout.hash;
            if (auto wtx = pwallet->GetWalletTx(txhash)) {
                // We only need the non_witness_utxo, which is a superset of the witness_utxo.
                //   The signing code will switch to the smaller witness_utxo if this is ok.
                input.non_witness_utxo = wtx->tx;
                if (psbtx.GetVersion() >= 2) {
                    // A PSBTv2 does not contain the unsigned transaction, so the complete
                    // previous transaction is kept in the DFI specific proprietary record.
                    input.SetPreviousTx(*wtx->tx);
                }
            }
        }

        // Get the Sighash type
        if (sign && input.sighash_type > 0 && input.sighash_type != sighash_type) {
            return TransactionError::SIGHASH_MISMATCH;
        }

        complete &= SignPSBTInput(HidingSigningProvider(pwallet, !sign, !bip32derivs), psbtx, i, sighash_type);
    }

    // Fill in the bip32 keypaths and redeemscripts for the outputs so that hardware wallets can identify change
    for (unsigned int i = 0; i < psbtx.outputs.size(); ++i) {
        UpdatePSBTOutput(HidingSigningProvider(pwallet, true, !bip32derivs), psbtx, i);
    }

    return TransactionError::OK;
}
