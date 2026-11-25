// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <external_signer.h>
#include <init.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <dfi/tokens.h>
#include <node/transaction.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/keyorigin.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <spv/spv_wrapper.h>
#include <util/bip32.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/url.h>
#include <util/validation.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/ismine.h>
#include <wallet/psbtwallet.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
#include <wallet/migrate.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <stdint.h>

#include <univalue.h>

#include <functional>

#include <dfi/mn_checks.h>

static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

static inline bool GetAvoidReuseFlag(CWallet * const pwallet, const UniValue& param) {
    bool can_avoid_reuse = pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);
    bool avoid_reuse = param.isNull() ? can_avoid_reuse : param.get_bool();

    if (avoid_reuse && !can_avoid_reuse) {
        throw JSONRPCError(RPC_WALLET_ERROR, "wallet does not have the \"avoid reuse\" feature enabled");
    }

    return avoid_reuse;
}


/** Used by RPC commands that have an include_watchonly parameter.
 *  We default to true for watchonly wallets if include_watchonly isn't
 *  explicitly set.
 */
static bool ParseIncludeWatchonly(const UniValue& include_watchonly, const CWallet& pwallet)
{
    if (include_watchonly.isNull()) {
        // if include_watchonly isn't explicitly set, then check if we have a watchonly wallet
        return pwallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    }

    // otherwise return whatever include_watchonly was set to
    return include_watchonly.get_bool();
}


/** Checks if a CKey is in the given CWallet compressed or otherwise*/
bool HaveKey(const CWallet& wallet, const CKey& key)
{
    CKey key2;
    key2.Set(key.begin(), key.end(), !key.IsCompressed());
    return wallet.HaveKey(key.GetPubKey().GetID()) || wallet.HaveKey(key2.GetPubKey().GetID());
}

bool GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& wallet_name)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        wallet_name = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        return true;
    }
    return false;
}

std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        std::shared_ptr<CWallet> pwallet = GetWallet(wallet_name);
        if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
        return pwallet;
    }

    std::vector<std::shared_ptr<CWallet>> wallets = GetWallets();
    return wallets.size() == 1 || (request.fHelp && wallets.size() > 0) ? wallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(const CWallet* pwallet)
{
    return pwallet && pwallet->IsCrypted()
        ? "\nRequires wallet passphrase to be set with walletpassphrase call."
        : "";
}

bool EnsureWalletIsAvailable(const CWallet* pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (!HasWallets()) {
        throw JSONRPCError(
            RPC_METHOD_NOT_FOUND, "Method not found (wallet method is disabled because no wallet is loaded)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(const CWallet* pwallet)
{
    if (pwallet->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
}

int64_t GetImportTimestamp(const UniValue& data, int64_t now)
{
    if (data.exists("timestamp")) {
        const UniValue& timestamp = data["timestamp"];
        if (timestamp.isNum()) {
            return timestamp.getInt<int64_t>();
        } else if (timestamp.isStr() && timestamp.get_str() == "now") {
            return now;
        }
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(timestamp.type())));
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
}

static void WalletTxToJSON(interfaces::Chain& chain, interfaces::Chain::Lock& locked_chain, const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain(locked_chain);
    entry.pushKV("confirmations", confirms);
    if (wtx.IsCoinBase())
        entry.pushKV("generated", true);
    if (confirms > 0)
    {
        entry.pushKV("blockhash", wtx.hashBlock.GetHex());
        entry.pushKV("blockindex", wtx.nIndex);
        int64_t block_time;
        bool found_block = chain.findBlock(wtx.hashBlock, nullptr /* block */, &block_time);
        assert(found_block);
        entry.pushKV("blocktime", block_time);
    } else {
        entry.pushKV("trusted", wtx.IsTrusted(locked_chain));
    }
    uint256 hash = wtx.GetHash();
    entry.pushKV("txid", hash.GetHex());
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.pushKV("walletconflicts", conflicts);
    entry.pushKV("time", wtx.GetTxTime());
    entry.pushKV("timereceived", (int64_t)wtx.nTimeReceived);

    // Add opt-in RBF status
    std::string rbfStatus = "no";
    if (confirms <= 0) {
        RBFTransactionState rbfState = chain.isRBFOptIn(*wtx.tx);
        if (rbfState == RBFTransactionState::UNKNOWN)
            rbfStatus = "unknown";
        else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125)
            rbfStatus = "yes";
    }
    entry.pushKV("bip125-replaceable", rbfStatus);

    for (const std::pair<const std::string, std::string>& item : wtx.mapValue)
        entry.pushKV(item.first, item.second);
}

static std::string LabelFromValue(const UniValue& value)
{
    std::string label = value.get_str();
    if (label == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

static UniValue TokensToJSON(const TAmounts & amounts, bool with_tokens)
{
    if (!with_tokens) {
        return ValueFromAmount(amounts.find(DCT_ID{0}) == amounts.end() ? 0 : amounts.at(DCT_ID{0}));
    }

    UniValue result(UniValue::VOBJ);
    for (auto pair : amounts) {
        result.pushKV(pair.first.ToString(), ValueFromAmount(pair.second));
    }
    return result;
}

static UniValue getnewaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getnewaddress",
                "\nReturns a new Defi address for receiving payments.\n"
                "If 'label' is specified, it is added to the address book \n"
                "so payments received with the address will be associated with 'label'.\n",
                {
                    {"label", RPCArg::Type::STR, /* default */ "\"\"", "The label name for the address to be linked to. It can also be set to the empty string \"\" to represent the default label. The label does not need to exist, it will be created if there is no label by the given name."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", R"(The address type to use. Options are "legacy", "p2sh-segwit", "bech32" and "erc55".)"},
                },
                RPCResult{
            "\"address\"    (string) The new defi address\n"
                },
                RPCExamples{
                    HelpExampleCli("getnewaddress", "")
            + HelpExampleRpc("getnewaddress", "")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    // Parse the label first so we don't generate a key if there's an error
    std::string label;
    if (!request.params[0].isNull())
        label = LabelFromValue(request.params[0]);

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[1].isNull()) {
        if (!ParseOutputType(request.params[1].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[1].get_str()));
        }
    }

    CTxDestination dest;
    std::string error;
    if (!pwallet->GetNewDestination(output_type, label, dest, error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
    }

    return EncodeDestination(dest);
}

static UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getrawchangeaddress",
                "\nReturns a new Defi address, for receiving change.\n"
                "This is for use with raw transactions, NOT normal use.\n",
                {
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
            "\"address\"    (string) The address\n"
                },
                RPCExamples{
                    HelpExampleCli("getrawchangeaddress", "")
            + HelpExampleRpc("getrawchangeaddress", "")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses(true)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    OutputType output_type = pwallet->m_default_change_type != OutputType::CHANGE_AUTO ? pwallet->m_default_change_type : pwallet->m_default_address_type;
    if (!request.params[0].isNull()) {
        if (!ParseOutputType(request.params[0].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[0].get_str()));
        }
    }

    CTxDestination dest;
    std::string error;
    if (!pwallet->GetNewChangeDestination(output_type, dest, error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
    }
    return EncodeDestination(dest);
}


static UniValue setlabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"setlabel",
                "\nSets the label associated with the given address.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The defi address to be associated with a label."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label to assign to the address."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("setlabel", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"tabby\"")
            + HelpExampleRpc("setlabel", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"tabby\"")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Defi address");
    }

    std::string label = LabelFromValue(request.params[1]);

    if (IsMineCached(*pwallet, dest)) {
        pwallet->SetAddressBook(dest, label, "receive");
    } else {
        pwallet->SetAddressBook(dest, label, "send");
    }

    return NullUniValue;
}


CTransactionRef SendMoney(interfaces::Chain::Lock& locked_chain, CWallet * const pwallet, const CTxDestination &address, CAmount nValue, DCT_ID tokenId, bool fSubtractFeeFromAmount, const CCoinControl& coin_control, mapValue_t mapValue, const CoinSelectionOptions &coinSelectOpts)
{
    TAmounts curBalance = pwallet->GetBalance(0, coin_control.m_avoid_address_reuse).m_mine_trusted;

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > curBalance[tokenId])
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Parse Defi address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, tokenId, tokenId == DCT_ID{0} ? fSubtractFeeFromAmount : false }; // turn off `fSubtractFeeFromAmount` for tokens
    vecSend.push_back(recipient);
    CTransactionRef tx;
    if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, nFeeRequired, nChangePosRet, strError, coin_control, true, coinSelectOpts)) {
        if (!fSubtractFeeFromAmount && (tokenId == DCT_ID{0} ? nValue : 0) + nFeeRequired > curBalance[DCT_ID{0}])
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, std::move(mapValue), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return tx;
}

static UniValue sendtoaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"sendtoaddress",
                "\nSend an amount to a given address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The defi address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment used to store what the transaction is for.\n"
            "                             This is not part of the transaction, just kept in your wallet."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment to store the name of the person or organization\n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet."},
                    {"subtractfeefromamount", RPCArg::Type::BOOL, /* default */ "false", "The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less defis than you enter in the amount field."},
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\""},
                    {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "true", "(only available if avoid_reuse wallet flag is set) Avoid spending from dirty addresses; addresses are considered\n"
            "                             dirty if they have previously been used in a transaction."},
                },
                RPCResult{
            "\"hash\"                  (string) The hex-encoded hash of broadcasted transaction\n"
                },
                RPCExamples{
                    HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.1, \"donation\", \"seans outpost\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    RejectErc55Address(GetScriptForDestination(dest));

    // Amount
    CTokenAmount const tokenAmount = DecodeAmount(pwallet->chain(), request.params[1], request.params[0].get_str()); // don't support multiple tokens due to "SendMoney()" compatibility

    // Wallet comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull() && tokenAmount.nTokenId == DCT_ID{0}) { // ignore fSubtractFeeFromAmount for tokens
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6], pwallet->chain().estimateMaxBlocks());
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    coin_control.m_avoid_address_reuse = GetAvoidReuseFlag(pwallet, request.params[8]);
    // We also enable partial spend avoidance if reuse avoidance is set.
    coin_control.m_avoid_partial_spends |= coin_control.m_avoid_address_reuse;

    EnsureWalletIsUnlocked(pwallet);

    CTransactionRef tx = SendMoney(*locked_chain, pwallet, dest, tokenAmount.nValue, tokenAmount.nTokenId, fSubtractFeeFromAmount, coin_control, std::move(mapValue), request.metadata.coinSelectOpts);
    return tx->GetHash().GetHex();
}

static UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listaddressgroupings",
                "\nLists groups of addresses which have had their common ownership\n"
                "made public by common use as inputs or as the resulting change\n"
                "in past transactions\n",
                {},
                RPCResult{
            "[\n"
            "  [\n"
            "    [\n"
            "      \"address\",            (string) The defi address\n"
            "      amount,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"label\"               (string, optional) The label\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listaddressgroupings", "")
            + HelpExampleRpc("listaddressgroupings", "")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);
    for (const std::set<CTxDestination>& grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (const CTxDestination& address : grouping)
        {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwallet->mapAddressBook.find(address) != pwallet->mapAddressBook.end()) {
                    addressInfo.push_back(pwallet->mapAddressBook.find(address)->second.name);
                }
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

static UniValue signmessage(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"signmessage",
                "\nSign a message with the private key of an address" +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The defi address to use for the private key."},
                    {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message to create a signature of."},
                },
                RPCResult{
            "\"signature\"          (string) The signature of the message encoded in base 64\n"
                },
                RPCExamples{
            "\nUnlock the wallet for 30 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"my message\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const PKHash *pkhash = std::get_if<PKHash>(&dest);
    if (!pkhash) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CKey key;
    CKeyID keyID(*pkhash);
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(vchSig.data(), vchSig.size());
}

static UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getreceivedbyaddress",
                "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The defi address for transactions."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
            "amount   (numeric) The total amount in " + CURRENCY_UNIT + " received at this address.\n"
                },
                RPCExamples{
            "\nThe amount from transactions with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    // Defi address
    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Defi address");
    }
    CScript scriptPubKey = GetScriptForDestination(dest);
    if (!IsMineCached(*pwallet, scriptPubKey)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");
    }

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].getInt<int>();

    // Tally
    CAmount nAmount = 0;
    for (const auto& wtx : pwallet->mapWallet.get<ByHash>()) {
        if (wtx.IsCoinBase() || !locked_chain->checkFinalTx(*wtx.tx)) {
            continue;
        }

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain(*locked_chain) >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


static UniValue getreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getreceivedbylabel",
                "\nReturns the total amount received by addresses with <label> in transactions with at least [minconf] confirmations.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The selected label, may be the default label using \"\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this label.\n"
                },
                RPCExamples{
            "\nAmount received by the default label with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbylabel", "\"\"") +
            "\nAmount received at the tabby label including unconfirmed amounts with zero confirmations\n"
            + HelpExampleCli("getreceivedbylabel", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbylabel", "\"tabby\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getreceivedbylabel", "\"tabby\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].getInt<int>();

    // Get the set of pub keys assigned to label
    std::string label = LabelFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetLabelAddresses(label);

    // Tally
    CAmount nAmount = 0;
    for (const auto& wtx : pwallet->mapWallet.get<ByHash>()) {
        if (wtx.IsCoinBase() || !locked_chain->checkFinalTx(*wtx.tx)) {
            continue;
        }

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMineCached(*pwallet, address) && setAddress.count(address)) {
                if (wtx.GetDepthInMainChain(*locked_chain) >= nMinDepth)
                    nAmount += txout.nValue;
            }
        }
    }

    return ValueFromAmount(nAmount);
}

inline void Increment(TAmounts & accum, TAmounts const & amounts) {
    for (auto const & pair : amounts) {
        accum[pair.first] += pair.second;
    }
}

inline void Decrement(TAmounts & accum, TAmounts const & amounts) {
    for (auto const & pair : amounts) {
        accum[pair.first] -= pair.second;
    }
}

static UniValue getbalance(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getbalance",
                "\nReturns the total available balance.\n"
                "The available balance is what the wallet considers currently spendable, and is\n"
                "thus affected by options which limit spendability such as -spendzeroconfchange.\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Remains for backward compatibility. Must be excluded or set to \"*\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "0", "Only include transactions confirmed at least this many times."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also include balance in watch-only addresses (see 'importaddress')"},
                    {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "true", "(only available if avoid_reuse wallet flag is set) Do not include balance in dirty outputs; addresses are considered dirty if they have previously been used in a transaction."},
                    {"with_tokens", RPCArg::Type::BOOL, /* default */ "false", "Include tokens balances; Default is 'false' for backward compatibility."},
                },
                RPCResult{
            "amount                 (numeric) The total amount in " + CURRENCY_UNIT + " received for this wallet.\n"
            "   or\n"
            "{tokenId: amount,...}  (list) If used 'with_tokens' option\n"
                },
                RPCExamples{
            "\nThe total amount in the wallet with 1 or more confirmations\n"
            + HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet at least 6 blocks confirmed\n"
            + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getbalance", "\"*\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    const UniValue& dummy_value = request.params[0];
    if (!dummy_value.isNull() && dummy_value.get_str() != "*") {
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "dummy first argument must be excluded or set to \"*\".");
    }

    int min_depth = 0;
    if (!request.params[1].isNull()) {
        min_depth = request.params[1].getInt<int>();
    }

    bool include_watchonly = ParseIncludeWatchonly(request.params[2], *pwallet);
    bool avoid_reuse = GetAvoidReuseFlag(pwallet, request.params[3]);
    bool with_tokens = request.params[4].isNull() ? false : request.params[4].get_bool();

    auto bal = pwallet->GetBalance(min_depth, avoid_reuse);
    if (include_watchonly) {
        Increment(bal.m_mine_trusted, bal.m_watchonly_trusted); // `mine` and `watchonly` may not intersect at all by token_ids; m_mine_trusted is "spoiled" after that but keys merged
    }
    return TokensToJSON(bal.m_mine_trusted, with_tokens); // possible updated by "watchonly"
}

static UniValue getunconfirmedbalance(const JSONRPCRequest &request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getunconfirmedbalance",
                "DEPRECATED\nIdentical to getbalances().mine.untrusted_pending\n",
                {
                    {"with_tokens", RPCArg::Type::BOOL, /* default */ "false", "Include tokens balances; Default is 'false' for backward compatibility."},
                },
                RPCResults{},
                RPCExamples{""},
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    bool with_tokens = request.params[0].isNull() ? false : request.params[0].get_bool();

    return TokensToJSON(pwallet->GetBalance().m_mine_untrusted_pending, with_tokens);
}


static UniValue sendmany(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"sendmany",
                "\nSend multiple times. Amounts are double-precision floating point numbers." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::NO, "Must be set to \"\" for backwards compatibility.", "\"\""},
                    {"amounts", RPCArg::Type::OBJ, RPCArg::Optional::NO, "A json object with addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The defi address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "Ignored dummy value"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {"subtractfeefrom", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array with addresses.\n"
            "                           The fee will be equally deducted from the amount of each selected address.\n"
            "                           Those recipients will receive less defis than you enter in their corresponding amount field.\n"
            "                           If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Subtract fee from this address"},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\""},
                },
                 RPCResult{
            "\"txid\"                   (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"
                 },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 1 \"\" \"[\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\":0.01,\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, 6, \"testing\"")
                },
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    UniValue sendTo = request.params[1].get_obj();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6], pwallet->chain().estimateMaxBlocks());
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }


    CAccounts recip = DecodeRecipients(pwallet->chain(), sendTo);
    std::vector<CRecipient> vecSend;

    for (auto const & scriptBalances : recip) {

        RejectErc55Address(scriptBalances.first);

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (DecodeScript(addr.get_str()) == scriptBalances.first)
                fSubtractFeeFromAmount = true;
        }

        for (auto tokenAmount : scriptBalances.second.balances) {
            CRecipient recipient = {scriptBalances.first, tokenAmount.second, tokenAmount.first, fSubtractFeeFromAmount && (tokenAmount.first == DCT_ID{0}) };
            vecSend.push_back(recipient);
        }
    }

    EnsureWalletIsUnlocked(pwallet);

    // Shuffle recipient list
    std::shuffle(vecSend.begin(), vecSend.end(), FastRandomContext());

    // Send
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;
    CTransactionRef tx;
    bool fCreated = pwallet->CreateTransaction(*locked_chain, vecSend, tx, nFeeRequired, nChangePosRet, strFailReason, coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, std::move(mapValue), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return tx->GetHash().GetHex();
}

static UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"addmultisigaddress",
                "\nAdd a nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
                "Each key is a Defi address or hex-encoded public key.\n"
                "This functionality is only intended for use with non-watchonly addresses.\n"
                "See `importaddress` for watchonly p2sh address support.\n"
                "If 'label' is specified, assign address to that label.\n",
                {
                    {"nrequired", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of required signatures out of the n keys or addresses."},
                    {"keys", RPCArg::Type::ARR, RPCArg::Optional::NO, "A json array of defi addresses or hex-encoded public keys",
                        {
                            {"key", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "defi address or hex-encoded public key"},
                        },
                        },
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A label to assign the addresses to."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
            "{\n"
            "  \"address\":\"multisigaddress\",    (string) The value of the new multisig address.\n"
            "  \"redeemScript\":\"script\"         (string) The string value of the hex-encoded redemption script.\n"
            "}\n"
                },
                RPCExamples{
            "\nAdd a multisig address from 2 addresses\n"
            + HelpExampleCli("addmultisigaddress", "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    std::string label;
    if (!request.params[2].isNull())
        label = LabelFromValue(request.params[2]);

    int required = request.params[0].getInt<int>();

    // Get the public keys
    const UniValue& keys_or_addrs = request.params[1].get_array();
    std::vector<CPubKey> pubkeys;
    for (unsigned int i = 0; i < keys_or_addrs.size(); ++i) {
        if (IsHex(keys_or_addrs[i].get_str()) && (keys_or_addrs[i].get_str().length() == 66 || keys_or_addrs[i].get_str().length() == 130)) {
            pubkeys.push_back(HexToPubKey(keys_or_addrs[i].get_str()));
        } else {
            pubkeys.push_back(AddrToPubKey(pwallet, keys_or_addrs[i].get_str()));
        }
    }

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[3].isNull()) {
        if (!ParseOutputType(request.params[3].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[3].get_str()));
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner;
    CTxDestination dest = AddAndGetMultisigDestination(required, pubkeys, output_type, *pwallet, inner);
    pwallet->SetAddressBook(dest, label, "send");

    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(dest));
    result.pushKV("redeemScript", HexStr(inner.begin(), inner.end()));
    return result;
}

struct tallyitem
{
    CAmount nAmount{0};
    int nConf{std::numeric_limits<int>::max()};
    std::vector<uint256> txids;
    bool fIsWatchonly{false};
    tallyitem()
    {
    }
};

UniValue ListReceived(interfaces::Chain::Lock& locked_chain, CWallet * const pwallet, const UniValue& params, bool by_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (!params[0].isNull())
        nMinDepth = params[0].getInt<int>();

    // Whether to include empty labels
    bool fIncludeEmpty = false;
    if (!params[1].isNull())
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(params[2], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool has_filtered_address = false;
    CTxDestination filtered_address = CNoDestination();
    if (!by_label && params.size() > 3) {
        if (!IsValidDestinationString(params[3].get_str())) {
            throw JSONRPCError(RPC_WALLET_ERROR, "address_filter parameter was invalid");
        }
        filtered_address = DecodeDestination(params[3].get_str());
        has_filtered_address = true;
    }

    // Tally
    std::map<CTxDestination, tallyitem> mapTally;
    for (const auto& wtx : pwallet->mapWallet.get<ByHash>()) {

        if (wtx.IsCoinBase() || !locked_chain.checkFinalTx(*wtx.tx)) {
            continue;
        }

        int nDepth = wtx.GetDepthInMainChain(locked_chain);
        if (nDepth < nMinDepth)
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            if (has_filtered_address && !(filtered_address == address)) {
                continue;
            }

            isminefilter mine = IsMineCached(*pwallet, address);
            if(!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> label_tally;

    // Create mapAddressBook iterator
    // If we aren't filtering, go from begin() to end()
    auto start = pwallet->mapAddressBook.begin();
    auto end = pwallet->mapAddressBook.end();
    // If we are filtering, find() the applicable entry
    if (has_filtered_address) {
        start = pwallet->mapAddressBook.find(filtered_address);
        if (start != end) {
            end = std::next(start);
        }
    }

    for (auto item_it = start; item_it != end; ++item_it)
    {
        const CTxDestination& address = item_it->first;

        // Do not display Eth addresses
        // 
        // This is ok since it's not allowed on mainnet due as these
        // will be non-standard TX.
        // TODO: flag it later to optionally display later.

        if (address.index() == WitV16KeyEthHashType) {
            continue;
        }

        const std::string& label = item_it->second.name;
        auto it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (by_label)
        {
            tallyitem& _item = label_tally[label];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        }
        else
        {
            UniValue obj(UniValue::VOBJ);
            if(fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("address",       EncodeDestination(address));
            obj.pushKV("amount",        ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label", label);
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end())
            {
                for (const uint256& _item : (*it).second.txids)
                {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.pushKV("txids", transactions);
            ret.push_back(obj);
        }
    }

    if (by_label)
    {
        for (const auto& entry : label_tally)
        {
            CAmount nAmount = entry.second.nAmount;
            int nConf = entry.second.nConf;
            UniValue obj(UniValue::VOBJ);
            if (entry.second.fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("amount",        ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label",         entry.first);
            ret.push_back(obj);
        }
    }

    return ret;
}

static UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listreceivedbyaddress",
                "\nList balances by receiving address.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include addresses that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses (see 'importaddress')"},
                    {"address_filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If present, only return information on this address."},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,        (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in " + CURRENCY_UNIT + " received by the address\n"
            "    \"confirmations\" : n,               (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\",               (string) The label of the receiving address. The default label is \"\".\n"
            "    \"txids\": [\n"
            "       \"txid\",                         (string) The ids of transactions received with the address \n"
            "       ...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listreceivedbyaddress", "")
            + HelpExampleCli("listreceivedbyaddress", "6 true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true, \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    return ListReceived(*locked_chain, pwallet, request.params, false);
}

static UniValue listreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listreceivedbylabel",
                "\nList received transactions by label.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include labels that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses (see 'importaddress')"},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,   (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this label\n"
            "    \"confirmations\" : n,          (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\"           (string) The label of the receiving address. The default label is \"\".\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listreceivedbylabel", "")
            + HelpExampleCli("listreceivedbylabel", "6 true")
            + HelpExampleRpc("listreceivedbylabel", "6, true, true")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    return ListReceived(*locked_chain, pwallet, request.params, true);
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    if (IsValidDestination(dest)) {
        entry.pushKV("address", EncodeDestination(dest));
    }
}

/**
 * List transactions based on the given criteria.
 *
 * @param  pwallet        The wallet.
 * @param  wtx            The wallet transaction.
 * @param  nMinDepth      The minimum confirmation depth.
 * @param  fLong          Whether to include the JSON version of the transaction.
 * @param  ret            The UniValue into which the result is stored.
 * @param  filter_ismine  The "is mine" filter flags.
 * @param  filter_label   Optional label string to filter incoming transactions.
 */
static void ListTransactions(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const CWalletTx& wtx, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter_ismine, const std::string* filter_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    CAmount nFee;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, filter_ismine);

    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if (!filter_label)
    {
        for (const COutputEntry& s : listSent)
        {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMineCached(*pwallet, s.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, s.destination);
            entry.pushKV("category", "send");
            entry.pushKV("amount", ValueFromAmount(-s.amount));
            if (pwallet->mapAddressBook.count(s.destination)) {
                entry.pushKV("label", pwallet->mapAddressBook[s.destination].name);
            }
            entry.pushKV("vout", s.vout);
            entry.pushKV("fee", ValueFromAmount(-nFee));
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            entry.pushKV("abandoned", wtx.isAbandoned());
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain(locked_chain) >= nMinDepth)
    {
        for (const COutputEntry& r : listReceived)
        {
            std::string label;
            if (pwallet->mapAddressBook.count(r.destination)) {
                label = pwallet->mapAddressBook[r.destination].name;
            }
            if (filter_label && label != *filter_label) {
                continue;
            }
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMineCached(*pwallet, r.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, r.destination);
            if (wtx.IsCoinBase())
            {
                if (wtx.GetDepthInMainChain(locked_chain) < 1)
                    entry.pushKV("category", "orphan");
                else if (wtx.IsImmatureCoinBase(locked_chain))
                    entry.pushKV("category", "immature");
                else
                    entry.pushKV("category", "generate");
            }
            else
            {
                entry.pushKV("category", "receive");
            }
            entry.pushKV("amount", ValueFromAmount(r.amount));
            if (pwallet->mapAddressBook.count(r.destination)) {
                entry.pushKV("label", label);
            }
            entry.pushKV("vout", r.vout);
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            ret.push_back(entry);
        }
    }
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listtransactions",
                "\nIf a label name is provided, this will return only incoming transactions paying to addresses with the specified label.\n"
                "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, should be a valid label name to return only incoming transactions\n"
            "              with the specified label, or \"*\" to disable filtering and return all transactions."},
                    {"count", RPCArg::Type::NUM, /* default */ "10", "The number of transactions to return"},
                    {"skip", RPCArg::Type::NUM, /* default */ "0", "The number of transactions to skip"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Include transactions to watch-only addresses (see 'importaddress')"},
                    {"exclude_custom_tx", RPCArg::Type::BOOL, /* default */ "false to include all transactions, otherwise exclude custom transactions", "Exclude custom transactions"},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"address\":\"address\",    (string) The defi address of the transaction.\n"
            "    \"category\":               (string) The transaction category.\n"
            "                \"send\"                  Transactions sent.\n"
            "                \"receive\"               Non-coinbase transactions received.\n"
            "                \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                \"orphan\"                Orphaned coinbase transactions received.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
            "                                        for all other categories\n"
            "    \"label\": \"label\",       (string) A comment for the address/transaction, if any\n"
            "    \"vout\": n,                (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Negative confirmations indicate the\n"
            "                                         transaction conflicts with the block chain\n"
            "    \"trusted\": xxx,           (bool) Whether we consider the outputs of this unconfirmed transaction safe to spend.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\", (string) The transaction id.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"generated\": \"...\",       (boolean) If transaction is a coinbase transaction.\n"
            "    \"walletconflicts\": [\n"
            "      \"txid\": \"...\" (string) Conflicting transaction ID\n"
            "    ],\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                     may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx          (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                         'send' category of transactions.\n"
            "  }\n"
            "]\n"
                },
                RPCExamples{
            "\nList the most recent 10 transactions in the systems\n"
            + HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n"
            + HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listtransactions", "\"*\", 20, 100")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    const std::string* filter_label = nullptr;
    if (!request.params[0].isNull() && request.params[0].get_str() != "*") {
        filter_label = &request.params[0].get_str();
        if (filter_label->empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Label argument must be a valid label name or \"*\".");
        }
    }
    int nCount = 10;
    if (!request.params[1].isNull())
        nCount = request.params[1].getInt<int>();
    int nFrom = 0;
    if (!request.params[2].isNull())
        nFrom = request.params[2].getInt<int>();
    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(request.params[3], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool exclude_custom_tx = false;
    if (!request.params[4].isNull())
        exclude_custom_tx = request.params[4].get_bool();

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK2(pwallet->cs_wallet, locked_chain->mutex());

        const auto & txOrdered = pwallet->mapWallet.get<ByOrder>();
        std::vector<unsigned char> metadata;
        // iterate backwards until we have nCount items to return:
        for (auto it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
        {
            const auto& pwtx = *it;
            if (exclude_custom_tx && GuessCustomTxType(*pwtx.tx, metadata) != CustomTxType::None) {
                continue;
            }
            ListTransactions(*locked_chain, pwallet, pwtx, 0, true, ret, filter, filter_label);
            if ((int)ret.size() >= (nCount+nFrom)) break;
        }
    }

    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

static UniValue listsinceblock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listsinceblock",
                "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted.\n"
                "If \"blockhash\" is no longer a part of the main chain, transactions from the fork point onward are included.\n"
                "Additionally, if include_removed is set, transactions affecting the wallet which were removed are returned in the \"removed\" array.\n",
                {
                    {"blockhash", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, the block hash to list transactions since, otherwise list all transactions."},
                    {"target_confirmations", RPCArg::Type::NUM, /* default */ "1", "Return the nth block hash from the main chain. e.g. 1 would mean the best block hash. Note: this is not used as a filter, but only affects [lastblock] in the return value"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Include transactions to watch-only addresses (see 'importaddress')"},
                    {"include_removed", RPCArg::Type::BOOL, /* default */ "true", "Show transactions that were removed due to a reorg in the \"removed\" array\n"
            "                                                           (not guaranteed to work on pruned nodes)"},
                },
                RPCResult{
            "{\n"
            "  \"transactions\": [\n"
            "    \"address\":\"address\",    (string) The defi address of the transaction.\n"
            "    \"category\":               (string) The transaction category.\n"
            "                \"send\"                  Transactions sent.\n"
            "                \"receive\"               Non-coinbase transactions received.\n"
            "                \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                \"orphan\"                Orphaned coinbase transactions received.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
            "                                         for all other categories\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction.\n"
            "                                          When it's < 0, it means the transaction conflicted that many blocks ago.\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx,         (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the 'send' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"label\" : \"label\"       (string) A comment for the address/transaction, if any\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
            "  ],\n"
            "  \"removed\": [\n"
            "    <structure is the same as \"transactions\" above, only present if include_removed=true>\n"
            "    Note: transactions that were re-added in the active chain will appear as-is in this array, and may thus have a positive confirmation count.\n"
            "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the block (target_confirmations-1) from the best block on the main chain. This is typically used to feed back into listsinceblock the next time you call it. So you would generally use a target_confirmations of say 6, so you will be continually re-notified of transactions until they've reached 6 confirmations plus any new ones\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("listsinceblock", "")
            + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6")
            + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    std::optional<int> height;    // Height of the specified block or the common ancestor, if the block provided was in a deactivated chain.
    std::optional<int> altheight; // Height of the specified block, even if it's in a deactivated chain.
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    uint256 blockId;
    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        blockId = ParseHashV(request.params[0], "blockhash");
        height = locked_chain->findFork(blockId, &altheight);
        if (!height) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    if (!request.params[1].isNull()) {
        target_confirms = request.params[1].getInt<int>();

        if (target_confirms < 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        }
    }

    if (ParseIncludeWatchonly(request.params[2], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool include_removed = (request.params[3].isNull() || request.params[3].get_bool());

    const std::optional<int> tip_height = locked_chain->getHeight();
    int depth = tip_height && height ? (1 + *tip_height - *height) : -1;

    UniValue transactions(UniValue::VARR);

    for (const auto& tx : pwallet->mapWallet.get<ByHash>()) {
        if (depth == -1 || tx.GetDepthInMainChain(*locked_chain) < depth) {
            ListTransactions(*locked_chain, pwallet, tx, 0, true, transactions, filter, nullptr /* filter_label */);
        }
    }

    // when a reorg'd block is requested, we also list any relevant transactions
    // in the blocks of the chain that was detached
    UniValue removed(UniValue::VARR);
    while (include_removed && altheight && *altheight > *height) {
        CBlock block;
        if (!pwallet->chain().findBlock(blockId, &block) || block.IsNull()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }
        for (const CTransactionRef& tx : block.vtx) {
            if (auto wtx = pwallet->GetWalletTx(tx->GetHash())) {
                // We want all transactions regardless of confirmation count to appear here,
                // even negative confirmation ones, hence the big negative.
                ListTransactions(*locked_chain, pwallet, *wtx, -100000000, true, removed, filter, nullptr /* filter_label */);
            }
        }
        blockId = block.hashPrevBlock;
        --*altheight;
    }

    int last_height = tip_height ? *tip_height + 1 - target_confirms : -1;
    uint256 lastblock = last_height >= 0 ? locked_chain->getBlockHash(last_height) : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("transactions", transactions);
    if (include_removed) ret.pushKV("removed", removed);
    ret.pushKV("lastblock", lastblock.GetHex());

    return ret;
}

static UniValue gettransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"gettransaction",
                "\nGet detailed information about in-wallet transaction <txid>\n",
                {
                    {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses in balance calculation and details[]"},
                },
                RPCResult{
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in " + CURRENCY_UNIT + "\n"
            "  \"fee\": x.xxx,            (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                              'send' category of transactions.\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The index of the transaction in the block that includes it\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"address\" : \"address\",          (string) The defi address involved in the transaction\n"
            "      \"category\" :                      (string) The transaction category.\n"
            "                   \"send\"                  Transactions sent.\n"
            "                   \"receive\"               Non-coinbase transactions received.\n"
            "                   \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                   \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                   \"orphan\"                Orphaned coinbase transactions received.\n"
            "      \"amount\" : x.xxx,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
            "      \"vout\" : n,                       (numeric) the vout value\n"
            "      \"fee\": x.xxx,                     (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                           'send' category of transactions.\n"
            "      \"abandoned\": xxx                  (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                           'send' category of transactions.\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true")
            + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    uint256 hash(ParseHashV(request.params[0], "txid"));

    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(request.params[1], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    UniValue entry(UniValue::VOBJ);
    const CWalletTx* wtx = pwallet->GetWalletTx(hash);
    if (!wtx) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }

    /// @todo tokens: id == 0??
    CAmount nCredit = wtx->GetCredit(*locked_chain, filter)[DCT_ID{0}]; /// @todo tokens: extend?
    CAmount nDebit = wtx->GetDebit(filter)[DCT_ID{0}];                  /// @todo tokens: extend?
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx->IsFromMe(filter) ? GetNonMintedValueOut(*(wtx->tx), DCT_ID{}) - nDebit : 0);

    entry.pushKV("amount", ValueFromAmount(nNet - nFee));
    if (wtx->IsFromMe(filter))
        entry.pushKV("fee", ValueFromAmount(nFee));

    WalletTxToJSON(pwallet->chain(), *locked_chain, *wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(*locked_chain, pwallet, *wtx, 0, false, details, filter, nullptr /* filter_label */);
    entry.pushKV("details", details);

    std::string strHex = EncodeHexTx(*(wtx->tx), pwallet->chain().rpcSerializationFlags());
    entry.pushKV("hex", strHex);

    return entry;
}

static UniValue abandontransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"abandontransaction",
                "\nMark in-wallet transaction <txid> as abandoned\n"
                "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
                "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
                "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
                "It has no effect on transactions which are already abandoned.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    uint256 hash(ParseHashV(request.params[0], "txid"));

    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    if (!pwallet->AbandonTransaction(*locked_chain, hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");
    }

    return NullUniValue;
}


static UniValue backupwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"backupwallet",
                "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n",
                {
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination directory or file"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("backupwallet", "\"backup.dat\"")
            + HelpExampleRpc("backupwallet", "\"backup.dat\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
}


static UniValue keypoolrefill(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"keypoolrefill",
                "\nFills the keypool."+
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newsize", RPCArg::Type::NUM, /* default */ "100", "The new keypool size"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("keypoolrefill", "")
            + HelpExampleRpc("keypoolrefill", "")
                },
            }.Check(request);

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (!request.params[0].isNull()) {
        if (request.params[0].getInt<int>() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].getInt<int>();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < kpSize) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
    }

    return NullUniValue;
}


static UniValue walletpassphrase(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletpassphrase",
                "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
                "This is needed prior to performing transactions related to private keys such as sending defis\n"
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet passphrase"},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The time to keep the decryption key in seconds; capped at 100000000 (~3 years)."},
                },
                RPCResults{},
                RPCExamples{
            "\nUnlock the wallet for 60 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nLock the wallet again (before 60 seconds)\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
    }

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    // Get the timeout
    int64_t nSleepTime = request.params[1].getInt<int64_t>();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout
    constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
    if (nSleepTime > MAX_SLEEP_TIME) {
        nSleepTime = MAX_SLEEP_TIME;
    }

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->Unlock(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    pwallet->TopUpKeyPool();

    pwallet->nRelockTime = GetTime() + nSleepTime;

    // Keep a weak pointer to the wallet so that it is possible to unload the
    // wallet before the following callback is called. If a valid shared pointer
    // is acquired in the callback then the wallet is still loaded.
    std::weak_ptr<CWallet> weak_wallet = wallet;
    pwallet->chain().rpcRunLater(strprintf("lockwallet(%s)", pwallet->GetName()), [weak_wallet] {
        if (auto shared_wallet = weak_wallet.lock()) {
            LOCK(shared_wallet->cs_wallet);
            shared_wallet->Lock();
            shared_wallet->nRelockTime = 0;
        }
    }, nSleepTime);

    return NullUniValue;
}


static UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletpassphrasechange",
                "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n",
                {
                    {"oldpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The current passphrase"},
                    {"newpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The new passphrase"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
    }

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.empty() || strNewWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    return NullUniValue;
}


static UniValue walletlock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletlock",
                "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
                "After calling this method, you will need to call walletpassphrase again\n"
                "before being able to call any methods which require the wallet to be unlocked.\n",
                {},
                RPCResults{},
                RPCExamples{
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("walletlock", "")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
    }

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


static UniValue encryptwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"encryptwallet",
                "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
                "After this, any calls that interact with private keys such as sending or signing \n"
                "will require the passphrase to be set prior the making these calls.\n"
                "Use the walletpassphrase call for this, and then walletlock call.\n"
                "If the wallet is already encrypted, use the walletpassphrasechange call.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long."},
                },
                RPCResults{},
                RPCExamples{
            "\nEncrypt your wallet\n"
            + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending defi\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can do something like sign\n"
            + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: wallet does not contain private keys, nothing to encrypt.");
    }

    if (pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
    }

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->EncryptWallet(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
    }

    return "wallet encrypted; The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

static UniValue lockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"lockunspent",
                "\nUpdates list of temporarily unspendable outputs.\n"
                "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
                "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
                "A locked transaction output will not be chosen by automatic coin selection, when spending defis.\n"
                "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
                "is always cleared (by virtue of process exit) when a node stops or fails.\n"
                "Also see the listunspent call\n",
                {
                    {"unlock", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to unlock (true) or lock (false) the specified transactions"},
                    {"transactions", RPCArg::Type::ARR, /* default */ "empty array", "A json array of objects. Each object the txid (string) vout (numeric).",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
            "true|false    (boolean) Whether the command was successful or not\n"
                },
                RPCExamples{
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    RPCTypeCheckArgument(request.params[0], UniValue::VBOOL);

    bool fUnlock = request.params[0].get_bool();

    if (request.params[1].isNull()) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    RPCTypeCheckArgument(request.params[1], UniValue::VARR);

    const UniValue& output_params = request.params[1];

    // Create and validate the COutPoints first.

    std::vector<COutPoint> outputs;
    outputs.reserve(output_params.size());

    for (unsigned int idx = 0; idx < output_params.size(); idx++) {
        const UniValue& o = output_params[idx].get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        const uint256 txid(ParseHashO(o, "txid"));
        const int nOutput = o.find_value("vout").getInt<int>();
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }

        const COutPoint outpt(txid, nOutput);

        const CWalletTx* trans = pwallet->GetWalletTx(outpt.hash);
        if (!trans) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, unknown transaction");
        }

        if (outpt.n >= trans->tx->vout.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout index out of bounds");
        }

        if (pwallet->IsSpent(*locked_chain, outpt.hash, outpt.n)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected unspent output");
        }

        const bool is_locked = pwallet->IsLockedCoin(outpt.hash, outpt.n);

        if (fUnlock && !is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected locked output");
        }

        if (!fUnlock && is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output already locked");
        }

        outputs.push_back(outpt);
    }

    // Atomically set (un)locked status for the outputs.
    for (const COutPoint& outpt : outputs) {
        if (fUnlock) pwallet->UnlockCoin(outpt);
        else pwallet->LockCoin(outpt);
    }

    return true;
}

static UniValue listlockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listlockunspent",
                "\nReturns list of temporarily unspendable outputs.\n"
                "See the lockunspent call to lock and unlock transactions for spending.\n",
                {},
                RPCResult{
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listlockunspent", "")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    std::vector<COutPoint> vOutpts;
    pwallet->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    for (const COutPoint& outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.pushKV("txid", outpt.hash.GetHex());
        o.pushKV("vout", (int)outpt.n);
        ret.push_back(o);
    }

    return ret;
}

static UniValue settxfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"settxfee",
                "\nSet the transaction fee per kB for this wallet. Overrides the global -paytxfee command line parameter.\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The transaction fee in " + CURRENCY_UNIT + "/kB"},
                },
                RPCResult{
            "true|false        (boolean) Returns true if successful\n"
                },
                RPCExamples{
                    HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    CAmount nAmount = AmountFromValue(request.params[0]);
    CFeeRate tx_fee_rate(nAmount, 1000);
    if (tx_fee_rate == 0) {
        // automatic selection
    } else if (tx_fee_rate < pwallet->chain().relayMinFee()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than min relay tx fee (%s)", pwallet->chain().relayMinFee().ToString()));
    } else if (tx_fee_rate < pwallet->m_min_fee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than wallet min fee (%s)", pwallet->m_min_fee.ToString()));
    }

    pwallet->m_pay_tx_fee = tx_fee_rate;
    return true;
}

static UniValue getbalances(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const rpc_wallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(rpc_wallet.get(), request.fHelp)) {
        return NullUniValue;
    }
    CWallet& wallet = *rpc_wallet;

    RPCHelpMan{
        "getbalances",
        "Returns an object with all balances in " + CURRENCY_UNIT + ".\n",
        {
            {"with_tokens", RPCArg::Type::BOOL, /* default */ "false", "Include tokens balances; Default is 'false' for backward compatibility."},
        },
        RPCResult{
            "{\n"
            "    \"mine\": {                        (object) balances from outputs that the wallet can sign\n"
            "      \"trusted\": xxx                 (numeric) trusted balance (outputs created by the wallet or confirmed outputs)\n"
            "      \"untrusted_pending\": xxx       (numeric) untrusted pending balance (outputs created by others that are in the mempool)\n"
            "      \"immature\": xxx                (numeric) balance from immature coinbase outputs\n"
            "      \"used\": xxx                    (numeric) (only present if avoid_reuse is set) balance from coins sent to addresses that were previously spent from (potentially privacy violating)\n"
            "    },\n"
            "    \"watchonly\": {                   (object) watchonly balances (not present if wallet does not watch anything)\n"
            "      \"trusted\": xxx                 (numeric) trusted balance (outputs created by the wallet or confirmed outputs)\n"
            "      \"untrusted_pending\": xxx       (numeric) untrusted pending balance (outputs created by others that are in the mempool)\n"
            "      \"immature\": xxx                (numeric) balance from immature coinbase outputs\n"
            "    },\n"
            "}\n"},
        RPCExamples{
            HelpExampleCli("getbalances", "") +
            HelpExampleRpc("getbalances", "")},
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    auto locked_chain = wallet.chain().lock();
    LOCK2(wallet.cs_wallet, locked_chain->mutex());

    bool with_tokens = request.params[0].isNull() ? false : request.params[0].get_bool();

    UniValue obj(UniValue::VOBJ);

    const auto bal = wallet.GetBalance();
    UniValue balances{UniValue::VOBJ};
    {
        UniValue balances_mine{UniValue::VOBJ};
        balances_mine.pushKV("trusted", TokensToJSON(bal.m_mine_trusted, with_tokens));
        balances_mine.pushKV("untrusted_pending", TokensToJSON(bal.m_mine_untrusted_pending, with_tokens));
        balances_mine.pushKV("immature", ValueFromAmount(bal.m_mine_immature));
        if (wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE)) {
            // If the AVOID_REUSE flag is set, bal has been set to just the un-reused address balance. Get
            // the total balance, and then subtract bal to get the reused address balance.
            auto full_bal = wallet.GetBalance(0, false);
            Increment(full_bal.m_mine_trusted, full_bal.m_mine_untrusted_pending);
            Decrement(full_bal.m_mine_trusted, bal.m_mine_trusted);
            Decrement(full_bal.m_mine_trusted, bal.m_mine_untrusted_pending);
            balances_mine.pushKV("used", TokensToJSON(full_bal.m_mine_trusted, with_tokens));
//                balances_mine.pushKV("used", ValueFromAmount(full_bal.m_mine_trusted[0] + full_bal.m_mine_untrusted_pending[0] - bal.m_mine_trusted[0] - bal.m_mine_untrusted_pending[0]));
        }
        balances.pushKV("mine", balances_mine);
    }
    if (wallet.HaveWatchOnly()) {
        UniValue balances_watchonly{UniValue::VOBJ};
        balances_watchonly.pushKV("trusted", TokensToJSON(bal.m_watchonly_trusted, with_tokens));
        balances_watchonly.pushKV("untrusted_pending", TokensToJSON(bal.m_watchonly_untrusted_pending, with_tokens));
        balances_watchonly.pushKV("immature", ValueFromAmount(bal.m_watchonly_immature));
        balances.pushKV("watchonly", balances_watchonly);
    }
    return balances;
}

static UniValue getwalletinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"getwalletinfo",
                "Returns an object containing various wallet state info.\n",
                {
                    {"with_tokens", RPCArg::Type::BOOL, /* default */ "false", "Include tokens balances; Default is 'false' for backward compatibility."},
                },
                RPCResult{
            "{\n"
            "  \"walletname\": xxxxx,               (string) the wallet name\n"
            "  \"walletversion\": xxxxx,            (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,                (numeric) DEPRECATED. Identical to getbalances().mine.trusted\n"
            "  \"unconfirmed_balance\": xxx,        (numeric) DEPRECATED. Identical to getbalances().mine.untrusted_pending\n"
            "  \"immature_balance\": xxxxxx,        (numeric) DEPRECATED. Identical to getbalances().mine.immature\n"
            "  \"txcount\": xxxxxxx,                (numeric) the total number of transactions in the wallet\n"
            "  \"keypoololdest\": xxxxxx,           (numeric) the timestamp (seconds since Unix epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,               (numeric) how many new keys are pre-generated (only counts external keys)\n"
            "  \"keypoolsize_hd_internal\": xxxx,   (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
            "  \"unlocked_until\": ttt,             (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,                (numeric) the transaction fee configuration, set in " + CURRENCY_UNIT + "/kB\n"
            "  \"hdseedid\": \"<hash160>\"            (string, optional) the Hash160 of the HD seed (only present when HD is enabled)\n"
            "  \"private_keys_enabled\": true|false (boolean) false if privatekeys are disabled for this wallet (enforced watch-only wallet)\n"
            "  \"avoid_reuse\": true|false          (boolean) whether this wallet tracks clean/dirty coins in terms of reuse\n"
            "  \"descriptors\": true|false          (boolean) whether this wallet holds descriptor records\n"
            "  \"scanning\":                        (json object) current scanning details, or false if no scan is in progress\n"
            "    {\n"
            "      \"duration\" : xxxx              (numeric) elapsed seconds since scan start\n"
            "      \"progress\" : x.xxxx,           (numeric) scanning progress percentage [0.0, 1.0]\n"
            "    }\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getwalletinfo", "")
            + HelpExampleRpc("getwalletinfo", "")
                },
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    bool with_tokens = request.params[0].isNull() ? false : request.params[0].get_bool();

    UniValue obj(UniValue::VOBJ);

    size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
    const auto bal = pwallet->GetBalance();
    obj.pushKV("walletname", pwallet->GetName());
    obj.pushKV("walletversion", pwallet->GetVersion());
    obj.pushKV("balance", TokensToJSON(bal.m_mine_trusted, with_tokens));
    obj.pushKV("unconfirmed_balance", TokensToJSON(bal.m_mine_untrusted_pending, with_tokens));
    obj.pushKV("immature_balance", ValueFromAmount(bal.m_mine_immature));
    obj.pushKV("txcount",       (int)pwallet->mapWallet.size());
    obj.pushKV("keypoololdest", pwallet->GetOldestKeyPoolTime());
    obj.pushKV("keypoolsize", (int64_t)kpExternalSize);
    CKeyID seed_id = pwallet->GetHDChain().seed_id;
    if (pwallet->CanSupportFeature(FEATURE_HD_SPLIT)) {
        obj.pushKV("keypoolsize_hd_internal",   (int64_t)(pwallet->GetKeyPoolSize() - kpExternalSize));
    }
    if (pwallet->IsCrypted()) {
        obj.pushKV("unlocked_until", pwallet->nRelockTime);
    }
    obj.pushKV("paytxfee", ValueFromAmount(pwallet->m_pay_tx_fee.GetFeePerK()));
    if (!seed_id.IsNull()) {
        obj.pushKV("hdseedid", seed_id.GetHex());
    }
    obj.pushKV("private_keys_enabled", !pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));
    obj.pushKV("avoid_reuse", pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE));
    obj.pushKV("descriptors", pwallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));
    if (pwallet->IsScanning()) {
        UniValue scanning(UniValue::VOBJ);
        scanning.pushKV("duration", pwallet->ScanningDuration() / 1000);
        scanning.pushKV("progress", pwallet->ScanningProgress());
        obj.pushKV("scanning", scanning);
    } else {
        obj.pushKV("scanning", false);
    }
    return obj;
}

