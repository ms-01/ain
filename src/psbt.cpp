// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <psbt.h>
#include <util/strencodings.h>

#include <numeric>

PartiallySignedTransaction::PartiallySignedTransaction(const CMutableTransaction& tx) : tx(tx)
{
    inputs.resize(tx.vin.size());
    outputs.resize(tx.vout.size());
}

PartiallySignedTransaction::PartiallySignedTransaction(const CMutableTransaction& tx, uint32_t version) : tx(tx)
{
    if (version != 0 && version != 2) {
        throw std::ios_base::failure("Unsupported PSBT version number");
    }
    m_version = version;
    tx_version = tx.nVersion;
    fallback_locktime = tx.nLockTime;

    inputs.resize(tx.vin.size());
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        inputs[i].m_psbt_version = version;
        inputs[i].prev_txid = tx.vin[i].prevout.hash;
        inputs[i].prev_out = tx.vin[i].prevout.n;
        inputs[i].sequence = tx.vin[i].nSequence;
    }

    outputs.resize(tx.vout.size());
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        outputs[i].m_psbt_version = version;
        outputs[i].amount = tx.vout[i].nValue;
        outputs[i].script = tx.vout[i].scriptPubKey;
        outputs[i].SetTokenId(tx.vout[i].nTokenId);
    }
}

void PartiallySignedTransaction::SetVersion(uint32_t version)
{
    if (version != 0 && version != 2) {
        throw std::ios_base::failure("Unsupported PSBT version number");
    }
    m_version = version;
    for (PSBTInput& input : inputs) {
        input.m_psbt_version = version;
    }
    for (PSBTOutput& output : outputs) {
        output.m_psbt_version = version;
    }
}

std::optional<uint32_t> PartiallySignedTransaction::ComputeTimeLock() const
{
    if (GetVersion() >= 2) {
        std::optional<uint32_t> time_lock{0};
        std::optional<uint32_t> height_lock{0};
        for (const PSBTInput& input : inputs) {
            if (input.time_locktime.has_value() && !input.height_locktime.has_value()) {
                height_lock.reset(); // Transaction can no longer have a height locktime
                if (!time_lock.has_value()) {
                    return std::nullopt;
                }
            } else if (!input.time_locktime.has_value() && input.height_locktime.has_value()) {
                time_lock.reset(); // Transaction can no longer have a time locktime
                if (!height_lock.has_value()) {
                    return std::nullopt;
                }
            }
            if (input.time_locktime && time_lock.has_value()) {
                time_lock = std::max(time_lock, input.time_locktime);
            }
            if (input.height_locktime && height_lock.has_value()) {
                height_lock = std::max(height_lock, input.height_locktime);
            }
        }
        if (height_lock.has_value() && *height_lock > 0) {
            return *height_lock;
        }
        if (time_lock.has_value() && *time_lock > 0) {
            return *time_lock;
        }
    }
    return fallback_locktime.value_or(0);
}

std::optional<CMutableTransaction> PartiallySignedTransaction::GetUnsignedTx() const
{
    if (GetVersion() < 2) {
        return tx;
    }

    if (tx_version > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }

    CMutableTransaction mtx;
    mtx.nVersion = static_cast<int32_t>(tx_version);
    std::optional<uint32_t> locktime = ComputeTimeLock();
    if (!locktime) {
        return std::nullopt;
    }
    mtx.nLockTime = *locktime;

    const uint32_t max_sequence = CTxIn::SEQUENCE_FINAL;
    for (const PSBTInput& input : inputs) {
        CTxIn txin;
        txin.prevout.hash = input.prev_txid;
        txin.prevout.n = input.prev_out;
        txin.nSequence = input.sequence.value_or(max_sequence);
        mtx.vin.push_back(txin);
    }
    for (const PSBTOutput& output : outputs) {
        CTxOut txout;
        txout.nValue = output.amount;
        txout.scriptPubKey = output.script;
        txout.nTokenId = output.GetTokenId();
        mtx.vout.push_back(txout);
    }
    return mtx;
}

bool PartiallySignedTransaction::IsNull() const
{
    return !tx && inputs.empty() && outputs.empty() && unknown.empty() && proprietary.empty();
}

bool PartiallySignedTransaction::Merge(const PartiallySignedTransaction& psbt)
{
    // Prohibited to merge two PSBTs of different versions
    if (GetVersion() != psbt.GetVersion()) {
        return false;
    }

    // Prohibited to merge two PSBTs over different transactions
    if (!tx || !psbt.tx || tx->GetHash() != psbt.tx->GetHash()) {
        return false;
    }

    for (unsigned int i = 0; i < inputs.size(); ++i) {
        inputs[i].Merge(psbt.inputs[i]);
    }
    for (unsigned int i = 0; i < outputs.size(); ++i) {
        outputs[i].Merge(psbt.outputs[i]);
    }
    unknown.insert(psbt.unknown.begin(), psbt.unknown.end());
    proprietary.insert(psbt.proprietary.begin(), psbt.proprietary.end());
    for (const PSBTXPub& xpub : psbt.m_xpubs) {
        if (std::find(m_xpubs.begin(), m_xpubs.end(), xpub) == m_xpubs.end()) {
            m_xpubs.push_back(xpub);
        }
    }

    return true;
}

bool PartiallySignedTransaction::IsSane() const
{
    // A PSBT must contain the transaction it signs
    if (!tx) {
        return false;
    }
    for (const PSBTInput& input : inputs) {
        if (!input.IsSane()) return false;
    }
    return true;
}

bool PartiallySignedTransaction::AddInput(const CTxIn& txin, PSBTInput& psbtin)
{
    if (!tx) {
        return false;
    }
    if (std::find(tx->vin.begin(), tx->vin.end(), txin) != tx->vin.end()) {
        return false;
    }
    tx->vin.push_back(txin);
    psbtin.partial_sigs.clear();
    psbtin.final_script_sig.clear();
    psbtin.final_script_witness.SetNull();
    psbtin.m_psbt_version = GetVersion();
    psbtin.prev_txid = txin.prevout.hash;
    psbtin.prev_out = txin.prevout.n;
    psbtin.sequence = txin.nSequence;
    inputs.push_back(psbtin);
    return true;
}

bool PartiallySignedTransaction::AddOutput(const CTxOut& txout, const PSBTOutput& psbtout)
{
    if (!tx) {
        return false;
    }
    tx->vout.push_back(txout);
    PSBTOutput output = psbtout;
    output.m_psbt_version = GetVersion();
    output.amount = txout.nValue;
    output.script = txout.scriptPubKey;
    output.SetTokenId(txout.nTokenId);
    outputs.push_back(output);
    return true;
}

bool PartiallySignedTransaction::GetInputUTXO(CTxOut& utxo, int input_index) const
{
    if (!tx || input_index < 0 || (size_t)input_index >= inputs.size()) {
        return false;
    }
    const PSBTInput& input = inputs[input_index];
    const uint32_t prevout_index = GetVersion() >= 2 ? input.prev_out : tx->vin[input_index].prevout.n;
    if (input.non_witness_utxo) {
        if (prevout_index >= input.non_witness_utxo->vout.size()) {
            return false;
        }
        utxo = input.non_witness_utxo->vout[prevout_index];
    } else if (!input.witness_utxo.IsNull()) {
        utxo = input.witness_utxo; /// @todo tokens: extend with correct txout version (and/or tokenid) when implemented
    } else {
        return false;
    }
    return true;
}

bool PSBTInput::IsNull() const
{
    return !non_witness_utxo && witness_utxo.IsNull() && partial_sigs.empty() && unknown.empty() && hd_keypaths.empty() && redeem_script.empty() && witness_script.empty() && proprietary.empty();
}

std::optional<CMutableTransaction> PSBTInput::GetPreviousTx() const
{
    const auto key = MakePSBTProprietaryKey(PSBT_PROPRIETARY_SUBTYPE_PREVIOUS_TX, PSBT_PROPRIETARY_IDENTIFIER);
    const auto it = proprietary.find(key);
    if (it == proprietary.end()) {
        return std::nullopt;
    }
    CDataStream ss(it->second, SER_NETWORK, PROTOCOL_VERSION);
    CMutableTransaction prev_tx;
    ss >> prev_tx;
    return prev_tx;
}

void PSBTInput::SetPreviousTx(const CTransaction& prev_tx)
{
    if (m_psbt_version < 2) {
        // The DFI specific records are only used in a PSBTv2, in a PSBTv0 the complete
        // previous transaction is stored in the non-witness utxo.
        return;
    }
    const auto key = MakePSBTProprietaryKey(PSBT_PROPRIETARY_SUBTYPE_PREVIOUS_TX, PSBT_PROPRIETARY_IDENTIFIER);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    ss << prev_tx;
    proprietary[key] = std::vector<unsigned char>(ss.begin(), ss.end());
}