static UniValue listwalletdir(const JSONRPCRequest& request)
{
            RPCHelpMan{"listwalletdir",
                "Returns a list of wallets in the wallet directory.\n",
                {},
                RPCResult{
            "{\n"
            "  \"wallets\" : [                (json array of objects)\n"
            "    {\n"
            "      \"name\" : \"name\"          (string) The wallet name\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("listwalletdir", "")
            + HelpExampleRpc("listwalletdir", "")
                },
            }.Check(request);

    UniValue wallets(UniValue::VARR);
    for (const auto& path : ListWalletDir()) {
        UniValue wallet(UniValue::VOBJ);
        wallet.pushKV("name", path.utf8string());
        wallets.push_back(wallet);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("wallets", wallets);
    return result;
}

static UniValue listwallets(const JSONRPCRequest& request)
{
            RPCHelpMan{"listwallets",
                "Returns a list of currently loaded wallets.\n"
                "For full information on the wallet, use \"getwalletinfo\"\n",
                {},
                RPCResult{
            "[                         (json array of strings)\n"
            "  \"walletname\"            (string) the wallet name\n"
            "   ...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listwallets", "")
            + HelpExampleRpc("listwallets", "")
                },
            }.Check(request);

    UniValue obj(UniValue::VARR);

    for (const std::shared_ptr<CWallet>& wallet : GetWallets()) {
        if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
            return NullUniValue;
        }

        LOCK(wallet->cs_wallet);

        obj.push_back(wallet->GetName());
    }

    return obj;
}

static UniValue loadwallet(const JSONRPCRequest& request)
{
            RPCHelpMan{"loadwallet",
                "\nLoads a wallet from a wallet file or directory."
                "\nNote that all wallet command-line options used when starting defid will be"
                "\napplied to the new wallet (eg -zapwallettxes, upgradewallet, rescan, etc).\n",
                {
                    {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet directory or .dat file."},
                },
                RPCResult{
            "{\n"
            "  \"name\" :    <wallet_name>,        (string) The wallet name if loaded successfully.\n"
            "  \"warning\" : <warning>,            (string) Warning message if wallet was not loaded cleanly.\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("loadwallet", "\"test.dat\"")
            + HelpExampleRpc("loadwallet", "\"test.dat\"")
                },
            }.Check(request);

    WalletLocation location(request.params[0].get_str());

    if (!location.Exists()) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet " + location.GetName() + " not found.");
    } else if (fs::is_directory(location.GetPath())) {
        // The given filename is a directory. Check that there's a wallet file.
        fs::path wallet_dat_file = location.GetPath() / "wallet.dat";
        fs::path wallet_sqlite_file = location.GetPath() / "wallet.sqlite";
        if (fs::symlink_status(wallet_dat_file).type() == fs::file_type::not_found &&
            fs::symlink_status(wallet_sqlite_file).type() == fs::file_type::not_found) {
            throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Directory " + location.GetName() + " does not contain a wallet.dat file (nor a wallet.sqlite file).");
        }
    }

    std::string error, warning;
    std::shared_ptr<CWallet> const wallet = LoadWallet(*g_rpc_interfaces->chain, location, error, warning);
    if (!wallet) throw JSONRPCError(RPC_WALLET_ERROR, error);

    if (spv::pspv) {
        bool foundSPV{false};
        for (const auto& entry : wallet->mapAddressBook) {
            if (entry.second.purpose == "spv") {
                uint160 userHash;
                if (entry.first.index() == PKHashType) {
                    userHash = std::get<PKHash>(entry.first);
                } else if (entry.first.index() == WitV0KeyHashType) {
                    userHash = std::get<WitnessV0KeyHash>(entry.first);
                } else {
                    continue;
                }

                spv::pspv->AddBitcoinHash(userHash);
                foundSPV = true;
            }

            if (foundSPV) {
                spv::pspv->RebuildBloomFilter();
                spv::pspv->Rescan(std::numeric_limits<int>::max());
            }
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", warning);

    return obj;
}

static UniValue setwalletflag(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            std::string flags = "";
            for (auto& it : WALLET_FLAG_MAP)
                if (it.second & MUTABLE_WALLET_FLAGS)
                    flags += (flags == "" ? "" : ", ") + it.first;
            RPCHelpMan{"setwalletflag",
                "\nChange the state of the given wallet flag for a wallet.\n",
                {
                    {"flag", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of the flag to change. Current available flags: " + flags},
                    {"value", RPCArg::Type::BOOL, /* default */ "true", "The new state."},
                },
                RPCResult{
            "{\n"
            "    \"flag_name\": string   (string) The name of the flag that was modified\n"
            "    \"flag_state\": bool    (bool) The new state of the flag\n"
            "    \"warnings\": string    (string) Any warnings associated with the change\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("setwalletflag", "avoid_reuse")
                  + HelpExampleRpc("setwalletflag", "\"avoid_reuse\"")
                },
            }.Check(request);

    std::string flag_str = request.params[0].get_str();
    bool value = request.params[1].isNull() || request.params[1].get_bool();

    if (!WALLET_FLAG_MAP.count(flag_str)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown wallet flag: %s", flag_str));
    }

    auto flag = WALLET_FLAG_MAP.at(flag_str);

    if (!(flag & MUTABLE_WALLET_FLAGS)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Wallet flag is immutable: %s", flag_str));
    }

    UniValue res(UniValue::VOBJ);

    if (pwallet->IsWalletFlagSet(flag) == value) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Wallet flag is already set to %s: %s", value ? "true" : "false", flag_str));
    }

    res.pushKV("flag_name", flag_str);
    res.pushKV("flag_state", value);

    if (value) {
        pwallet->SetWalletFlag(flag);
    } else {
        pwallet->UnsetWalletFlag(flag);
    }

    if (flag && value && WALLET_FLAG_CAVEATS.count(flag)) {
        res.pushKV("warnings", WALLET_FLAG_CAVEATS.at(flag));
    }

    return res;
}

static UniValue createwallet(const JSONRPCRequest& request)
{
    RPCHelpMan{
        "createwallet",
        "\nCreates and loads a new wallet.\n",
        {
            {"wallet_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name for the new wallet. If this is a path, the wallet will be created at the path location."},
            {"disable_private_keys", RPCArg::Type::BOOL, /* default */ "false", "Disable the possibility of private keys (only watchonlys are possible in this mode)."},
            {"blank", RPCArg::Type::BOOL, /* default */ "false", "Create a blank wallet. A blank wallet has no keys or HD seed. One can be set using sethdseed."},
            {"passphrase", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Encrypt the wallet with this passphrase."},
            {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "false", "Keep track of coin reuse, and treat dirty and clean coins differently with privacy considerations in mind."},
            {"dbtype", RPCArg::Type::STR, /* default */ "bdb", "The database backend of the new wallet, either \"bdb\" (legacy Berkeley DB) or \"sqlite\"."},
        },
        RPCResult{
            "{\n"
            "  \"name\" :    <wallet_name>,        (string) The wallet name if created successfully. If the wallet was created using a full path, the wallet_name will be the full path.\n"
            "  \"warning\" : <warning>,            (string) Warning message if wallet was not loaded cleanly.\n"
            "}\n"
        },
        RPCExamples{
            HelpExampleCli("createwallet", "\"testwallet\"")
            + HelpExampleRpc("createwallet", "\"testwallet\"")
        },
    }.Check(request);

    uint64_t flags = 0;
    if (!request.params[1].isNull() && request.params[1].get_bool()) {
        flags |= WALLET_FLAG_DISABLE_PRIVATE_KEYS;
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        flags |= WALLET_FLAG_BLANK_WALLET;
    }
    SecureString passphrase;
    passphrase.reserve(100);
    std::string warning;
    if (!request.params[3].isNull()) {
        passphrase = request.params[3].get_str().c_str();
        if (passphrase.empty()) {
            // Empty string means unencrypted
            warning = "Empty string given as passphrase, wallet will not be encrypted.";
        }
    }

    if (!request.params[4].isNull() && request.params[4].get_bool()) {
        flags |= WALLET_FLAG_AVOID_REUSE;
    }

    DatabaseFormat format = DefaultDatabaseFormat();
    if (!request.params[5].isNull()) {
        const std::string dbtype = request.params[5].get_str();
        if (!ParseDatabaseFormat(dbtype, format)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown wallet database type %s, expected \"bdb\" or \"sqlite\"", dbtype));
        }
    }

    std::string error;
    std::string create_warning;
    std::shared_ptr<CWallet> wallet;
    WalletCreationStatus status = CreateWallet(*g_rpc_interfaces->chain, passphrase, flags, request.params[0].get_str(), error, create_warning, wallet, format);
    switch (status) {
        case WalletCreationStatus::CREATION_FAILED:
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        case WalletCreationStatus::ENCRYPTION_FAILED:
            throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, error);
        case WalletCreationStatus::SUCCESS:
            break;
        // no default case, so the compiler can warn about missing cases
    }

    if (warning.empty()) {
        warning = create_warning;
    } else if (!warning.empty() && !create_warning.empty()){
        warning += "; " + create_warning;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", warning);

    return obj;
}