void PSBTInput::FillSignatureData(SignatureData& sigdata) const
{
    if (!final_script_sig.empty()) {
        sigdata.scriptSig = final_script_sig;
        sigdata.complete = true;
    }
    if (!final_script_witness.IsNull()) {
        sigdata.scriptWitness = final_script_witness;
        sigdata.complete = true;
    }
    if (sigdata.complete) {
        return;
    }

    sigdata.signatures.insert(partial_sigs.begin(), partial_sigs.end());
    if (!redeem_script.empty()) {
        sigdata.redeem_script = redeem_script;
    }
    if (!witness_script.empty()) {
        sigdata.witness_script = witness_script;
    }
    for (const auto& key_pair : hd_keypaths) {
        sigdata.misc_pubkeys.emplace(key_pair.first.GetID(), key_pair);
    }
}

void PSBTInput::FromSignatureData(const SignatureData& sigdata)
{
    if (sigdata.complete) {
        partial_sigs.clear();
        hd_keypaths.clear();
        redeem_script.clear();
        witness_script.clear();

        if (!sigdata.scriptSig.empty()) {
            final_script_sig = sigdata.scriptSig;
        }
        if (!sigdata.scriptWitness.IsNull()) {
            final_script_witness = sigdata.scriptWitness;
        }
        return;
    }

    partial_sigs.insert(sigdata.signatures.begin(), sigdata.signatures.end());
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
    if (witness_script.empty() && !sigdata.witness_script.empty()) {
        witness_script = sigdata.witness_script;
    }
    for (const auto& entry : sigdata.misc_pubkeys) {
        hd_keypaths.emplace(entry.second);
    }
}

void PSBTInput::Merge(const PSBTInput& input)
{
    if (!non_witness_utxo && input.non_witness_utxo) non_witness_utxo = input.non_witness_utxo;
    if (witness_utxo.IsNull() && !input.witness_utxo.IsNull()) {
        witness_utxo = input.witness_utxo; /// @todo tokens: extend with correct txout version (and/or tokenid) when implemented
        non_witness_utxo = nullptr; // Clear out any non-witness utxo when we set a witness one.
    }

    partial_sigs.insert(input.partial_sigs.begin(), input.partial_sigs.end());
    hd_keypaths.insert(input.hd_keypaths.begin(), input.hd_keypaths.end());
    unknown.insert(input.unknown.begin(), input.unknown.end());
    proprietary.insert(input.proprietary.begin(), input.proprietary.end());

    if (!sequence && input.sequence) sequence = input.sequence;
    if (!time_locktime && input.time_locktime) time_locktime = input.time_locktime;
    if (!height_locktime && input.height_locktime) height_locktime = input.height_locktime;

    if (redeem_script.empty() && !input.redeem_script.empty()) redeem_script = input.redeem_script;
    if (witness_script.empty() && !input.witness_script.empty()) witness_script = input.witness_script;
    if (final_script_sig.empty() && !input.final_script_sig.empty()) final_script_sig = input.final_script_sig;
    if (final_script_witness.IsNull() && !input.final_script_witness.IsNull()) final_script_witness = input.final_script_witness;
}

bool PSBTInput::IsSane() const
{
    // Cannot have both witness and non-witness utxos
    if (!witness_utxo.IsNull() && non_witness_utxo) return false;

    // If we have a witness_script or a scriptWitness, we must also have a witness utxo
    if (!witness_script.empty() && witness_utxo.IsNull()) return false;
    if (!final_script_witness.IsNull() && witness_utxo.IsNull()) return false;

    return true;
}

void PSBTOutput::FillSignatureData(SignatureData& sigdata) const
{
    if (!redeem_script.empty()) {
        sigdata.redeem_script = redeem_script;
    }
    if (!witness_script.empty()) {
        sigdata.witness_script = witness_script;
    }
    for (const auto& key_pair : hd_keypaths) {
        sigdata.misc_pubkeys.emplace(key_pair.first.GetID(), key_pair);
    }
}

void PSBTOutput::FromSignatureData(const SignatureData& sigdata)
{
    if (redeem_script.empty() && !sigdata.redeem_script.empty()) {
        redeem_script = sigdata.redeem_script;
    }
    if (witness_script.empty() && !sigdata.witness_script.empty()) {
        witness_script = sigdata.witness_script;
    }
    for (const auto& entry : sigdata.misc_pubkeys) {
        hd_keypaths.emplace(entry.second);
    }
}

bool PSBTOutput::IsNull() const
{
    return redeem_script.empty() && witness_script.empty() && hd_keypaths.empty() && unknown.empty() && proprietary.empty();
}