static UniValue unloadwallet(const JSONRPCRequest& request)
{
            RPCHelpMan{"unloadwallet",
                "Unloads the wallet referenced by the request endpoint otherwise unloads the wallet specified in the argument.\n"
                "Specifying the wallet name on a wallet endpoint is invalid.",
                {
                    {"wallet_name", RPCArg::Type::STR, /* default */ "the wallet name from the RPC request", "The name of the wallet to unload."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("unloadwallet", "wallet_name")
            + HelpExampleRpc("unloadwallet", "wallet_name")
                },
            }.Check(request);

    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        if (!request.params[0].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot unload the requested wallet");
        }
    } else {
        wallet_name = request.params[0].get_str();
    }

    std::shared_ptr<CWallet> wallet = GetWallet(wallet_name);
    if (!wallet) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }

    // Release the "main" shared pointer and prevent further notifications.
    // Note that any attempt to load the same wallet would fail until the wallet
    // is destroyed (see CheckUniqueFileid).
    if (!RemoveWallet(wallet)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Requested wallet already unloaded");
    }

    UnloadWallet(std::move(wallet));

    return NullUniValue;
}

static UniValue migratewallet(const JSONRPCRequest& request)
{
            RPCHelpMan{"migratewallet",
                "\nMigrates a legacy Berkeley DB wallet into a new SQLite wallet.\n"
                "The source wallet is opened read-only and is never modified, the migrated\n"
                "wallet is written to a new wallet directory. The migration is verified\n"
                "record by record before it is reported as successful.\n"
                "The source wallet must not be loaded, unload it before migrating.\n",
                {
                    {"wallet_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of the wallet that should be migrated. The wallet must not be loaded."},
                    {"target_name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The name of the newly created SQLite wallet, defaults to \"<wallet_name>_sqlite\"."},
                    {"backup", RPCArg::Type::BOOL, /* default */ "false", "Keep a copy of the legacy wallet file (\"wallet.dat.legacy.bak\") next to the original."},
                },
                RPCResult{
            "{\n"
            "  \"source\" :   <wallet_name>,  (string) The name of the migrated wallet.\n"
            "  \"target\" :   <wallet_name>,  (string) The name of the newly created wallet.\n"
            "  \"records\" :  <count>,        (numeric) The number of migrated records.\n"
            "  \"format\" :   \"sqlite\",      (string) The backend of the newly created wallet.\n"
            "}\n"
                },
                RPCExamples{
            HelpExampleCli("migratewallet", "\"legacywallet\"")
            + HelpExampleRpc("migratewallet", "\"legacywallet\"")
                },
            }.Check(request);

    const std::string source_name = request.params[0].get_str();
    if (source_name.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "The name of the wallet that should be migrated must not be empty");
    }

    WalletLocation source_location(source_name);
    if (!source_location.Exists()) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet " + source_name + " not found.");
    }

    // A loaded wallet cannot be migrated, the source is opened read-only by a
    // separate database environment which conflicts with the loaded wallet.
    if (IsWalletLoaded(source_location.GetPath())) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet " + source_name + " is loaded, unload it before migrating it.");
    }

    std::string target_name = source_name + "_sqlite";
    if (!request.params[1].isNull()) {
        target_name = request.params[1].get_str();
        if (target_name.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "The name of the target wallet must not be empty");
        }
    }
    if (target_name == source_name) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "The target wallet must have a different name than the source wallet");
    }

    WalletLocation target_location(target_name);
    if (target_location.Exists()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet " + target_name + " already exists.");
    }

    WalletMigrationResult result;
    if (!MigrateWalletDatabase(source_location.GetPath(), target_location.GetPath(), DatabaseFormat::SQLITE, result)) {
        throw JSONRPCError(RPC_WALLET_ERROR, result.error);
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        const fs::path source_file = WalletDataFilePath(source_location.GetPath());
        const fs::path backup_file = fs::PathFromString(fs::PathToString(source_file) + ".legacy.bak");
        std::error_code ec;
        std::filesystem::copy_file(source_file, backup_file, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LogPrintf("migratewallet: unable to create the backup %s: %s\n", fs::PathToString(backup_file), ec.message());
        } else {
            LogPrintf("migratewallet: created the backup %s\n", fs::PathToString(backup_file));
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("source", source_name);
    obj.pushKV("target", target_name);
    obj.pushKV("records", result.records);
    obj.pushKV("format", DatabaseFormatName(result.target_format));

    return obj;
}

static UniValue listunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{
                "listunspent",
                "\nReturns array of unspent transaction outputs\n"
                "with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include txouts paid to specified addresses.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum confirmations to filter"},
                    {"maxconf", RPCArg::Type::NUM, /* default */ "9999999", "The maximum confirmations to filter"},
                    {"addresses", RPCArg::Type::ARR, /* default */ "empty array", "A json array of defi addresses to filter",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "defi address"},
                        },
                    },
                    {"include_unsafe", RPCArg::Type::BOOL, /* default */ "true", "Include outputs that are not safe to spend\n"
            "                  See description of \"safe\" attribute below."},
                    {"query_options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "JSON with query options",
                        {
                            {"minimumAmount", RPCArg::Type::AMOUNT, /* default */ "0", "Minimum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Maximum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumCount", RPCArg::Type::NUM, /* default */ "unlimited", "Maximum number of UTXOs"},
                            {"minimumSumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Minimum sum value of all UTXOs in " + CURRENCY_UNIT + ""},
                            {"tokenId", RPCArg::Type::STR, /* default */ "all", "Filter by token (id/symbol/creationTx)"},
                        },
                        "query_options"},
                },
                RPCResult{
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the defi address\n"
            "    \"label\" : \"label\",        (string) The associated label, or \"\" for the default label\n"
            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"tokenId\" : n,            (numeric) the transaction output token Id\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            "    \"redeemScript\" : \"script\" (string) The redeemScript if scriptPubKey is P2SH\n"
            "    \"witnessScript\" : \"script\" (string) witnessScript if the scriptPubKey is P2WSH or P2SH-P2WSH\n"
            "    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            "    \"solvable\" : xxx,         (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "    \"reused\" : xxx,           (bool) (only present if avoid_reuse is set) Whether this output is reused/dirty (sent to an address that was previously spent from)\n"
            "    \"desc\" : xxx,             (string, only when solvable) A descriptor for spending this output\n"
            "    \"safe\" : xxx              (bool) Whether this output is considered safe to spend. Unconfirmed transactions\n"
            "                              from outside keys and unconfirmed replacement transactions are considered unsafe\n"
            "                              and are not eligible for spending by fundrawtransaction and sendtoaddress.\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listunspent", "")
            + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleCli("listunspent", "6 9999999 '[]' true '{ \"minimumAmount\": 0.005 }'")
            + HelpExampleRpc("listunspent", "6, 9999999, [] , true, { \"minimumAmount\": 0.005 } ")
                },
            }.Check(request);

    int nMinDepth = 1;
    if (!request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].getInt<int>();
    }

    int nMaxDepth = 9999999;
    if (!request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].getInt<int>();
    }

    std::set<CTxDestination> destinations;
    if (!request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CTxDestination dest = DecodeDestination(input.get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Defi address: ") + input.get_str());
            }
            if (!destinations.insert(dest).second) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            }
        }
    }

    bool include_unsafe = true;
    if (!request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;
    int nOnlyTokensId = -1; /// @todo tokens set default to 0 or -1 ???

    if (!request.params[4].isNull()) {
        const UniValue& options = request.params[4].get_obj();

        if (options.exists("minimumAmount"))
            nMinimumAmount = AmountFromValue(options["minimumAmount"]);

        if (options.exists("maximumAmount"))
            nMaximumAmount = AmountFromValue(options["maximumAmount"]);

        if (options.exists("minimumSumAmount"))
            nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);

        if (options.exists("maximumCount"))
            nMaximumCount = options["maximumCount"].getInt<int64_t>();

        DCT_ID tokenId;
        if (options.exists("tokenId")) {
            if (pwallet->chain().existTokenGuessId(options["tokenId"].getValStr(), tokenId)) {
                nOnlyTokensId = tokenId.v;
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid token id: ") + options["tokenId"].getValStr());
            }
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;
    {
        CCoinControl cctl;
        cctl.m_avoid_address_reuse = false;
        cctl.m_min_depth = nMinDepth;
        cctl.m_max_depth = nMaxDepth;
        if (nOnlyTokensId >= 0) {
            cctl.m_tokenFilter.insert(DCT_ID{(uint32_t) nOnlyTokensId});
        }
        auto locked_chain = pwallet->chain().lock();
        LOCK2(pwallet->cs_wallet, locked_chain->mutex());
        pwallet->AvailableCoins(*locked_chain, vecOutputs, !include_unsafe, &cctl, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount);
    }

    LOCK(pwallet->cs_wallet);

    const bool avoid_reuse = pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);

    for (const COutput& out : vecOutputs) {
        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);
        bool reused = avoid_reuse && pwallet->IsUsedDestination(address);

        if (destinations.size() && (!fValidAddress || !destinations.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", out.tx->GetHash().GetHex());
        entry.pushKV("vout", out.i);

        if (fValidAddress) {
            entry.pushKV("address", EncodeDestination(address));

            auto i = pwallet->mapAddressBook.find(address);
            if (i != pwallet->mapAddressBook.end()) {
                entry.pushKV("label", i->second.name);
            }

            if (scriptPubKey.IsPayToScriptHash()) {
                const CScriptID& hash = CScriptID(std::get<ScriptHash>(address));
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript)) {
                    entry.pushKV("redeemScript", HexStr(redeemScript.begin(), redeemScript.end()));
                    // Now check if the redeemScript is actually a P2WSH script
                    CTxDestination witness_destination;
                    if (redeemScript.IsPayToWitnessScriptHash()) {
                        bool extracted = ExtractDestination(redeemScript, witness_destination);
                        assert(extracted);
                        // Also return the witness script
                        const WitnessV0ScriptHash& whash = std::get<WitnessV0ScriptHash>(witness_destination);
                        CScriptID id;
                        CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                        CScript witnessScript;
                        if (pwallet->GetCScript(id, witnessScript)) {
                            entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                        }
                    }
                }
            } else if (scriptPubKey.IsPayToWitnessScriptHash()) {
                const WitnessV0ScriptHash& whash = std::get<WitnessV0ScriptHash>(address);
                CScriptID id;
                CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                CScript witnessScript;
                if (pwallet->GetCScript(id, witnessScript)) {
                    entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                }
            }
        }

        entry.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));
        entry.pushKV("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue));
        entry.pushKV("tokenId", out.tx->tx->vout[out.i].nTokenId.ToString());
        entry.pushKV("confirmations", out.nDepth);
        entry.pushKV("spendable", out.fSpendable);
        entry.pushKV("solvable", out.fSolvable);
        if (out.fSolvable) {
            auto descriptor = InferDescriptor(scriptPubKey, *pwallet);
            entry.pushKV("desc", descriptor->ToString());
        }
        if (avoid_reuse) entry.pushKV("reused", reused);
        entry.pushKV("safe", out.fSafe);
        results.push_back(entry);
    }

    return results;
}

void FundTransaction(CWallet* const pwallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, UniValue options)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CCoinControl coinControl;
    change_position = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!options.isNull()) {
      if (options.type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        coinControl.fAllowWatchOnly = options.get_bool();
      }
      else {
        RPCTypeCheckArgument(options, UniValue::VOBJ);
        RPCTypeCheckObj(options,
            {
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"feeRate", UniValueType()}, // will be checked below
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
                {"psbt_version", UniValueType(UniValue::VNUM)},
                {"include_unsafe", UniValueType(UniValue::VBOOL)},
            },
            true, true);

        if (options.exists("changeAddress")) {
            CTxDestination dest = DecodeDestination(options["changeAddress"].get_str());

            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "changeAddress must be a valid defi address");
            }

            coinControl.destChange = dest;
        }

        if (options.exists("changePosition"))
            change_position = options["changePosition"].getInt<int>();

        if (options.exists("change_type")) {
            if (options.exists("changeAddress")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both changeAddress and address_type options");
            }
            coinControl.m_change_type = pwallet->m_default_change_type;
            if (!ParseOutputType(options["change_type"].get_str(), *coinControl.m_change_type)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(options["includeWatching"], *pwallet);

        if (options.exists("lockUnspents"))
            lockUnspents = options["lockUnspents"].get_bool();

        if (options.exists("feeRate"))
        {
            coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
            coinControl.fOverrideFeeRate = true;
        }

        if (options.exists("subtractFeeFromOutputs"))
            subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();

        if (options.exists("replaceable")) {
            coinControl.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("include_unsafe")) {
            coinControl.m_include_unsafe_inputs = options["include_unsafe"].get_bool();
        }
        if (options.exists("conf_target")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");
            }
            coinControl.m_confirm_target = ParseConfirmTarget(options["conf_target"], pwallet->chain().estimateMaxBlocks());
        }
        if (options.exists("estimate_mode")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
            }
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coinControl.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
      }
    } else {
        // if options is null and not a bool
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(NullUniValue, *pwallet);
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (change_position != -1 && (change_position < 0 || (unsigned int)change_position > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].getInt<int>();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    std::string strFailReason;

    if (!pwallet->FundTransaction(tx, fee_out, change_position, strFailReason, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }
}

static UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"fundrawtransaction",
                "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
                "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
                "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                "The inputs added will not be signed, use signrawtransactionwithkey\n"
                " or signrawtransactionwithwallet for that.\n"
                "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
                "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n",
                {
                    {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}",
                        {
                            {"changeAddress", RPCArg::Type::STR, /* default */ "pool address", "The defi address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only"},
                            {"include_unsafe", RPCArg::Type::BOOL, /* default */ "false", "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions)."},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, /* default */ "empty array", "A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              Those recipients will receive less defis than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"iswitness", RPCArg::Type::BOOL, /* default */ "depends on heuristic tests", "Whether the transaction hex is a serialized witness transaction.\n"
                        "If iswitness is not present, heuristic tests will be used in decoding.\n"
                        "If true, only witness deserialization will be tried.\n"
                        "If false, only non-witness deserialization will be tried.\n"
                        "This boolean should reflect whether the transaction has inputs\n"
                        "(e.g. fully valid, or on-chain transactions), if known by the caller."
                    },
                },
                RPCResult{
                            "{\n"
                            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransactionwithwallet", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                                },
    }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValueType(), UniValue::VBOOL});

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CAmount fee;
    int change_position;
    FundTransaction(pwallet, tx, fee, change_position, request.params[1]);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);

    return result;
}

UniValue signrawtransactionwithwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"signrawtransactionwithwallet",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"prevtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of previous dependent transaction outputs",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"scriptPubKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "script key"},
                                    {"redeemScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witnessScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH or P2SH-P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "(required for Segwit inputs) the amount spent"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
            "{\n"
            "  \"hex\" : \"value\",                  (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,          (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                      (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",              (string) The hash of the referenced, previous transaction\n"
            "      \"vout\" : n,                   (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",          (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,               (numeric) Script sequence number\n"
            "      \"error\" : \"text\"              (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithwallet", "\"myhex\"")
            + HelpExampleRpc("signrawtransactionwithwallet", "\"myhex\"")
                },
            }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str(), true)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    // Sign the transaction
    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());
    EnsureWalletIsUnlocked(pwallet);

    // Fetch previous transactions (inputs):
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : mtx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    pwallet->chain().findCoins(coins);

    return SignTransaction(mtx, request.params[1], pwallet, coins, false, request.params[2]);
}

static UniValue bumpfee_helper(const std::string& method_name, const JSONRPCRequest& request)
{
    const bool want_psbt = method_name == "psbtbumpfee";
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();


    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    std::vector<RPCArg> optionArgs{
        {"confTarget", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
        {"totalFee", RPCArg::Type::NUM, /* default */ "fallback to 'confTarget'", "Total fee (NOT feerate) to pay, in satoshis. (DEPRECATED)\n"
         "                         In rare cases, the actual fee paid might be slightly higher than the specified\n"
         "                         totalFee if the tx change output has to be removed because it is too close to\n"
         "                         the dust threshold."},
        {"replaceable", RPCArg::Type::BOOL, /* default */ "true", "Whether the new transaction should still be\n"
         "                         marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
         "                         be left unchanged from the original. If false, any input sequence numbers in the\n"
         "                         original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
         "                         so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
         "                         still be replaceable in practice, for example if it has unconfirmed ancestors which\n"
         "                         are replaceable)."},
        {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
         "         \"UNSET\"\n"
         "         \"ECONOMICAL\"\n"
         "         \"CONSERVATIVE\""},
    };
    if (want_psbt) {
        optionArgs.emplace_back("psbt_version", RPCArg::Type::NUM, /* default */ "0", "The PSBT version number to use. 0 = BIP174 (PSBTv0), 2 = PSBTv2.");
    }

    RPCHelpMan{method_name,
                std::string("\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n") +
                (want_psbt ? "Returns a PSBT instead of creating and signing a new transaction.\n" : "") +
                "An opt-in RBF transaction with the given txid must be in the wallet.\n"
                "The command will pay the additional fee by reducing change outputs or adding inputs when necessary. It may add a new change output if one does not already exist.\n"
                "If `totalFee` (DEPRECATED) is given, adding inputs is not supported, so there must be a single change output that is big enough or it will fail.\n"
                "All inputs in the original transaction will be included in the replacement transaction.\n"
                "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
                "By default, the new fee will be calculated automatically using estimatesmartfee.\n"
                "The user can specify a confirmation target for estimatesmartfee.\n"
                "Alternatively, the user can specify totalFee (DEPRECATED), or use RPC settxfee to set a higher fee rate.\n"
                "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
                "returned by getnetworkinfo) to enter the node's mempool.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid to be bumped"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "", optionArgs, "options"},
                },
                RPCResult{
            want_psbt ?
            "{\n"
            "  \"psbt\":    \"value\",   (string)  The base64-encoded unsigned PSBT of the new transaction\n"
            "  \"origfee\":  n,         (numeric) Fee of the replaced transaction\n"
            "  \"fee\":      n,         (numeric) Fee of the new transaction\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty)\n"
            "}\n" :
            "{\n"
            "  \"txid\":    \"value\",   (string)  The id of the new transaction\n"
            "  \"origfee\":  n,         (numeric) Fee of the replaced transaction\n"
            "  \"fee\":      n,         (numeric) Fee of the new transaction\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty)\n"
            "}\n"
                },
                RPCExamples{
            "\nBump the fee, get the new transaction\'s " + std::string(want_psbt ? "psbt" : "txid") + "\n" +
                    HelpExampleCli(method_name, "<txid>")
                },
    }.Check(request);

    if (!want_psbt && pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "bumpfee is not available with wallets that have private keys disabled. Use psbtbumpfee instead.");
    }
    if (!want_psbt) {
        // Only the signing path needs an unlocked wallet. psbtbumpfee must stay
        // usable with locked wallets, so it is not checked here.
        EnsureWalletIsUnlocked(pwallet);
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash(ParseHashV(request.params[0], "txid"));

    // optional parameters
    CAmount totalFee = 0;
    CCoinControl coin_control;
    // Watch-only wallets have no private keys, but their coins still have to be
    // eligible to fund the replacement transaction.
    coin_control.fAllowWatchOnly = pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    coin_control.m_signal_bip125_rbf = true;
    uint32_t psbt_version = 0;
    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"totalFee", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
                {"psbt_version", UniValueType(UniValue::VNUM)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("totalFee")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and totalFee options should not both be set. Please provide either a confirmation target for fee estimation or an explicit total fee for the transaction.");
        } else if (options.exists("confTarget")) { // TODO: alias this to conf_target
            coin_control.m_confirm_target = ParseConfirmTarget(options["confTarget"], pwallet->chain().estimateMaxBlocks());
        } else if (options.exists("totalFee")) {
            if (want_psbt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "totalFee is not supported by psbtbumpfee, use the fee estimation options instead.");
            }
            if (!pwallet->chain().rpcEnableDeprecated("totalFee")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "totalFee argument has been deprecated and will be removed in 0.20. Please use -deprecatedrpc=totalFee to continue using this argument until removal.");
            }
            totalFee = options["totalFee"].getInt<int64_t>();
            if (totalFee <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid totalFee %s (must be greater than 0)", FormatMoney(totalFee)));
            }
        }

        if (options.exists("replaceable")) {
            coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("estimate_mode")) {
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coin_control.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
        if (options.exists("psbt_version")) {
            psbt_version = ParsePSBTVersion(options["psbt_version"]);
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());


    std::vector<std::string> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res;
    if (totalFee > 0) {
        // Targeting total fee bump. Requires a change output of sufficient size.
        res = feebumper::CreateTotalBumpTransaction(pwallet, hash, coin_control, totalFee, errors, old_fee, new_fee, mtx);
    } else {
        // Targeting feerate bump.
        res = feebumper::CreateRateBumpTransaction(pwallet, hash, coin_control, errors, old_fee, new_fee, mtx);
    }
    if (res != feebumper::Result::OK) {
        switch(res) {
            case feebumper::Result::INVALID_ADDRESS_OR_KEY:
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0]);
                break;
            case feebumper::Result::INVALID_REQUEST:
                throw JSONRPCError(RPC_INVALID_REQUEST, errors[0]);
                break;
            case feebumper::Result::INVALID_PARAMETER:
                throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0]);
                break;
            case feebumper::Result::WALLET_ERROR:
                throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
                break;
            default:
                throw JSONRPCError(RPC_MISC_ERROR, errors[0]);
                break;
        }
    }

    UniValue result(UniValue::VOBJ);
    if (!want_psbt) {
        // Sign and commit the bumped transaction
        if (!feebumper::SignTransaction(pwallet, mtx)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
        }
        // commit the bumped transaction
        uint256 txid;
        if (feebumper::CommitTransaction(pwallet, hash, std::move(mtx), errors, txid) != feebumper::Result::OK) {
            throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
        }
        result.pushKV("txid", txid.GetHex());
    } else {
        // Return an unsigned PSBT of the replacement transaction. Nothing is signed,
        // no wallet state is changed and the transaction is not broadcast, so that an
        // external signer can complete and broadcast it.
        PartiallySignedTransaction psbtx(mtx, psbt_version);
        bool complete = false;
        const TransactionError err = FillPSBT(pwallet, psbtx, complete, /*sighash_type=*/1, /*sign=*/false, /*bip32derivs=*/true);
        if (err != TransactionError::OK) {
            throw JSONRPCTransactionError(err);
        }
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }
    result.pushKV("origfee", ValueFromAmount(old_fee));
    result.pushKV("fee", ValueFromAmount(new_fee));
    UniValue result_errors(UniValue::VARR);
    for (const std::string& error : errors) {
        result_errors.push_back(error);
    }
    result.pushKV("errors", result_errors);

    return result;
}

static UniValue bumpfee(const JSONRPCRequest& request)
{
    return bumpfee_helper("bumpfee", request);
}

static UniValue psbtbumpfee(const JSONRPCRequest& request)
{
    return bumpfee_helper("psbtbumpfee", request);
}

UniValue rescanblockchain(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"rescanblockchain",
                "\nRescan the local blockchain for wallet related transactions.\n"
                "Note: Use \"getwalletinfo\" to query the scanning progress.\n",
                {
                    {"start_height", RPCArg::Type::NUM, /* default */ "0", "block height where the rescan should start"},
                    {"stop_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "the last block height that should be scanned. If none is provided it will rescan up to the tip at return time of this call."},
                },
                RPCResult{
            "{\n"
            "  \"start_height\"     (numeric) The block height where the rescan started (the requested height or 0)\n"
            "  \"stop_height\"      (numeric) The height of the last rescanned block. May be null in rare cases if there was a reorg and the call didn't scan any blocks because they were already scanned in the background.\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("rescanblockchain", "100000 120000")
            + HelpExampleRpc("rescanblockchain", "100000, 120000")
                },
            }.Check(request);

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int start_height = 0;
    uint256 start_block, stop_block;
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(locked_chain->mutex());
        std::optional<int> tip_height = locked_chain->getHeight();

        if (!request.params[0].isNull()) {
            start_height = request.params[0].getInt<int>();
            if (start_height < 0 || !tip_height || start_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid start_height");
            }
        }

        std::optional<int> stop_height;
        if (!request.params[1].isNull()) {
            stop_height = request.params[1].getInt<int>();
            if (*stop_height < 0 || !tip_height || *stop_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid stop_height");
            }
            else if (*stop_height < start_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "stop_height must be greater than start_height");
            }
        }

        // We can't rescan beyond non-pruned blocks, stop and throw an error
        if (locked_chain->findPruned(start_height, stop_height)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Can't rescan beyond pruned data. Use RPC call getblockchaininfo to determine your pruned height.");
        }

        if (tip_height) {
            start_block = locked_chain->getBlockHash(start_height);
            // If called with a stop_height, set the stop_height here to
            // trigger a rescan to that height.
            // If called without a stop height, leave stop_height as null here
            // so rescan continues to the tip (even if the tip advances during
            // rescan).
            if (stop_height) {
                stop_block = locked_chain->getBlockHash(*stop_height);
            }
        }
    }

    CWallet::ScanResult result =
        pwallet->ScanForWalletTransactions(start_block, stop_block, reserver, true /* fUpdate */);
    switch (result.status) {
    case CWallet::ScanResult::SUCCESS:
        break;
    case CWallet::ScanResult::FAILURE:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan failed. Potentially corrupted data files.");
    case CWallet::ScanResult::USER_ABORT:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        // no default case, so the compiler can warn about missing cases
    }
    UniValue response(UniValue::VOBJ);
    response.pushKV("start_height", start_height);
    response.pushKV("stop_height", result.last_scanned_height ? *result.last_scanned_height : UniValue());
    return response;
}

class DescribeWalletAddressVisitor
{
public:
    CWallet * const pwallet;

    void ProcessSubScript(const CScript& subscript, UniValue& obj) const
    {
        // Always present: script type and redeemscript
        std::vector<std::vector<unsigned char>> solutions_data;
        txnouttype which_type = Solver(subscript, solutions_data);
        obj.pushKV("script", GetTxnOutputType(which_type));
        obj.pushKV("hex", HexStr(subscript.begin(), subscript.end()));

        CTxDestination embedded;
        if (ExtractDestination(subscript, embedded)) {
            // Only when the script corresponds to an address.
            UniValue subobj(UniValue::VOBJ);
            UniValue detail = DescribeAddress(embedded);
            subobj.pushKVs(detail);
            UniValue wallet_detail = std::visit(*this, embedded);
            subobj.pushKVs(wallet_detail);
            subobj.pushKV("address", EncodeDestination(embedded));
            subobj.pushKV("scriptPubKey", HexStr(subscript.begin(), subscript.end()));
            // Always report the pubkey at the top level, so that `getnewaddress()['pubkey']` always works.
            if (subobj.exists("pubkey")) obj.pushKV("pubkey", subobj["pubkey"]);
            obj.pushKV("embedded", std::move(subobj));
        } else if (which_type == TX_MULTISIG) {
            // Also report some information on multisig scripts (which do not have a corresponding address).
            // TODO: abstract out the common functionality between this logic and ExtractDestinations.
            obj.pushKV("sigsrequired", solutions_data[0][0]);
            UniValue pubkeys(UniValue::VARR);
            for (size_t i = 1; i < solutions_data.size() - 1; ++i) {
                CPubKey key(solutions_data[i].begin(), solutions_data[i].end());
                pubkeys.push_back(HexStr(key.begin(), key.end()));
            }
            obj.pushKV("pubkeys", std::move(pubkeys));
        }
    }

    explicit DescribeWalletAddressVisitor(CWallet* _pwallet) : pwallet(_pwallet) {}

    UniValue operator()(const CNoDestination& dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const PKHash& pkhash) const
    {
        CKeyID keyID(pkhash);
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        if (pwallet && pwallet->GetPubKey(keyID, vchPubKey)) {
            obj.pushKV("pubkey", HexStr(vchPubKey));
            obj.pushKV("iscompressed", vchPubKey.IsCompressed());
        }
        return obj;
    }

    UniValue operator()(const ScriptHash& scripthash) const
    {
        CScriptID scriptID(scripthash);
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        if (pwallet && pwallet->GetCScript(scriptID, subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessV0KeyHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey pubkey;
        if (pwallet && pwallet->GetPubKey(CKeyID(id, KeyAddressType::COMPRESSED), pubkey)) {
            obj.pushKV("pubkey", HexStr(pubkey));
        }
        return obj;
    }

    UniValue operator()(const WitnessV0ScriptHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        CRIPEMD160 hasher;
        uint160 hash;
        hasher.Write(id.begin(), 32).Finalize(hash.begin());
        if (pwallet && pwallet->GetCScript(CScriptID(hash), subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessUnknown& id) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const WitnessV16EthHash& id) const {
        UniValue obj(UniValue::VOBJ);
        CPubKey pubkey;
        if (pwallet && pwallet->GetPubKey(CKeyID(id, KeyAddressType::UNCOMPRESSED), pubkey)) {
            obj.pushKV("pubkey", HexStr(pubkey));
        }
        return obj;
    }
};

static UniValue DescribeWalletAddress(CWallet* pwallet, const CTxDestination& dest)
{
    UniValue ret(UniValue::VOBJ);
    UniValue detail = DescribeAddress(dest);
    ret.pushKVs(detail);
    ret.pushKVs(std::visit(DescribeWalletAddressVisitor(pwallet), dest));
    return ret;
}

/** Convert CAddressBookData to JSON record.  */
static UniValue AddressBookDataToJSON(const CAddressBookData& data, const bool verbose)
{
    UniValue ret(UniValue::VOBJ);
    if (verbose) {
        ret.pushKV("name", data.name);
    }
    ret.pushKV("purpose", data.purpose);
    return ret;
}

UniValue getaddressinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getaddressinfo",
                "\nReturn information about the given defi address. Some information requires the address\n"
                "to be in the wallet.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The defi address to get the information of."},
                },
                RPCResult{
            "{\n"
            "  \"address\" : \"address\",        (string) The defi address validated\n"
            "  \"scriptPubKey\" : \"hex\",       (string) The hex-encoded scriptPubKey generated by the address\n"
            "  \"ismine\" : true|false,        (boolean) If the address is yours or not\n"
            "  \"iswatchonly\" : true|false,   (boolean) If the address is watchonly\n"
            "  \"solvable\" : true|false,      (boolean) Whether we know how to spend coins sent to this address, ignoring the possible lack of private keys\n"
            "  \"desc\" : \"desc\",            (string, optional) A descriptor for spending coins sent to this address (only when solvable)\n"
            "  \"isscript\" : true|false,      (boolean) If the key is a script\n"
            "  \"ischange\" : true|false,      (boolean) If the address was used for change output\n"
            "  \"iswitness\" : true|false,     (boolean) If the address is a witness address\n"
            "  \"witness_version\" : version   (numeric, optional) The version number of the witness program\n"
            "  \"witness_program\" : \"hex\"     (string, optional) The hex value of the witness program\n"
            "  \"script\" : \"type\"             (string, optional) The output script type. Only if \"isscript\" is true and the redeemscript is known. Possible types: nonstandard, pubkey, pubkeyhash, scripthash, multisig, nulldata, witness_v0_keyhash, witness_v0_scripthash, witness_unknown\n"
            "  \"hex\" : \"hex\",                (string, optional) The redeemscript for the p2sh address\n"
            "  \"pubkeys\"                     (string, optional) Array of pubkeys associated with the known redeemscript (only if \"script\" is \"multisig\")\n"
            "    [\n"
            "      \"pubkey\"\n"
            "      ,...\n"
            "    ]\n"
            "  \"sigsrequired\" : xxxxx        (numeric, optional) Number of signatures required to spend multisig output (only if \"script\" is \"multisig\")\n"
            "  \"pubkey\" : \"publickeyhex\",    (string, optional) The hex value of the raw public key, for single-key addresses (possibly embedded in P2SH or P2WSH)\n"
            "  \"embedded\" : {...},           (object, optional) Information about the address embedded in P2SH or P2WSH, if relevant and known. It includes all getaddressinfo output fields for the embedded address, excluding metadata (\"timestamp\", \"hdkeypath\", \"hdseedid\") and relation to the wallet (\"ismine\", \"iswatchonly\").\n"
            "  \"iscompressed\" : true|false,  (boolean, optional) If the pubkey is compressed\n"
            "  \"label\" :  \"label\"         (string) The label associated with the address, \"\" is the default label\n"
            "  \"timestamp\" : timestamp,      (number, optional) The creation time of the key if available in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"hdkeypath\" : \"keypath\"       (string, optional) The HD keypath if the key is HD and available\n"
            "  \"hdseedid\" : \"<hash160>\"      (string, optional) The Hash160 of the HD seed\n"
            "  \"hdmasterfingerprint\" : \"<hash160>\" (string, optional) The fingperint of the master key.\n"
            "  \"labels\"                      (object) Array of labels associated with the address.\n"
            "    [\n"
            "      { (json object of label data)\n"
            "        \"name\": \"labelname\" (string) The label\n"
            "        \"purpose\": \"string\" (string) Purpose of address (\"send\" for sending address, \"receive\" for receiving address)\n"
            "      },...\n"
            "    ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getaddressinfo", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\"")
            + HelpExampleRpc("getaddressinfo", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\"")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    UniValue ret(UniValue::VOBJ);

    const auto userAddress = request.params[0].get_str();
    CTxDestination dest;
    if (IsHex(userAddress)) {  // ScriptPubKey
        const auto hexVec = ParseHex(userAddress);
        const auto reqOwner = CScript(hexVec.begin(), hexVec.end());
        if (!ExtractDestination(reqOwner, dest) || !IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        }
    } else {  // Address
        dest = DecodeDestination(userAddress);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        }
    }

    std::string currentAddress = EncodeDestination(dest);
    ret.pushKV("address", currentAddress);

    CScript scriptPubKey = GetScriptForDestination(dest);
    ret.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));

    isminetype mine = IsMineCached(*pwallet, dest);
    ret.pushKV("ismine", bool(mine & ISMINE_SPENDABLE));
    bool solvable = IsSolvable(*pwallet, scriptPubKey);
    ret.pushKV("solvable", solvable);
    if (solvable) {
       ret.pushKV("desc", InferDescriptor(scriptPubKey, *pwallet)->ToString());
    }
    ret.pushKV("iswatchonly", bool(mine & ISMINE_WATCH_ONLY));
    UniValue detail = DescribeWalletAddress(pwallet, dest);
    ret.pushKVs(detail);
    if (pwallet->mapAddressBook.count(dest)) {
        ret.pushKV("label", pwallet->mapAddressBook[dest].name);
    }
    ret.pushKV("ischange", pwallet->IsChange(scriptPubKey));
    const CKeyMetadata* meta = nullptr;
    CKeyID key_id = GetKeyOrDefaultFromDestination(*pwallet, dest);
    if (!key_id.IsNull()) {
        auto it = pwallet->mapKeyMetadata.find(key_id);
        if (it != pwallet->mapKeyMetadata.end()) {
            meta = &it->second;
        }
    }
    if (!meta) {
        auto it = pwallet->m_script_metadata.find(CScriptID(scriptPubKey));
        if (it != pwallet->m_script_metadata.end()) {
            meta = &it->second;
        }
    }
    if (meta) {
        ret.pushKV("timestamp", meta->nCreateTime);
        if (meta->has_key_origin) {
            ret.pushKV("hdkeypath", WriteHDKeypath(meta->key_origin.path));
            ret.pushKV("hdseedid", meta->hd_seed_id.GetHex());
            ret.pushKV("hdmasterfingerprint", HexStr(meta->key_origin.fingerprint, meta->key_origin.fingerprint + 4));
        }
    }

    // Currently only one label can be associated with an address, return an array
    // so the API remains stable if we allow multiple labels to be associated with
    // an address.
    UniValue labels(UniValue::VARR);
    std::map<CTxDestination, CAddressBookData>::iterator mi = pwallet->mapAddressBook.find(dest);
    if (mi != pwallet->mapAddressBook.end()) {
        labels.push_back(AddressBookDataToJSON(mi->second, true));
    }
    ret.pushKV("labels", std::move(labels));

    return ret;
}

static UniValue getaddressesbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getaddressesbylabel",
                "\nReturns the list of addresses assigned the specified label.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label."},
                },
                RPCResult{
            "{ (json object with addresses as keys)\n"
            "  \"address\": { (json object with information about address)\n"
            "    \"purpose\": \"string\" (string)  Purpose of address (\"send\" for sending address, \"receive\" for receiving address)\n"
            "  },...\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getaddressesbylabel", "\"tabby\"")
            + HelpExampleRpc("getaddressesbylabel", "\"tabby\"")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    std::string label = LabelFromValue(request.params[0]);

    // Find all addresses that have the given label
    UniValue ret(UniValue::VOBJ);
    std::set<std::string> addresses;
    for (const std::pair<const CTxDestination, CAddressBookData>& item : pwallet->mapAddressBook) {
        if (item.second.name == label) {
            std::string address = EncodeDestination(item.first);
            // CWallet::mapAddressBook is not expected to contain duplicate
            // address strings, but build a separate set as a precaution just in
            // case it does.
            bool unique = addresses.emplace(address).second;
            assert(unique);
            // UniValue::pushKV checks if the key exists in O(N)
            // and since duplicate addresses are unexpected (checked with
            // std::set in O(log(N))), UniValue::__pushKV is used instead,
            // which currently is O(1).
            ret.pushKVEnd(address, AddressBookDataToJSON(item.second, false));
        }
    }

    if (ret.empty()) {
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, std::string("No addresses with label " + label));
    }

    return ret;
}