void PSBTOutput::Merge(const PSBTOutput& output)
{
    hd_keypaths.insert(output.hd_keypaths.begin(), output.hd_keypaths.end());
    unknown.insert(output.unknown.begin(), output.unknown.end());
    proprietary.insert(output.proprietary.begin(), output.proprietary.end());

    if (redeem_script.empty() && !output.redeem_script.empty()) redeem_script = output.redeem_script;
    if (witness_script.empty() && !output.witness_script.empty()) witness_script = output.witness_script;
}

DCT_ID PSBTOutput::GetTokenId() const
{
    const auto key = MakePSBTProprietaryKey(PSBT_PROPRIETARY_SUBTYPE_OUTPUT_TOKEN_ID, PSBT_PROPRIETARY_IDENTIFIER);
    const auto it = proprietary.find(key);
    if (it == proprietary.end()) {
        return DCT_ID{0};
    }
    CDataStream ss(it->second, SER_NETWORK, PROTOCOL_VERSION);
    uint32_t token_id = 0;
    ss >> VARINT(token_id);
    return DCT_ID{token_id};
}

void PSBTOutput::SetTokenId(const DCT_ID& id)
{
    token_id = id;
    const auto key = MakePSBTProprietaryKey(PSBT_PROPRIETARY_SUBTYPE_OUTPUT_TOKEN_ID, PSBT_PROPRIETARY_IDENTIFIER);
    if (id.v == 0) {
        // A output without a token does not need a record
        proprietary.erase(key);
        return;
    }
    if (m_psbt_version < 2) {
        // The DFI specific records are only used in a PSBTv2
        return;
    }
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    uint32_t token_id_value = id.v;
    ss << VARINT(token_id_value);
    proprietary[key] = std::vector<unsigned char>(ss.begin(), ss.end());
}

bool PSBTInputSigned(const PSBTInput& input)
{
    return !input.final_script_sig.empty() || !input.final_script_witness.IsNull();
}

void UpdatePSBTOutput(const SigningProvider& provider, PartiallySignedTransaction& psbt, int index)
{
    if (!psbt.tx) {
        return;
    }
    if (index < 0 || (size_t)index >= psbt.outputs.size()) {
        return;
    }

    CMutableTransaction& tx = *psbt.tx;
    const CTxOut& out = tx.vout.at(index);
    PSBTOutput& psbt_out = psbt.outputs.at(index);

    // Fill a SignatureData with output info
    SignatureData sigdata;
    psbt_out.FillSignatureData(sigdata);

    // Construct a would-be spend of this output, to update sigdata with.
    // Note that ProduceSignature is used to fill in metadata (not actual signatures),
    // so provider does not need to provide any private keys (it can be a HidingSigningProvider).
    MutableTransactionSignatureCreator creator(&tx, /* index */ 0, out.nValue, SIGHASH_ALL);
    ProduceSignature(provider, creator, out.scriptPubKey, sigdata);

    // Put redeem_script, witness_script, key paths, into PSBTOutput.
    psbt_out.FromSignatureData(sigdata);
}

bool SignPSBTInput(const SigningProvider& provider, PartiallySignedTransaction& psbt, int index, int sighash, SignatureData* out_sigdata, bool use_dummy)
{
    if (!psbt.tx) {
        return false;
    }
    if (index < 0 || (size_t)index >= psbt.inputs.size()) {
        return false;
    }

    PSBTInput& input = psbt.inputs.at(index);
    const CMutableTransaction& tx = *psbt.tx;

    if (PSBTInputSigned(input)) {
        return true;
    }

    // Fill SignatureData with input info
    SignatureData sigdata;
    input.FillSignatureData(sigdata);

    // Get UTXO
    bool require_witness_sig = false;
    CTxOut utxo;

    // Verify input sanity, which checks that at most one of witness or non-witness utxos is provided.
    if (!input.IsSane()) {
        return false;
    }

    if (input.non_witness_utxo) {
        // If we're taking our information from a non-witness UTXO, verify that it matches the prevout.
        COutPoint prevout = tx.vin[index].prevout;
        if (input.non_witness_utxo->GetHash() != prevout.hash) {
            return false;
        }
        if (prevout.n >= input.non_witness_utxo->vout.size()) {
            return false;
        }
        utxo = input.non_witness_utxo->vout[prevout.n];
    } else if (!input.witness_utxo.IsNull()) {
        utxo = input.witness_utxo; /// @todo tokens: extend with correct txout version (and/or tokenid) when implemented
        // When we're taking our information from a witness UTXO, we can't verify it is actually data from
        // the output being spent. This is safe in case a witness signature is produced (which includes this
        // information directly in the hash), but not for non-witness signatures. Remember that we require
        // a witness signature in this situation.
        require_witness_sig = true;
    } else {
        return false;
    }

    sigdata.witness = false;
    bool sig_complete;
    if (use_dummy) {
        sig_complete = ProduceSignature(provider, DUMMY_SIGNATURE_CREATOR, utxo.scriptPubKey, sigdata);
    } else {
        MutableTransactionSignatureCreator creator(&tx, index, utxo.nValue, sighash);
        sig_complete = ProduceSignature(provider, creator, utxo.scriptPubKey, sigdata);
    }
    // Verify that a witness signature was produced in case one was required.
    if (require_witness_sig && !sigdata.witness) return false;
    input.FromSignatureData(sigdata);

    // If we have a witness signature, use the smaller witness UTXO.
    if (sigdata.witness) {
        input.witness_utxo = utxo; /// @todo tokens: extend with correct txout version (and/or tokenid) when implemented
        input.non_witness_utxo = nullptr;
    }

    // Fill in the missing info
    if (out_sigdata) {
        out_sigdata->missing_pubkeys = sigdata.missing_pubkeys;
        out_sigdata->missing_sigs = sigdata.missing_sigs;
        out_sigdata->missing_redeem_script = sigdata.missing_redeem_script;
        out_sigdata->missing_witness_script = sigdata.missing_witness_script;
    }

    return sig_complete;
}