static UniValue listlabels(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listlabels",
                "\nReturns the list of all labels, or labels that are assigned to addresses with a specific purpose.\n",
                {
                    {"purpose", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Address purpose to list labels for ('send','receive'). An empty string is the same as not providing this argument."},
                },
                RPCResult{
            "[               (json array of string)\n"
            "  \"label\",      (string) Label name\n"
            "  ...\n"
            "]\n"
                },
                RPCExamples{
            "\nList all labels\n"
            + HelpExampleCli("listlabels", "") +
            "\nList labels that have receiving addresses\n"
            + HelpExampleCli("listlabels", "receive") +
            "\nList labels that have sending addresses\n"
            + HelpExampleCli("listlabels", "send") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listlabels", "receive")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    std::string purpose;
    if (!request.params[0].isNull()) {
        purpose = request.params[0].get_str();
    }

    // Add to a set to sort by label name, then insert into Univalue array
    std::set<std::string> label_set;
    for (const std::pair<const CTxDestination, CAddressBookData>& entry : pwallet->mapAddressBook) {
        if (purpose.empty() || entry.second.purpose == purpose) {
            label_set.insert(entry.second.name);
        }
    }

    UniValue ret(UniValue::VARR);
    for (const std::string& name : label_set) {
        ret.push_back(name);
    }

    return ret;
}

UniValue sethdseed(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"sethdseed",
                "\nSet or generate a new HD wallet seed. Non-HD wallets will not be upgraded to being a HD wallet. Wallets that are already\n"
                "HD will have a new HD seed set so that new keys added to the keypool will be derived from this new seed.\n"
                "\nNote that you will need to MAKE A NEW BACKUP of your wallet after setting the HD wallet seed." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newkeypool", RPCArg::Type::BOOL, /* default */ "true", "Whether to flush old unused addresses, including change addresses, from the keypool and regenerate it.\n"
            "                             If true, the next address from getnewaddress and change address from getrawchangeaddress will be from this new seed.\n"
            "                             If false, addresses (including change addresses if the wallet already had HD Chain Split enabled) from the existing\n"
            "                             keypool will be used until it has been depleted."},
                    {"seed", RPCArg::Type::STR, /* default */ "random seed", "The WIF private key to use as the new HD seed.\n"
            "                             The seed value can be retrieved using the dumpwallet command. It is the private key marked hdseed=1"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("sethdseed", "")
            + HelpExampleCli("sethdseed", "false")
            + HelpExampleCli("sethdseed", "true \"wifkey\"")
            + HelpExampleRpc("sethdseed", "true, \"wifkey\"")
                },
            }.Check(request);

    if (pwallet->chain().isInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot set a new HD seed while still in Initial Block Download");
    }

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed to a wallet with private keys disabled");
    }

    // A wallet with an active descriptor derives its addresses from the
    // descriptor and not from the HD keypool, changing the seed would create a
    // second source of truth for the wallet addresses.
    if (pwallet->HasActiveDescriptors()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed on a wallet with an active descriptor");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, locked_chain->mutex());

    // Do not do anything to non-HD wallets
    if (!pwallet->CanSupportFeature(FEATURE_HD)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed on a non-HD wallet. Start with -upgradewallet in order to upgrade a non-HD wallet to HD");
    }

    EnsureWalletIsUnlocked(pwallet);

    bool flush_key_pool = true;
    if (!request.params[0].isNull()) {
        flush_key_pool = request.params[0].get_bool();
    }

    CPubKey master_pub_key;
    if (request.params[1].isNull()) {
        master_pub_key = pwallet->GenerateNewSeed();
    } else {
        CKey key = DecodeSecret(request.params[1].get_str());
        if (!key.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
        }

        if (HaveKey(*pwallet, key)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key (either as an HD seed or as a loose private key)");
        }

        master_pub_key = pwallet->DeriveNewSeed(key);
    }

    pwallet->SetHDSeed(master_pub_key);
    if (flush_key_pool) pwallet->NewKeyPool();

    return NullUniValue;
}

UniValue walletprocesspsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletprocesspsbt",
                "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
                "that we can sign for." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, /* default */ "true", "Also sign the transaction when updating"},
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type to sign with if not specified by the PSBT. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
            "{\n"
            "  \"psbt\" : \"value\",          (string) The base64-encoded partially signed transaction\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("walletprocesspsbt", "\"psbt\"")
                },
            }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL, UniValue::VSTR});

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Fill transaction with our data and also sign
    bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
    bool bip32derivs = request.params[3].isNull() ? false : request.params[3].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, nHashType, sign, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    UniValue result(UniValue::VOBJ);
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);

    return result;
}

UniValue walletcreatefundedpsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletcreatefundedpsbt",
                "\nCreates and funds a transaction in the Partially Signed Transaction format. Inputs will be added if supplied inputs are not enough\n"
                "Implements the Creator and Updater roles.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "A json array of json objects",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "a json array with outputs (key-value pairs), where none of the keys are duplicated.\n"
                            "That is, each address can only appear once and there can only be one 'data' object.\n"
                            "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                            "                             accepted as second parameter.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the defi address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                },
                                },
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"changeAddress", RPCArg::Type::STR_HEX, /* default */ "pool address", "The defi address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only"},
                            {"include_unsafe", RPCArg::Type::BOOL, /* default */ "false", "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions)."},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, /* default */ "empty array", "A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              Those recipients will receive less defis than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "Fallback to wallet's confirmation target", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\""},
                            {"psbt_version", RPCArg::Type::NUM, /* default */ "0", "The PSBT version number to use. 0 = BIP174 (PSBTv0), 2 = PSBTv2."},
                        },
                        "options"},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                            "{\n"
                            "  \"psbt\": \"value\",        (string)  The resulting raw transaction (base64-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
                                },
                            }.Check(request);

    RPCTypeCheck(request.params, {
        UniValue::VARR,
        UniValueType(), // ARR or OBJ, checked later
        UniValue::VNUM,
        UniValue::VOBJ,
        UniValue::VBOOL
        }, true
    );

    CAmount fee;
    int change_position;
    bool rbf = pwallet->m_signal_rbf;
    const UniValue &replaceable_arg = request.params[3]["replaceable"];
    if (!replaceable_arg.isNull()) {
        RPCTypeCheckArgument(replaceable_arg, UniValue::VBOOL);
        rbf = replaceable_arg.isTrue();
    }

    const uint32_t psbt_version = ParsePSBTVersion(request.params[3]["psbt_version"]);

    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], rbf, pwallet->chain());
    FundTransaction(pwallet, rawTx, fee, change_position, request.params[3]);

    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx, psbt_version);

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[4].isNull() ? false : request.params[4].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, 1, false, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);
    return result;
}

// ===========================================================================
// Modern spending RPCs: send, sendall and simulaterawtransaction
//
// The implementation is adapted from Bitcoin Core v24.0
// (src/wallet/rpc/spend.cpp and src/wallet/rpc/wallet.cpp) to the
// DeFiChain wallet layer.
//
// DeFiChain specific restrictions:
//  - Only the DFI UTXO amount (DCT_ID{0}) can be spent. Token amounts are
//    rejected with an explicit error message that points to the token capable
//    account based RPCs.
//  - ERC-55 addresses are not supported.
//  - The `send` RPC keeps the DeFiChain funding semantics (change output,
//    token aware coin selection is restricted to the DFI UTXO) and therefore
//    only accepts the `v24.0` option set without the `v32.0` only extras
//    (`solving_data`, `input_weights`, ...).
// ===========================================================================

/** Description of a single output of the modern spending RPCs. */
struct SpendOutput {
    std::string address; //!< empty for data (OP_RETURN) outputs
    CAmount amount{0};
    bool has_amount{false};
    bool is_data{false};
    CScript script;
};

/**
 * Parse an amount for the modern spending RPCs.
 *
 * Only the DFI UTXO amount is supported. Token amounts ("1.5@2") are rejected
 * with an explicit error message instead of failing with a generic parse
 * error.
 */
static CAmount ParseSpendAmount(const UniValue& value, const std::string& key)
{
    if (value.isStr() && value.get_str().find('@') != std::string::npos) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Token amounts are not supported by this RPC ('%s'). Use the token capable account RPCs (e.g. 'sendtoaddress' with a token amount) instead.", key));
    }
    return AmountFromValue(value);
}

/** Decode and validate a DeFiChain address. */
static CTxDestination ParseSpendDestination(const std::string& address)
{
    CTxDestination dest = DecodeDestination(address);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Invalid address: %s", address));
    }
    RejectErc55Address(GetScriptForDestination(dest));
    return dest;
}

/**
 * Parse the outputs of the `send` RPC.
 *
 * Accepts a dictionary of "address" -> "amount" pairs (an array of single key
 * objects is accepted as well) and an optional "data" key holding hex encoded
 * data for an OP_RETURN output.
 */
static std::vector<SpendOutput> ParseSpendOutputs(const UniValue& outputs_in)
{
    if (outputs_in.isNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, outputs must not be null");
    }

    // Collect the key-value pairs of the outputs. Both a dictionary and an
    // array of single key objects are accepted.
    std::vector<std::pair<std::string, UniValue>> pairs;
    if (outputs_in.isObject()) {
        for (const std::string& key : outputs_in.getKeys()) {
            pairs.emplace_back(key, outputs_in[key]);
        }
    } else if (outputs_in.isArray()) {
        for (const UniValue& output : outputs_in.get_array().getValues()) {
            if (!output.isObject() || output.size() != 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, key-value pair must be an object with exactly one key");
            }
            for (const std::string& key : output.getKeys()) {
                pairs.emplace_back(key, output[key]);
            }
        }
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, outputs must be an object or an array of single key objects");
    }

    std::vector<SpendOutput> result;
    std::set<CTxDestination> destinations;
    bool has_data{false};
    for (const auto& pair : pairs) {
        const std::string& key = pair.first;
        const UniValue& value = pair.second;
        if (key == "data") {
            if (has_data) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicate key: data");
            }
            has_data = true;
            SpendOutput output;
            output.is_data = true;
            output.script = CScript() << OP_RETURN << ParseHexV(value, "Data");
            result.push_back(std::move(output));
            continue;
        }
        SpendOutput output;
        output.address = key;
        const CTxDestination dest = ParseSpendDestination(key);
        if (!destinations.insert(dest).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated address: %s", key));
        }
        output.script = GetScriptForDestination(dest);
        output.amount = ParseSpendAmount(value, key);
        output.has_amount = true;
        result.push_back(std::move(output));
    }
    if (result.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, at least one output is required");
    }
    return result;
}

/**
 * Parse the recipients of the `sendall` RPC.
 *
 * Every recipient is either an address string (the remaining funds are split
 * between those addresses) or an object with a single "address" -> "amount"
 * pair.
 */
static std::vector<SpendOutput> ParseSendAllRecipients(const UniValue& recipients_in)
{
    if (!recipients_in.isArray() || recipients_in.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, recipients must be a non-empty array");
    }

    std::vector<SpendOutput> result;
    std::set<std::string> seen;
    for (const UniValue& recipient : recipients_in.get_array().getValues()) {
        SpendOutput output;
        if (recipient.isStr()) {
            output.address = recipient.get_str();
        } else if (recipient.isObject() && recipient.size() == 1) {
            const std::string& key = recipient.getKeys()[0];
            output.address = key;
            output.amount = ParseSpendAmount(recipient[key], key);
            output.has_amount = true;
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, recipient must be an address string or an object with a single address key");
        }
        const CTxDestination dest = ParseSpendDestination(output.address);
        output.script = GetScriptForDestination(dest);
        if (!seen.insert(output.address).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated address: %s", output.address));
        }
        result.push_back(std::move(output));
    }
    return result;
}

/** Parse the inputs of the modern spending RPCs. */
static std::vector<CTxIn> ParseSpendInputs(const UniValue& inputs_in, bool rbf, uint32_t locktime)
{
    std::vector<CTxIn> inputs;
    if (inputs_in.isNull()) {
        return inputs;
    }
    for (const UniValue& input : inputs_in.get_array().getValues()) {
        const uint256 txid = ParseHashO(input, "txid");
        const UniValue& vout_v = input.find_value("vout");
        if (!vout_v.isNum()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        }
        const int vout = vout_v.getInt<int>();
        if (vout < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }
        uint32_t nSequence;
        if (rbf) {
            nSequence = (CTxIn::SEQUENCE_FINAL - 2);
        } else if (locktime) {
            nSequence = CTxIn::SEQUENCE_FINAL - 1;
        } else {
            nSequence = CTxIn::SEQUENCE_FINAL;
        }
        const UniValue& sequence_v = input.find_value("sequence");
        if (sequence_v.isNum()) {
            const int64_t sequence = sequence_v.getInt<int64_t>();
            if (sequence < 0 || sequence > CTxIn::SEQUENCE_FINAL) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, sequence number is out of range");
            }
            nSequence = static_cast<uint32_t>(sequence);
        }
        inputs.emplace_back(COutPoint(txid, vout), CScript(), nSequence);
    }
    return inputs;
}

/**
 * Update the coin control with the fee estimation settings.
 *
 * Ported from Bitcoin Core v24.0 (`SetFeeEstimateMode`).
 */
static void SetFeeEstimateMode(const CWallet& wallet, CCoinControl& cc, const UniValue& conf_target, const UniValue& estimate_mode, const UniValue& fee_rate, bool override_min_fee)
{
    if (!fee_rate.isNull()) {
        if (!conf_target.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and fee_rate. Please provide either a confirmation target in blocks for automatic fee estimation, or an explicit fee rate.");
        }
        if (!estimate_mode.isNull()) {
            // "unset" is the default estimate mode and does not conflict with
            // an explicit fee rate.
            if (ToLower(estimate_mode.get_str()) != "unset") {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and fee_rate");
            }
        }
        cc.m_feerate = CFeeRate{AmountFromValue(fee_rate)};
        if (override_min_fee) cc.fOverrideFeeRate = true;
        // Default RBF to true for an explicit fee rate, if unset.
        if (!cc.m_signal_bip125_rbf) cc.m_signal_bip125_rbf = true;
        return;
    }
    if (!estimate_mode.isNull() && !FeeModeFromString(estimate_mode.get_str(), cc.m_fee_mode)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
    }
    if (!conf_target.isNull()) {
        cc.m_confirm_target = ParseConfirmTarget(conf_target, wallet.chain().estimateMaxBlocks());
    }
}

/**
 * Merge the fee estimation arguments into the options object.
 *
 * Ported from Bitcoin Core v24.0 (`InterpretFeeEstimationInstructions`).
 */
static void InterpretFeeEstimationInstructions(const UniValue& conf_target, const UniValue& estimate_mode, const UniValue& fee_rate, UniValue& options)
{
    if (options.exists("conf_target") || options.exists("estimate_mode")) {
        if (!conf_target.isNull() || !estimate_mode.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass conf_target and estimate_mode either as arguments or in the options object, but not both");
        }
    } else {
        options.pushKV("conf_target", conf_target);
        options.pushKV("estimate_mode", estimate_mode);
    }
    if (options.exists("fee_rate")) {
        if (!fee_rate.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass the fee_rate either as an argument, or in the options object, but not both");
        }
    } else {
        options.pushKV("fee_rate", fee_rate);
    }
}

/**
 * Reject the outdated camelCase option names.
 *
 * Ported from Bitcoin Core v24.0 (`PreventOutdatedOptions`).
 */
static void PreventOutdatedOptions(const UniValue& options)
{
    if (options.exists("feeRate")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use fee_rate instead of feeRate");
    }
    if (options.exists("changeAddress")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_address instead of changeAddress");
    }
    if (options.exists("changePosition")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_position instead of changePosition");
    }
    if (options.exists("includeWatching")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use include_watching instead of includeWatching");
    }
    if (options.exists("lockUnspents")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use lock_unspents instead of lockUnspents");
    }
    if (options.exists("subtractFeeFromOutputs")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use subtract_fee_from_outputs instead of subtractFeeFromOutputs");
    }
}

/**
 * Sign, finalize and (optionally) commit the transaction.
 *
 * Ported from Bitcoin Core v24.0 (`FinishTransaction`). Returns the same
 * result object as upstream: `complete`, `txid`, `hex` and `psbt`.
 */
static UniValue FinishTransaction(CWallet* const pwallet, const UniValue& options, const CMutableTransaction& rawTx)
{
    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // First fill the transaction with our data without signing, so that
    // external signers are not asked to sign more than once.
    bool complete{false};
    const TransactionError err{FillPSBT(pwallet, psbtx, complete, SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }
    const TransactionError sign_err{FillPSBT(pwallet, psbtx, complete, SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true)};
    if (sign_err != TransactionError::OK) {
        throw JSONRPCTransactionError(sign_err);
    }

    CMutableTransaction mtx;
    complete = FinalizeAndExtractPSBT(psbtx, mtx);

    UniValue result(UniValue::VOBJ);

    const bool psbt_opt_in{options.exists("psbt") && options["psbt"].get_bool()};
    const bool add_to_wallet{options.exists("add_to_wallet") ? options["add_to_wallet"].get_bool() : true};
    if (psbt_opt_in || !complete || !add_to_wallet) {
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    if (complete) {
        const std::string hex{EncodeHexTx(CTransaction(mtx))};
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        result.pushKV("txid", tx->GetHash().GetHex());
        if (add_to_wallet && !psbt_opt_in) {
            CValidationState state;
            if (!pwallet->CommitTransaction(tx, {}, state)) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Error: The transaction was rejected! Reason given: %s", FormatStateMessage(state)));
            }
        } else {
            result.pushKV("hex", hex);
        }
    }
    result.pushKV("complete", complete);

    return result;
}

/** Parse the `subtract_fee_from_outputs` option. */
static std::set<int> ParseSubtractFeeFromOutputs(const UniValue& subtract_fee_from_outputs, size_t num_outputs)
{
    std::set<int> set_subtract_fee_from_outputs;
    if (subtract_fee_from_outputs.isNull()) {
        return set_subtract_fee_from_outputs;
    }
    for (unsigned int idx = 0; idx < subtract_fee_from_outputs.size(); idx++) {
        const int pos = subtract_fee_from_outputs[idx].getInt<int>();
        if (set_subtract_fee_from_outputs.count(pos)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        }
        if (pos < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        }
        if (pos >= int(num_outputs)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        }
        set_subtract_fee_from_outputs.insert(pos);
    }
    return set_subtract_fee_from_outputs;
}

static UniValue send(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"send",
        "\nEXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSend a transaction. Only the DFI UTXO amount is supported, token amounts are rejected.\n",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs (key-value pairs), where none of the keys are duplicated.\n"
                    "That is, each address can only appear once and there can only be one 'data' object.\n"
                    "For convenience, a dictionary, which holds the key-value pairs directly, is also accepted.",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the defi address, the value (float or string) is the amount in " + CURRENCY_UNIT},
                        },
                    },
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                        },
                    },
                },
            },
            {"conf_target", RPCArg::Type::NUM, /* default */ "wallet -txconfirmtarget", "Confirmation target in blocks"},
            {"estimate_mode", RPCArg::Type::STR, /* default */ "unset", "The fee estimate mode, must be one of (case insensitive):\n"
             "\"UNSET\"\n\"ECONOMICAL\"\n\"CONSERVATIVE\""},
            {"fee_rate", RPCArg::Type::AMOUNT, /* default */ "not set, fall back to wallet fee estimation", "Specify a fee rate in " + CURRENCY_UNIT + "/vB."},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                {
                    {"add_inputs", RPCArg::Type::BOOL, /* default */ "false when \"inputs\" are specified, true otherwise", "Automatically include coins from the wallet to cover the target amount.\n"},
                    {"include_unsafe", RPCArg::Type::BOOL, /* default */ "false", "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                    {"add_to_wallet", RPCArg::Type::BOOL, /* default */ "true", "When false, returns a serialized transaction which will not be added to the wallet or broadcast"},
                    {"change_address", RPCArg::Type::STR, /* default */ "automatic", "The defi address to receive the change"},
                    {"change_position", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                    {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if change_address is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                    {"fee_rate", RPCArg::Type::AMOUNT, /* default */ "not set, fall back to wallet fee estimation", "Specify a fee rate in " + CURRENCY_UNIT + "/vB."},
                    {"include_watching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only.\n"
                                          "Only solvable inputs can be used."},
                    {"inputs", RPCArg::Type::ARR, /* default */ "[]", "Specify inputs instead of adding them automatically. A JSON array of JSON objects",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"lock_unspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                    {"psbt", RPCArg::Type::BOOL, /* default */ "automatic", "Always return a PSBT, implies add_to_wallet=false."},
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable."},
                    {"subtract_fee_from_outputs", RPCArg::Type::ARR, /* default */ "[]", "Outputs to subtract the fee from, specified as integer indices.\n"
                    "The fee will be equally deducted from the amount of each specified output.\n"
                    "Those recipients will receive less defis than you enter in their corresponding amount field.\n"
                    "If no outputs are specified here, the sender pays the fee.",
                        {
                            {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                        },
                    },
                },
                "options"},
        },
        RPCResult{
            "{\n"
            "  \"complete\": true|false, (boolean) If the transaction has a complete set of signatures\n"
            "  \"txid\": \"txid\",        (string, optional) The transaction id for the send. Only 1 transaction is created regardless of the number of addresses.\n"
            "  \"hex\": \"hex\",          (string, optional) If add_to_wallet is false, the hex-encoded raw transaction with signature(s)\n"
            "  \"psbt\": \"psbt\",        (string, optional) If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction\n"
            "}\n"
        },
        RPCExamples{
            "\nSend 0.1 DFI with a fee rate of 1 " + CURRENCY_UNIT + "/vB\n"
            + HelpExampleCli("send", "'{\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\": 0.1}' null \"unset\" 1")
        },
    }.Check(request);

    RPCTypeCheck(request.params, {
        UniValueType(), // outputs (ARR or OBJ, checked later)
        UniValue::VNUM, // conf_target
        UniValue::VSTR, // estimate_mode
        UniValueType(), // fee_rate, will be checked by AmountFromValue() in SetFeeEstimateMode()
        UniValue::VOBJ, // options
        }, true
    );

    UniValue options{request.params[4].isNull() ? UniValue::VOBJ : request.params[4]};
    InterpretFeeEstimationInstructions(/*conf_target=*/request.params[1], /*estimate_mode=*/request.params[2], /*fee_rate=*/request.params[3], options);
    PreventOutdatedOptions(options);
    RPCTypeCheckObj(options,
        {
            {"add_inputs", UniValueType(UniValue::VBOOL)},
            {"include_unsafe", UniValueType(UniValue::VBOOL)},
            {"add_to_wallet", UniValueType(UniValue::VBOOL)},
            {"change_address", UniValueType(UniValue::VSTR)},
            {"change_position", UniValueType(UniValue::VNUM)},
            {"change_type", UniValueType(UniValue::VSTR)},
            {"fee_rate", UniValueType()},
            {"include_watching", UniValueType(UniValue::VBOOL)},
            {"inputs", UniValueType(UniValue::VARR)},
            {"locktime", UniValueType(UniValue::VNUM)},
            {"lock_unspents", UniValueType(UniValue::VBOOL)},
            {"psbt", UniValueType(UniValue::VBOOL)},
            {"replaceable", UniValueType(UniValue::VBOOL)},
            {"subtract_fee_from_outputs", UniValueType(UniValue::VARR)},
            {"conf_target", UniValueType(UniValue::VNUM)},
            {"estimate_mode", UniValueType(UniValue::VSTR)},
        },
        true, true);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    // Outputs
    const std::vector<SpendOutput> outputs = ParseSpendOutputs(request.params[0]);
    for (const SpendOutput& output : outputs) {
        if (!output.has_amount && !output.is_data) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, every output of 'send' must specify an amount");
        }
    }

    // Fee estimation
    CCoinControl coin_control;
    SetFeeEstimateMode(*pwallet, coin_control, options["conf_target"], options["estimate_mode"], options["fee_rate"], /*override_min_fee=*/false);
    coin_control.fAllowWatchOnly = ParseIncludeWatchonly(options["include_watching"], *pwallet);
    if (options.exists("include_unsafe")) {
        coin_control.m_include_unsafe_inputs = options["include_unsafe"].get_bool();
    }
    // Only the DFI UTXO can be spent by this RPC.
    coin_control.m_tokenFilter = {DCT_ID{0}};
    if (options.exists("change_address")) {
        coin_control.destChange = ParseSpendDestination(options["change_address"].get_str());
    }
    if (options.exists("change_type")) {
        if (options.exists("change_address")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both change_address and change_type options");
        }
        coin_control.m_change_type = pwallet->m_default_change_type;
        if (!ParseOutputType(options["change_type"].get_str(), *coin_control.m_change_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
        }
    }
    if (options.exists("replaceable")) {
        coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
    }

    // Transaction version and locktime
    uint32_t locktime{0};
    if (options.exists("locktime")) {
        const int64_t nLockTime = options["locktime"].getInt<int64_t>();
        if (nLockTime < 0 || nLockTime > LOCKTIME_MAX) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, locktime out of range");
        }
        locktime = static_cast<uint32_t>(nLockTime);
    }
    const bool rbf{options.exists("replaceable") ? options["replaceable"].get_bool() : pwallet->m_signal_rbf};

    // Inputs
    const std::vector<CTxIn> inputs = ParseSpendInputs(options["inputs"], rbf, locktime);
    const bool add_inputs{options.exists("add_inputs") ? options["add_inputs"].get_bool() : inputs.empty()};

    CMutableTransaction rawTx;
    {
        auto locked_chain = pwallet->chain().lock();
        rawTx = CMutableTransaction(GetTransactionVersion(locked_chain->getHeight().value_or(-1)));
    }
    rawTx.nLockTime = locktime;
    for (const SpendOutput& output : outputs) {
        rawTx.vout.emplace_back(output.amount, output.script);
    }
    rawTx.vin = inputs;

    // Change position
    int change_position{-1};
    if (options.exists("change_position")) {
        change_position = options["change_position"].getInt<int>();
        if (change_position < 0 || change_position > int(rawTx.vout.size())) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, change_position out of bounds");
        }
    }

    // Fee subtraction
    const std::set<int> set_subtract_fee_from_outputs = ParseSubtractFeeFromOutputs(options["subtract_fee_from_outputs"], rawTx.vout.size());
    const bool lock_unspents{options.exists("lock_unspents") ? options["lock_unspents"].get_bool() : false};

    // Fund the transaction
    CAmount fee{0};
    std::string strFailReason;
    if (!pwallet->FundTransaction(rawTx, fee, change_position, strFailReason, lock_unspents, set_subtract_fee_from_outputs, coin_control, CoinSelectionOptions::CreateDefault(), add_inputs)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return FinishTransaction(pwallet, options, rawTx);
}

static UniValue sendall(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"sendall",
        "EXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSpend the value of all (or specific) confirmed UTXOs in the wallet to one or more recipients.\n"
        "Unconfirmed inbound UTXOs and locked UTXOs will not be spent. Sendall will respect the avoid_reuse wallet flag.\n"
        "Only the DFI UTXO amount is supported, token amounts are rejected.\n",
        {
            {"recipients", RPCArg::Type::ARR, RPCArg::Optional::NO, "The sendall destinations. Each address may only appear once.\n"
                "Optionally some recipients can be specified with an amount to perform payments, but at least one address must appear without a specified amount.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "A defi address which receives an equal share of the unspecified amount."},
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the defi address, the value (float or string) is the amount in " + CURRENCY_UNIT},
                        },
                    },
                },
            },
            {"conf_target", RPCArg::Type::NUM, /* default */ "wallet -txconfirmtarget", "Confirmation target in blocks"},
            {"estimate_mode", RPCArg::Type::STR, /* default */ "unset", "The fee estimate mode, must be one of (case insensitive):\n"
             "\"UNSET\"\n\"ECONOMICAL\"\n\"CONSERVATIVE\""},
            {"fee_rate", RPCArg::Type::AMOUNT, /* default */ "not set, fall back to wallet fee estimation", "Specify a fee rate in " + CURRENCY_UNIT + "/vB."},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                {
                    {"add_to_wallet", RPCArg::Type::BOOL, /* default */ "true", "When false, returns a serialized transaction without broadcasting or adding it to the wallet"},
                    {"fee_rate", RPCArg::Type::AMOUNT, /* default */ "not set, fall back to wallet fee estimation", "Specify a fee rate in " + CURRENCY_UNIT + "/vB."},
                    {"include_watching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only.\n"
                                          "Only solvable inputs can be used."},
                    {"inputs", RPCArg::Type::ARR, /* default */ "[]", "Use exactly the specified inputs to build the transaction. Specifying inputs is incompatible with send_max.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, /* default */ "depends on the value of the 'replaceable' and 'locktime' arguments", "The sequence number"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"lock_unspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                    {"minconf", RPCArg::Type::NUM, /* default */ "0", "Only include UTXOs with at least this many confirmations"},
                    {"maxconf", RPCArg::Type::NUM, /* default */ "wallet default", "Only include UTXOs with at most this many confirmations"},
                    {"psbt", RPCArg::Type::BOOL, /* default */ "automatic", "Always return a PSBT, implies add_to_wallet=false."},
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable."},
                    {"send_max", RPCArg::Type::BOOL, /* default */ "false", "When true, only use UTXOs that can pay for their own fees to maximize the output amount. When 'false' (default), no UTXO is left behind. send_max is incompatible with providing specific inputs."},
                },
                "options"},
        },
        RPCResult{
            "{\n"
            "  \"complete\": true|false, (boolean) If the transaction has a complete set of signatures\n"
            "  \"txid\": \"txid\",        (string, optional) The transaction id for the send. Only 1 transaction is created regardless of the number of addresses.\n"
            "  \"hex\": \"hex\",          (string, optional) If add_to_wallet is false, the hex-encoded raw transaction with signature(s)\n"
            "  \"psbt\": \"psbt\",        (string, optional) If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction\n"
            "}\n"
        },
        RPCExamples{
            "\nSpend all UTXOs from the wallet with a fee rate of 1 " + CURRENCY_UNIT + "/vB\n"
            + HelpExampleCli("sendall", "'[\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"]' null \"unset\" 1")
        },
    }.Check(request);

    RPCTypeCheck(request.params, {
        UniValue::VARR, // recipients
        UniValue::VNUM, // conf_target
        UniValue::VSTR, // estimate_mode
        UniValueType(), // fee_rate, will be checked by AmountFromValue() in SetFeeEstimateMode()
        UniValue::VOBJ, // options
        }, true
    );

    UniValue options{request.params[4].isNull() ? UniValue::VOBJ : request.params[4]};
    InterpretFeeEstimationInstructions(/*conf_target=*/request.params[1], /*estimate_mode=*/request.params[2], /*fee_rate=*/request.params[3], options);
    PreventOutdatedOptions(options);
    RPCTypeCheckObj(options,
        {
            {"add_to_wallet", UniValueType(UniValue::VBOOL)},
            {"fee_rate", UniValueType()},
            {"include_watching", UniValueType(UniValue::VBOOL)},
            {"inputs", UniValueType(UniValue::VARR)},
            {"locktime", UniValueType(UniValue::VNUM)},
            {"lock_unspents", UniValueType(UniValue::VBOOL)},
            {"minconf", UniValueType(UniValue::VNUM)},
            {"maxconf", UniValueType(UniValue::VNUM)},
            {"psbt", UniValueType(UniValue::VBOOL)},
            {"replaceable", UniValueType(UniValue::VBOOL)},
            {"send_max", UniValueType(UniValue::VBOOL)},
            {"conf_target", UniValueType(UniValue::VNUM)},
            {"estimate_mode", UniValueType(UniValue::VSTR)},
        },
        true, true);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    // Recipients
    const std::vector<SpendOutput> recipients = ParseSendAllRecipients(request.params[0]);
    size_t addresses_without_amount{0};
    for (const SpendOutput& recipient : recipients) {
        if (!recipient.has_amount) {
            ++addresses_without_amount;
        }
    }
    if (addresses_without_amount == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide at least one address without a specified amount");
    }

    // Fee estimation
    CCoinControl coin_control;
    SetFeeEstimateMode(*pwallet, coin_control, options["conf_target"], options["estimate_mode"], options["fee_rate"], /*override_min_fee=*/false);
    coin_control.fAllowWatchOnly = ParseIncludeWatchonly(options["include_watching"], *pwallet);
    // Only the DFI UTXO can be spent by this RPC.
    coin_control.m_tokenFilter = {DCT_ID{0}};
    if (options.exists("minconf")) {
        coin_control.m_min_depth = options["minconf"].getInt<int>();
    }
    if (options.exists("maxconf")) {
        coin_control.m_max_depth = options["maxconf"].getInt<int>();
    }
    if (options.exists("replaceable")) {
        coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
    }

    FeeCalculation fee_calc_out;
    const CFeeRate fee_rate{GetMinimumFeeRate(*pwallet, coin_control, &fee_calc_out)};
    // Do not, ever, assume that it's fine to change the fee rate if the user has explicitly provided one
    if (coin_control.m_feerate && fee_rate > *coin_control.m_feerate) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Fee rate (%s) is lower than the minimum fee rate setting (%s)", coin_control.m_feerate->ToString(), fee_rate.ToString()));
    }
    if (fee_calc_out.reason == FeeReason::FALLBACK && !pwallet->m_allow_fallback_fee) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Fee estimation failed. Fallbackfee is disabled. Wait a few blocks or enable -fallbackfee.");
    }

    uint32_t locktime{0};
    if (options.exists("locktime")) {
        const int64_t nLockTime = options["locktime"].getInt<int64_t>();
        if (nLockTime < 0 || nLockTime > LOCKTIME_MAX) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, locktime out of range");
        }
        locktime = static_cast<uint32_t>(nLockTime);
    }
    const bool rbf{options.exists("replaceable") ? options["replaceable"].get_bool() : pwallet->m_signal_rbf};

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    CMutableTransaction rawTx{GetTransactionVersion(locked_chain->getHeight().value_or(-1))};
    rawTx.nLockTime = locktime;
    for (const SpendOutput& recipient : recipients) {
        rawTx.vout.emplace_back(recipient.amount, recipient.script);
    }

    // Inputs
    CAmount total_input_value{0};
    const bool send_max{options.exists("send_max") ? options["send_max"].get_bool() : false};
    if (options.exists("inputs") && options.exists("send_max")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot combine send_max with specific inputs.");
    } else if (options.exists("inputs")) {
        for (const CTxIn& input : ParseSpendInputs(options["inputs"], rbf, locktime)) {
            if (pwallet->IsSpent(*locked_chain, input.prevout.hash, input.prevout.n)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not available. UTXO (%s:%d) was already spent.", input.prevout.hash.ToString(), input.prevout.n));
            }
            const CWalletTx* tx{pwallet->GetWalletTx(input.prevout.hash)};
            if (!tx || input.prevout.n >= tx->tx->vout.size() || !(pwallet->IsMine(tx->tx->vout[input.prevout.n]) & (coin_control.fAllowWatchOnly ? ISMINE_ALL : ISMINE_SPENDABLE))) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not found. UTXO (%s:%d) is not part of wallet.", input.prevout.hash.ToString(), input.prevout.n));
            }
            if (tx->tx->vout[input.prevout.n].nTokenId != DCT_ID{0}) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input (%s:%d) is a token output. Token amounts are not supported by this RPC.", input.prevout.hash.ToString(), input.prevout.n));
            }
            total_input_value += tx->tx->vout[input.prevout.n].nValue;
            rawTx.vin.push_back(input);
        }
    } else {
        std::vector<COutput> vCoins;
        pwallet->AvailableCoins(*locked_chain, vCoins, /*fOnlySafe=*/true, &coin_control, /*nMinimumAmount=*/0, /*nMaximumAmount=*/MAX_MONEY, /*nMinimumSumAmount=*/MAX_MONEY, /*nMaximumCount=*/0);
        for (const COutput& output : vCoins) {
            const CTxOut& txout = output.tx->tx->vout[output.i];
            if (txout.nTokenId != DCT_ID{0}) {
                // Only the DFI UTXO is swept by this RPC.
                continue;
            }
            if (!output.fSpendable) {
                continue;
            }
            if (send_max) {
                if (output.nInputBytes <= 0) {
                    continue;
                }
                if (fee_rate.GetFee(output.nInputBytes) > txout.nValue) {
                    continue;
                }
            }
            rawTx.vin.emplace_back(COutPoint(output.tx->GetHash(), output.i), CScript(), rbf ? (CTxIn::SEQUENCE_FINAL - 2) : CTxIn::SEQUENCE_FINAL);
            total_input_value += txout.nValue;
        }
    }

    // Estimate the final size of the transaction
    const int64_t tx_size{CalculateMaximumSignedTxSize(CTransaction(rawTx), pwallet)};
    if (tx_size < 0) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Signing transaction failed");
    }
    const CAmount fee_from_size{fee_rate.GetFee(tx_size)};
    const CAmount effective_value{total_input_value - fee_from_size};

    if (fee_from_size > pwallet->m_default_max_tx_fee) {
        throw JSONRPCError(RPC_WALLET_ERROR, TransactionErrorString(TransactionError::MAX_FEE_EXCEEDED));
    }

    if (effective_value <= 0) {
        if (send_max) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Total value of UTXO pool too low to pay for transaction, try using lower feerate.");
        } else {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Total value of UTXO pool too low to pay for transaction. Try using lower feerate or excluding uneconomic UTXOs with 'send_max' option.");
        }
    }

    CAmount output_amounts_claimed{0};
    for (const CTxOut& out : rawTx.vout) {
        output_amounts_claimed += out.nValue;
    }

    if (output_amounts_claimed > total_input_value) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Assigned more value to outputs than available funds.");
    }

    const CAmount remainder{effective_value - output_amounts_claimed};
    if (remainder < 0) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds for fees after creating specified outputs.");
    }

    const CAmount per_output_without_amount{remainder / static_cast<CAmount>(addresses_without_amount)};
    bool gave_remaining_to_first{false};
    for (size_t i = 0; i < rawTx.vout.size(); ++i) {
        if (!recipients[i].has_amount) {
            CAmount value = per_output_without_amount;
            if (!gave_remaining_to_first) {
                value += remainder % static_cast<CAmount>(addresses_without_amount);
                gave_remaining_to_first = true;
            }
            rawTx.vout[i].nValue = value;
            if (IsDust(rawTx.vout[i], rawTx.nVersion, pwallet->chain().relayDustFee())) {
                // Dynamically generated output amount is dust
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Dynamically assigned remainder results in dust output.");
            }
        } else {
            if (IsDust(rawTx.vout[i], rawTx.nVersion, pwallet->chain().relayDustFee())) {
                // Specified output amount is dust
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Specified output amount to %s is below dust threshold.", recipients[i].address));
            }
        }
    }

    if (options.exists("lock_unspents") && options["lock_unspents"].get_bool()) {
        for (const CTxIn& txin : rawTx.vin) {
            pwallet->LockCoin(txin.prevout);
        }
    }

    return FinishTransaction(pwallet, options, rawTx);
}