bool FinalizePSBT(PartiallySignedTransaction& psbtx)
{
    if (!psbtx.tx) {
        return false;
    }
    // Finalize input signatures -- in case we have partial signatures that add up to a complete
    //   signature, but have not combined them yet (e.g. because the combiner that created this
    //   PartiallySignedTransaction did not understand them), this will combine them into a final
    //   script.
    bool complete = true;
    for (unsigned int i = 0; i < psbtx.inputs.size(); ++i) {
        complete &= SignPSBTInput(DUMMY_SIGNING_PROVIDER, psbtx, i, SIGHASH_ALL);
    }

    return complete;
}

bool FinalizeAndExtractPSBT(PartiallySignedTransaction& psbtx, CMutableTransaction& result)
{
    if (!psbtx.tx) {
        return false;
    }
    // It's not safe to extract a PSBT that isn't finalized, and there's no easy way to check
    //   whether a PSBT is finalized without finalizing it, so we just do this.
    if (!FinalizePSBT(psbtx)) {
        return false;
    }

    result = *psbtx.tx;
    for (unsigned int i = 0; i < result.vin.size(); ++i) {
        result.vin[i].scriptSig = psbtx.inputs[i].final_script_sig;
        result.vin[i].scriptWitness = psbtx.inputs[i].final_script_witness;
    }
    return true;
}

TransactionError CombinePSBTs(PartiallySignedTransaction& out, const std::vector<PartiallySignedTransaction>& psbtxs)
{
    if (psbtxs.empty()) {
        return TransactionError::INVALID_PSBT;
    }

    out = psbtxs[0]; // Copy the first one

    // Merge
    for (auto it = std::next(psbtxs.begin()); it != psbtxs.end(); ++it) {
        if (!out.Merge(*it)) {
            return TransactionError::PSBT_MISMATCH;
        }
    }
    if (!out.IsSane()) {
        return TransactionError::INVALID_PSBT;
    }

    return TransactionError::OK;
}

std::string PSBTRoleName(PSBTRole role) {
    switch (role) {
    case PSBTRole::UPDATER: return "updater";
    case PSBTRole::SIGNER: return "signer";
    case PSBTRole::FINALIZER: return "finalizer";
    case PSBTRole::EXTRACTOR: return "extractor";
        // no default case, so the compiler can warn about missing cases
    }
    assert(false);
}

bool DecodeBase64PSBT(PartiallySignedTransaction& psbt, const std::string& base64_tx, std::string& error)
{
    bool invalid;
    std::string tx_data = DecodeBase64(base64_tx, &invalid);
    if (invalid) {
        error = "invalid base64";
        return false;
    }
    return DecodeRawPSBT(psbt, tx_data, error);
}

bool DecodeRawPSBT(PartiallySignedTransaction& psbt, const std::string& tx_data, std::string& error)
{
    CDataStream ss_data(tx_data.data(), tx_data.data() + tx_data.size(), SER_NETWORK, PROTOCOL_VERSION);
    try {
        ss_data >> psbt;
        if (!ss_data.empty()) {
            error = "extra data after PSBT";
            return false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}