static UniValue simulaterawtransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"simulaterawtransaction",
        "\nCalculate the balance change resulting in the signing and broadcasting of the given transaction(s).\n"
        "Only the DFI UTXO balance is taken into account, token amounts are excluded.\n",
        {
            {"rawtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "An array of hex strings of raw transactions.\n",
                {
                    {"rawtx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Options",
                {
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses (see RPC importaddress)"},
                },
            },
        },
        RPCResult{
            "{\n"
            "  \"balance_change\": n,   (numeric) The DFI wallet balance change (negative means decrease). Token amounts are not included.\n"
            "}\n"
        },
        RPCExamples{
            HelpExampleCli("simulaterawtransaction", "[\"myhex\"]")
        },
    }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VARR, UniValue::VOBJ}, true);

    UniValue include_watchonly(UniValue::VNULL);
    if (request.params[1].isObject()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"include_watchonly", UniValueType(UniValue::VBOOL)},
            },
            true, true);

        include_watchonly = options["include_watchonly"];
    }

    isminefilter filter = ISMINE_SPENDABLE;
    if (ParseIncludeWatchonly(include_watchonly, *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    LOCK(pwallet->cs_wallet);

    const auto& txs = request.params[0].get_array();
    CAmount changes{0};
    std::map<COutPoint, CAmount> new_utxos; // UTXOs that were made available in the transaction array
    std::set<COutPoint> spent;

    for (size_t i = 0; i < txs.size(); ++i) {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, txs[i].get_str(), /*try_no_witness=*/true, /*try_witness=*/true)) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Transaction hex string decoding failure.");
        }

        // Fetch previous transactions (inputs)
        std::map<COutPoint, Coin> coins;
        for (const CTxIn& txin : mtx.vin) {
            coins[txin.prevout]; // Create empty map entry keyed by prevout.
        }
        pwallet->chain().findCoins(coins);

        // Fetch debit; we are *spending* these; if the transaction is signed and
        // broadcast, we will lose everything in these
        for (const auto& txin : mtx.vin) {
            const auto& outpoint = txin.prevout;
            if (spent.count(outpoint)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Transaction(s) are spending the same output more than once");
            }
            if (new_utxos.count(outpoint)) {
                changes -= new_utxos.at(outpoint);
                new_utxos.erase(outpoint);
            } else {
                const Coin& coin = coins.at(outpoint);
                if (coin.out.scriptPubKey.empty()) {
                    // The coin is either unknown or already spent.
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "One or more transaction inputs are missing or have been spent already");
                }
                const CTokenAmount debit = pwallet->GetDebit(txin, filter);
                // Only the DFI UTXO amount is part of the balance change. Token
                // amounts are excluded because DFI tokens are account based.
                if (debit.nTokenId == DCT_ID{0}) {
                    changes -= debit.nValue;
                }
            }
            spent.insert(outpoint);
        }

        // Iterate over outputs; we are *receiving* these, if the wallet considers
        // them "mine"; if the transaction is signed and broadcast, we will receive
        // everything in these.
        // Also populate new_utxos in case these are spent in later transactions.
        const auto& hash = mtx.GetHash();
        for (size_t i = 0; i < mtx.vout.size(); ++i) {
            const auto& txout = mtx.vout[i];
            const bool is_mine = 0 < (pwallet->IsMine(txout) & filter);
            // Token outputs are not part of the DFI UTXO balance.
            const CAmount value = (is_mine && txout.nTokenId == DCT_ID{0}) ? txout.nValue : 0;
            changes += new_utxos[COutPoint(hash, i)] = value;
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("balance_change", ValueFromAmount(changes));

    return result;
}

enum class AddressConversionType {
    Auto,
    DVMToEVMAddress,
    EVMToDVMAddress,
};

static int AddressConversionTypeCount = 3;

UniValue addressmap(const JSONRPCRequest &request) {
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    RPCHelpMan{
        "addressmap",
        "Give the equivalent of an address from EVM to DVM and versa\n",
        {
            {"input", RPCArg::Type::STR, RPCArg::Optional::NO, "DVM address or EVM address"},
            {"type", RPCArg::Type::NUM, RPCArg::Optional::NO, "Map types: \n\
                            1 - Address format: DFI -> ERC55 \n\
                            2 - Address format: ERC55 -> DFI \n"}
        },
        RPCResult{
            "{\n"
            "    input :    \"address\",         (string) The input address to be converted\n"
            "    type :     \"map type\"\n       (numeric) address map type indicator"
            "    format : { \n"
            "       bech32: \"address\"\n        (string, optional) output converted address"
            "       p2pkh:  \"address\"\n        (string, optional) output converted address"
            "       erc55 : \"address\"\n        ..."
            "    }]\n"
            "}\n"
        },
        RPCExamples{HelpExampleCli("addressmap", R"('"<address>"' 1)")},
    }
        .Check(request);

    auto throwInvalidParamType = []() { throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid type parameter")); };
    auto throwInvalidAddressFmt = []() { throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid address format")); };

    const std::string input = request.params[0].get_str();

    int typeInt = request.params[1].getInt<int>();
    if (typeInt < 0 || typeInt >= AddressConversionTypeCount) {
        throwInvalidParamType();
    }

    CTxDestination dest = DecodeDestination(input);

    // auto infer type
    if (typeInt == 0) {
        if (dest.index() == WitV0KeyHashType || dest.index() == PKHashType) {
            typeInt = 1;
        } else if (dest.index() == WitV16KeyEthHashType) {
            typeInt = 2;
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unsupported type or unable to determine conversion type automatically from the input");
        }
    }

    const auto type = static_cast<AddressConversionType>(typeInt);

    UniValue format(UniValue::VOBJ);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("input", input);
    ret.pushKV("type", typeInt);

    switch (type) {
        case AddressConversionType::DVMToEVMAddress: {
            if (dest.index() != WitV0KeyHashType && dest.index() != PKHashType) {
                throwInvalidAddressFmt();
            }
            CPubKey key = AddrToPubKey(pwallet, input);
            if (key.IsCompressed()) {
                key.Decompress();
            }
            format.pushKV("erc55", EncodeDestination(WitnessV16EthHash(key)));
            break;
        }
        case AddressConversionType::EVMToDVMAddress: {
            if (dest.index() != WitV16KeyEthHashType) {
                throwInvalidAddressFmt();
            }
            CPubKey key = AddrToPubKey(pwallet, input);
            auto [uncomp, comp] = GetBothPubkeyCompressions(key);
            format.pushKV("bech32", EncodeDestination(WitnessV0KeyHash(comp)));
            format.pushKV("legacy", EncodeDestination(PKHash(uncomp)));
            break;
        }
        default:
            throwInvalidParamType();
    }

    ret.pushKV("format", format);
    return ret;
}

namespace {
/**
 * A signing provider view on a wallet that only exposes genuine key origins.
 *
 * CWallet::GetKeyOrigin() fabricates a fingerprint for every key, even for the
 * keys without origin information. This adapter returns false in that case so
 * that a fabricated origin can never reach an external device.
 */
class WalletSigningProvider : public SigningProvider
{
public:
    explicit WalletSigningProvider(const CWallet& wallet) : m_wallet(wallet) {}

    bool GetPubKey(const CKeyID& address, CPubKey& pubkey) const override { return m_wallet.GetPubKey(address, pubkey); }
    bool GetKey(const CKeyID& address, CKey& key) const override { return m_wallet.GetKey(address, key); }
    bool GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const override { return m_wallet.GetKeyOriginIfPresent(keyid, info); }

private:
    const CWallet& m_wallet;
};

/**
 * Returns the public key of the given destination if the wallet knows it.
 */
bool GetDestinationPubKey(const CWallet& wallet, const CTxDestination& dest, CPubKey& pubkey)
{
    const CScript script = GetScriptForDestination(dest);
    std::vector<std::vector<unsigned char>> data;
    const txnouttype type = Solver(script, data);
    if (type == TX_PUBKEY) {
        pubkey = CPubKey(data[0].begin(), data[0].end());
        return pubkey.IsValid();
    }
    if ((type == TX_PUBKEYHASH || type == TX_WITNESS_V0_KEYHASH) && data[0].size() == 20) {
        return wallet.GetPubKey(CKeyID(uint160(data[0])), pubkey);
    }
    return false;
}

/**
 * Returns the fingerprints of the given signers as a comma separated list.
 */
std::string SignerFingerprintList(const std::vector<ExternalSigner>& signers)
{
    std::string result;
    for (const ExternalSigner& signer : signers) {
        if (!result.empty()) result += ", ";
        result += signer.m_fingerprint;
    }
    return result;
}
} // namespace

UniValue walletdisplayaddress(const JSONRPCRequest& request)
{
            RPCHelpMan{"walletdisplayaddress",
                "\nDisplay an address of this wallet on an external signer for verification.\n"
                "The address has to be owned by the wallet and the external signer has to be\n"
                "configured with the -signer option of the node.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to display on the external signer."},
                },
                RPCResult{
            "{\n"
            "  \"address\" : \"address\"   (string) The address as confirmed by the signer\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("walletdisplayaddress", "\"bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g\"")
            + HelpExampleRpc("walletdisplayaddress", "\"bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g\"")
                },
            }.Check(request);

    // The build check and the -signer check are performed in a deterministic
    // order, see doc/external-signer.md.
    std::string error;
    const std::string command = GetSignerCommand(error);
    if (command.empty()) {
        throw JSONRPCError(RPC_MISC_ERROR, error);
    }

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    const std::string address = request.params[0].get_str();
    const CTxDestination dest = DecodeDestination(address);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    LOCK(pwallet->cs_wallet);

    if (!IsMineCached(*pwallet, dest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Address %s is not owned by this wallet", address));
    }

    // Resolve the address to a descriptor. The descriptor of a single key with
    // the origin information of the wallet is handed to the device.
    const CScript script = GetScriptForDestination(dest);
    const WalletSigningProvider provider{*pwallet};
    const std::unique_ptr<Descriptor> descriptor = InferDescriptor(script, provider);
    if (!descriptor || !descriptor->IsSolvable()) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Cannot resolve address %s to a descriptor (no public key / origin information in this wallet)", address));
    }
    const std::string descriptor_string = descriptor->ToString();

    // The fingerprint of the device is taken from the origin information of the
    // key. Without a genuine origin the device cannot be selected by its
    // fingerprint, exactly one connected signer is required then.
    std::string fingerprint;
    CPubKey pubkey;
    KeyOriginInfo origin;
    if (GetDestinationPubKey(*pwallet, dest, pubkey) && pwallet->GetKeyOriginIfPresent(pubkey.GetID(), origin)) {
        fingerprint = HexStr(std::begin(origin.fingerprint), std::end(origin.fingerprint));
    }

    // Enumerate the connected signers and select the one of the device.
    std::vector<ExternalSigner> signers;
    try {
        ExternalSigner::Enumerate(command, signers, SignerChainName(Params().NetworkIDString()));
    } catch (const std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
    if (signers.empty()) {
        throw JSONRPCError(RPC_MISC_ERROR, "No external signer connected");
    }

    const ExternalSigner* signer = nullptr;
    if (!fingerprint.empty()) {
        for (const ExternalSigner& candidate : signers) {
            if (candidate.m_fingerprint == fingerprint) {
                signer = &candidate;
                break;
            }
        }
        if (signer == nullptr) {
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("No external signer with fingerprint %s connected (connected: %s)", fingerprint, SignerFingerprintList(signers)));
        }
    } else {
        if (signers.size() > 1) {
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("Multiple external signers connected (%s); connect only one device", SignerFingerprintList(signers)));
        }
        signer = &signers[0];
    }

    // Display the address on the device and validate the answer.
    UniValue signer_result;
    try {
        signer_result = signer->DisplayAddress(descriptor_string);
    } catch (const std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
    const UniValue& signer_error = signer_result.find_value("error");
    if (signer_error.isStr()) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("Signer returned error: %s", signer_error.getValStr()));
    }
    const UniValue& signer_address = signer_result.find_value("address");
    if (!signer_address.isStr()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Signer did not echo address");
    }
    if (signer_address.getValStr() != EncodeDestination(dest)) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("Signer echoed unexpected address %s", signer_address.getValStr()));
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("address", address);
    return ret;
}

UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue importprivkey(const JSONRPCRequest& request);
UniValue importaddress(const JSONRPCRequest& request);
UniValue importpubkey(const JSONRPCRequest& request);
UniValue dumpwallet(const JSONRPCRequest& request);
UniValue importwallet(const JSONRPCRequest& request);
UniValue importprunedfunds(const JSONRPCRequest& request);
UniValue removeprunedfunds(const JSONRPCRequest& request);
UniValue importmulti(const JSONRPCRequest& request);

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                                actor (function)                argNames
    //  --------------------- ------------------------          -----------------------         ----------
    { "rawtransactions",    "fundrawtransaction",               &fundrawtransaction,            {"hexstring","options","iswitness"} },
    { "wallet",             "abandontransaction",               &abandontransaction,            {"txid"} },
    { "wallet",             "abortrescan",                      &abortrescan,                   {} },
    { "wallet",             "addmultisigaddress",               &addmultisigaddress,            {"nrequired","keys","label","address_type"} },
    { "wallet",             "addressmap",                       &addressmap,                    {"input", "type"} },
    { "wallet",             "backupwallet",                     &backupwallet,                  {"destination"} },
    { "wallet",             "bumpfee",                          &bumpfee,                       {"txid", "options"} },
    { "wallet",             "psbtbumpfee",                      &psbtbumpfee,                   {"txid", "options"} },
    { "wallet",             "createwallet",                     &createwallet,                  {"wallet_name", "disable_private_keys", "blank", "passphrase", "avoid_reuse", "dbtype"} },
    { "wallet",             "dumpprivkey",                      &dumpprivkey,                   {"address"}  },
    { "wallet",             "dumpwallet",                       &dumpwallet,                    {"filename"} },
    { "wallet",             "encryptwallet",                    &encryptwallet,                 {"passphrase"} },
    { "wallet",             "getaddressesbylabel",              &getaddressesbylabel,           {"label"} },
    { "wallet",             "getaddressinfo",                   &getaddressinfo,                {"address"} },
    { "wallet",             "getbalance",                       &getbalance,                    {"dummy","minconf","include_watchonly","avoid_reuse", "with_tokens"} },
    { "wallet",             "getnewaddress",                    &getnewaddress,                 {"label","address_type"} },
    { "wallet",             "getrawchangeaddress",              &getrawchangeaddress,           {"address_type"} },
    { "wallet",             "getreceivedbyaddress",             &getreceivedbyaddress,          {"address","minconf"} },
    { "wallet",             "getreceivedbylabel",               &getreceivedbylabel,            {"label","minconf"} },
    { "wallet",             "gettransaction",                   &gettransaction,                {"txid","include_watchonly"} },
    { "wallet",             "getunconfirmedbalance",            &getunconfirmedbalance,         {"with_tokens"} },
    { "wallet",             "getbalances",                      &getbalances,                   {"with_tokens"} },
    { "wallet",             "getwalletinfo",                    &getwalletinfo,                 {"with_tokens"} },
    { "wallet",             "importaddress",                    &importaddress,                 {"address","label","rescan","p2sh"} },
    { "wallet",             "importmulti",                      &importmulti,                   {"requests","options"} },
    { "wallet",             "importdescriptors",                &importdescriptors,             {"requests"} },
    { "wallet",             "importprivkey",                    &importprivkey,                 {"privkey","label","rescan"} },
    { "wallet",             "importprunedfunds",                &importprunedfunds,             {"rawtransaction","txoutproof"} },
    { "wallet",             "importpubkey",                     &importpubkey,                  {"pubkey","label","rescan"} },
    { "wallet",             "importwallet",                     &importwallet,                  {"filename"} },
    { "wallet",             "keypoolrefill",                    &keypoolrefill,                 {"newsize"} },
    { "wallet",             "listaddressgroupings",             &listaddressgroupings,          {} },
    { "wallet",             "listlabels",                       &listlabels,                    {"purpose"} },
    { "wallet",             "listdescriptors",                  &listdescriptors,               {"private"} },
    { "wallet",             "listlockunspent",                  &listlockunspent,               {} },
    { "wallet",             "listreceivedbyaddress",            &listreceivedbyaddress,         {"minconf","include_empty","include_watchonly","address_filter"} },
    { "wallet",             "listreceivedbylabel",              &listreceivedbylabel,           {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listsinceblock",                   &listsinceblock,                {"blockhash","target_confirmations","include_watchonly","include_removed"} },
    { "wallet",             "listtransactions",                 &listtransactions,              {"label|dummy","count","skip","include_watchonly","exclude_custom_tx"} },
    { "wallet",             "listunspent",                      &listunspent,                   {"minconf","maxconf","addresses","include_unsafe","query_options"} },
    { "wallet",             "listwalletdir",                    &listwalletdir,                 {} },
    { "wallet",             "listwallets",                      &listwallets,                   {} },
    { "wallet",             "loadwallet",                       &loadwallet,                    {"filename"} },
    { "wallet",             "lockunspent",                      &lockunspent,                   {"unlock","transactions"} },
    { "wallet",             "removeprunedfunds",                &removeprunedfunds,             {"txid"} },
    { "wallet",             "rescanblockchain",                 &rescanblockchain,              {"start_height", "stop_height"} },
    { "wallet",             "send",                             &send,                          {"outputs","conf_target","estimate_mode","fee_rate","options"} },
    { "wallet",             "sendall",                          &sendall,                       {"recipients","conf_target","estimate_mode","fee_rate","options"} },
    { "wallet",             "sendmany",                         &sendmany,                      {"dummy","amounts","minconf","comment","subtractfeefrom","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "sendtoaddress",                    &sendtoaddress,                 {"address","amount","comment","comment_to","subtractfeefromamount","replaceable","conf_target","estimate_mode","avoid_reuse"} },
    { "wallet",             "sethdseed",                        &sethdseed,                     {"newkeypool","seed"} },
    { "wallet",             "setlabel",                         &setlabel,                      {"address","label"} },
    { "wallet",             "settxfee",                         &settxfee,                      {"amount"} },
    { "wallet",             "setwalletflag",                    &setwalletflag,                 {"flag","value"} },
    { "wallet",             "signmessage",                      &signmessage,                   {"address","message"} },
    { "wallet",             "simulaterawtransaction",           &simulaterawtransaction,        {"rawtxs","options"} },
    { "wallet",             "signrawtransactionwithwallet",     &signrawtransactionwithwallet,  {"hexstring","prevtxs","sighashtype"} },
    { "wallet",             "migratewallet",                    &migratewallet,                 {"wallet_name", "target_name", "backup"} },
    { "wallet",             "unloadwallet",                     &unloadwallet,                  {"wallet_name"} },
    { "wallet",             "walletcreatefundedpsbt",           &walletcreatefundedpsbt,        {"inputs","outputs","locktime","options","bip32derivs"} },
    { "wallet",             "walletdisplayaddress",             &walletdisplayaddress,          {"address"} },
    { "wallet",             "walletlock",                       &walletlock,                    {} },
    { "wallet",             "walletpassphrase",                 &walletpassphrase,              {"passphrase","timeout"} },
    { "wallet",             "walletpassphrasechange",           &walletpassphrasechange,        {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletprocesspsbt",                &walletprocesspsbt,             {"psbt","sign","sighashtype","bip32derivs"} },
};
// clang-format on

void RegisterWalletRPCCommands(interfaces::Chain& chain, std::vector<std::unique_ptr<interfaces::Handler>>& handlers)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        handlers.emplace_back(chain.handleRpc(commands[vcidx]));
}
