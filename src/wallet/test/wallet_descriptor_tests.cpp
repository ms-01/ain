// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <config/defi-config.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <script/descriptor.h>
#include <streams.h>
#include <test/setup_common.h>
#include <util/bip32.h>
#include <util/system.h>
#include <wallet/db.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletdescriptors.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(wallet_descriptor_tests, BasicTestingSetup)

namespace {

/** Run the given test case against every available wallet database backend. */
void ForEachBackend(const std::function<void(DatabaseFormat)>& test)
{
    test(DatabaseFormat::BERKELEY);
#ifdef USE_SQLITE
    test(DatabaseFormat::SQLITE);
#endif
}

/** A descriptor record with all fields set to a non default value. */
WalletDescriptor MakeDescriptor()
{
    WalletDescriptor descriptor;
    descriptor.desc = "wpkh(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)#8fhd9pwu";
    descriptor.desc_private = "wpkh(cVt4o7BGAig1UXywgGSmARhxMczP5e7tcU9S5VrPMyU6rf3i9tD)#checksum";
    descriptor.encrypted = true;
    descriptor.active = true;
    descriptor.internal = false;
    descriptor.range_begin = 0;
    descriptor.range_end = 99;
    descriptor.next_index = 7;
    descriptor.timestamp = 1600000000;
    descriptor.label = "test label";
    descriptor.output_type = 2;
    return descriptor;
}

/** Create an extended public key that can be used for a ranged descriptor. */
CExtPubKey MakeExtPubKey()
{
    CKey seed;
    seed.MakeNewKey(true);
    CExtKey extkey;
    extkey.SetSeed(seed.begin(), seed.size());
    return extkey.Neuter();
}

/** Build a ranged descriptor (with checksum) for the given extended public key. */
std::string MakeRangedDescriptor(const CExtPubKey& extkey, const std::string& func = "wpkh")
{
    const std::string body = strprintf("%s(%s/0/*)", func, EncodeExtPubKey(extkey));
    FlatSigningProvider provider;
    std::string error;
    auto parsed = Parse(body, provider, error, /* require_checksum = */ false);
    BOOST_REQUIRE_MESSAGE(parsed != nullptr, error);
    return parsed->ToString();
}

/** Create a wallet on a real wallet database of the given backend. */
std::unique_ptr<CWallet> MakeWalletOnDatabase(const fs::path& dir, DatabaseFormat format)
{
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain();
    auto wallet = std::make_unique<CWallet>(chain.get(), WalletLocation(), WalletDatabase::Create(dir, format));
    bool first_run{false};
    wallet->LoadWallet(first_run);
    return wallet;
}

} // namespace

BOOST_AUTO_TEST_CASE(descriptor_serialization_roundtrip)
{
    const WalletDescriptor descriptor = MakeDescriptor();

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << descriptor;

    WalletDescriptor loaded;
    ss >> loaded;

    BOOST_CHECK_EQUAL(loaded.desc, descriptor.desc);
    BOOST_CHECK_EQUAL(loaded.desc_private, descriptor.desc_private);
    BOOST_CHECK_EQUAL(loaded.encrypted, descriptor.encrypted);
    BOOST_CHECK_EQUAL(loaded.active, descriptor.active);
    BOOST_CHECK_EQUAL(loaded.internal, descriptor.internal);
    BOOST_CHECK_EQUAL(loaded.range_begin, descriptor.range_begin);
    BOOST_CHECK_EQUAL(loaded.range_end, descriptor.range_end);
    BOOST_CHECK_EQUAL(loaded.next_index, descriptor.next_index);
    BOOST_CHECK_EQUAL(loaded.timestamp, descriptor.timestamp);
    BOOST_CHECK_EQUAL(loaded.label, descriptor.label);
    BOOST_CHECK_EQUAL(loaded.output_type, descriptor.output_type);
}

BOOST_AUTO_TEST_CASE(descriptor_identifier_is_stable)
{
    const WalletDescriptor descriptor = MakeDescriptor();
    BOOST_CHECK_EQUAL(DescriptorID(descriptor.desc), DescriptorID(descriptor.desc));
    BOOST_CHECK(DescriptorID(descriptor.desc) != DescriptorID(descriptor.desc + "x"));
}

BOOST_AUTO_TEST_CASE(descriptor_record_roundtrip_on_every_backend)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("descriptor_record_%d", (int)format);
        std::unique_ptr<WalletDatabase> db = WalletDatabase::Create(dir, format);

        const WalletDescriptor descriptor = MakeDescriptor();
        {
            WalletBatch batch(*db, DatabaseBatchMode::CREATE);
            BOOST_CHECK(batch.WriteDescriptor(descriptor));
        }
        {
            WalletBatch batch(*db, DatabaseBatchMode::READ_ONLY);
            WalletDescriptor loaded;
            BOOST_CHECK(batch.ReadDescriptor(DescriptorID(descriptor.desc), loaded));
            BOOST_CHECK_EQUAL(loaded.desc, descriptor.desc);
            BOOST_CHECK_EQUAL(loaded.desc_private, descriptor.desc_private);
            BOOST_CHECK_EQUAL(loaded.encrypted, descriptor.encrypted);
            BOOST_CHECK_EQUAL(loaded.active, descriptor.active);
            BOOST_CHECK_EQUAL(loaded.internal, descriptor.internal);
            BOOST_CHECK_EQUAL(loaded.range_begin, descriptor.range_begin);
            BOOST_CHECK_EQUAL(loaded.range_end, descriptor.range_end);
            BOOST_CHECK_EQUAL(loaded.next_index, descriptor.next_index);
            BOOST_CHECK_EQUAL(loaded.timestamp, descriptor.timestamp);
            BOOST_CHECK_EQUAL(loaded.label, descriptor.label);
            BOOST_CHECK_EQUAL(loaded.output_type, descriptor.output_type);
        }
        {
            WalletBatch batch(*db, DatabaseBatchMode::CREATE);
            BOOST_CHECK(batch.EraseDescriptor(DescriptorID(descriptor.desc)));
        }
        {
            WalletBatch batch(*db, DatabaseBatchMode::READ_ONLY);
            WalletDescriptor loaded;
            BOOST_CHECK(!batch.ReadDescriptor(DescriptorID(descriptor.desc), loaded));
        }
    });
}

BOOST_AUTO_TEST_CASE(descriptor_records_are_loaded_by_the_wallet)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("descriptor_load_%d", (int)format);

        const WalletDescriptor descriptor = MakeDescriptor();
        {
            std::unique_ptr<WalletDatabase> db = WalletDatabase::Create(dir, format);
            WalletBatch batch(*db, DatabaseBatchMode::CREATE);
            BOOST_REQUIRE(batch.WriteDescriptor(descriptor));
        }

        std::unique_ptr<CWallet> wallet = MakeWalletOnDatabase(dir, format);
        BOOST_CHECK(wallet->HasDescriptors());
        const auto descriptors = wallet->GetDescriptors();
        BOOST_REQUIRE_EQUAL(descriptors.size(), 1U);
        const auto& loaded = descriptors.at(descriptor.desc);
        BOOST_CHECK_EQUAL(loaded.desc_private, descriptor.desc_private);
        BOOST_CHECK_EQUAL(loaded.active, descriptor.active);
        BOOST_CHECK_EQUAL(loaded.next_index, descriptor.next_index);
        // The descriptor wallet flag is derived from the descriptor records.
        BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));
    });
}

BOOST_AUTO_TEST_CASE(legacy_wallet_without_descriptors_is_unchanged)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("descriptor_legacy_%d", (int)format);
        std::unique_ptr<CWallet> wallet = MakeWalletOnDatabase(dir, format);

        BOOST_CHECK(!wallet->HasDescriptors());
        BOOST_CHECK(wallet->GetDescriptors().empty());
        // A wallet without descriptor records does not carry the descriptor flag.
        BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));
    });
}

BOOST_AUTO_TEST_CASE(descriptor_requires_a_valid_checksum)
{
    FlatSigningProvider provider;
    std::string error;
    const CExtPubKey extkey = MakeExtPubKey();
    const std::string without_checksum = strprintf("wpkh(%s/0/*)", EncodeExtPubKey(extkey));

    BOOST_CHECK(Parse(without_checksum, provider, error, /* require_checksum = */ false) != nullptr);
    BOOST_CHECK(Parse(without_checksum, provider, error, /* require_checksum = */ true) == nullptr);
    BOOST_CHECK(!error.empty());

    // A wrong checksum is rejected as well.
    const std::string wrong_checksum = without_checksum + "#00000000";
    BOOST_CHECK(Parse(wrong_checksum, provider, error, /* require_checksum = */ true) == nullptr);
}

BOOST_AUTO_TEST_CASE(unsupported_descriptor_function_is_rejected)
{
    FlatSigningProvider provider;
    std::string error;
    const std::string descriptor = "tr(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)";
    BOOST_CHECK(Parse(descriptor, provider, error, /* require_checksum = */ false) == nullptr);
    BOOST_CHECK(error.find("tr") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(output_type_of_descriptor_scripts)
{
    const CExtPubKey extkey = MakeExtPubKey();

    // wpkh() derives a bech32 address.
    FlatSigningProvider provider;
    std::string error;
    auto wpkh = Parse(strprintf("wpkh(%s/0/*)", EncodeExtPubKey(extkey)), provider, error, false);
    BOOST_REQUIRE(wpkh != nullptr);
    FlatSigningProvider expanded;
    std::vector<CScript> scripts;
    BOOST_REQUIRE(wpkh->Expand(0, provider, scripts, expanded));
    BOOST_REQUIRE_EQUAL(scripts.size(), 1U);
    BOOST_CHECK_EQUAL(GetWalletOutputType(scripts[0]), static_cast<int8_t>(OutputType::BECH32));

    // pkh() derives a legacy address.
    provider = FlatSigningProvider();
    auto pkh = Parse(strprintf("pkh(%s/0/*)", EncodeExtPubKey(extkey)), provider, error, false);
    BOOST_REQUIRE(pkh != nullptr);
    expanded = FlatSigningProvider();
    scripts.clear();
    BOOST_REQUIRE(pkh->Expand(0, provider, scripts, expanded));
    BOOST_REQUIRE_EQUAL(scripts.size(), 1U);
    BOOST_CHECK_EQUAL(GetWalletOutputType(scripts[0]), static_cast<int8_t>(OutputType::LEGACY));

    // sh(wpkh()) derives a P2SH address.
    provider = FlatSigningProvider();
    auto sh = Parse(strprintf("sh(wpkh(%s/0/*))", EncodeExtPubKey(extkey)), provider, error, false);
    BOOST_REQUIRE(sh != nullptr);
    expanded = FlatSigningProvider();
    scripts.clear();
    BOOST_REQUIRE(sh->Expand(0, provider, scripts, expanded));
    BOOST_REQUIRE_EQUAL(scripts.size(), 1U);
    BOOST_CHECK_EQUAL(GetWalletOutputType(scripts[0]), static_cast<int8_t>(OutputType::P2SH_SEGWIT));
}

BOOST_AUTO_TEST_CASE(active_descriptor_advances_the_next_index)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("descriptor_active_%d", (int)format);
        std::unique_ptr<CWallet> wallet = MakeWalletOnDatabase(dir, format);

        const std::string desc = MakeRangedDescriptor(MakeExtPubKey());
        WalletDescriptor descriptor;
        descriptor.desc = desc;
        descriptor.active = true;
        descriptor.internal = false;
        descriptor.range_begin = 0;
        descriptor.range_end = 9;
        descriptor.next_index = 0;
        descriptor.timestamp = 1600000000;
        descriptor.output_type = static_cast<int8_t>(OutputType::BECH32);

        {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(wallet->AddDescriptor(descriptor));
        }
        BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));

        // The active descriptor is found for its output type and externality.
        WalletDescriptor active;
        {
            LOCK(wallet->cs_wallet);
            BOOST_CHECK(wallet->GetActiveDescriptor(OutputType::BECH32, /* internal = */ false, active));
            BOOST_CHECK(!wallet->GetActiveDescriptor(OutputType::BECH32, /* internal = */ true, active));
            BOOST_CHECK(!wallet->GetActiveDescriptor(OutputType::LEGACY, /* internal = */ false, active));
        }

        // Deriving a new address advances the next index of the descriptor.
        CTxDestination dest;
        std::string error;
        {
            LOCK(wallet->cs_wallet);
            WalletDescriptor record = active;
            BOOST_REQUIRE_MESSAGE(wallet->DeriveNewDescriptorDestination(record, OutputType::BECH32, dest, error), error);
            BOOST_CHECK_EQUAL(record.next_index, 1);
        }

        // The advanced next index is persisted.
        {
            LOCK(wallet->cs_wallet);
            const auto descriptors = wallet->GetDescriptors();
            BOOST_REQUIRE_EQUAL(descriptors.size(), 1U);
            BOOST_CHECK_EQUAL(descriptors.at(desc).next_index, 1);
        }

        // The derived address is a valid bech32 destination.
        BOOST_CHECK(IsValidDestination(dest));
    });
}

BOOST_AUTO_TEST_CASE(private_descriptor_is_not_stored_in_plaintext)
{
    ForEachBackend([](DatabaseFormat format) {
        const fs::path dir = GetDataDir() / strprintf("descriptor_private_%d", (int)format);

        // A descriptor that carries private key material.
        CKey key;
        key.MakeNewKey(true);
        const std::string wif = EncodeSecret(key);
        const std::string body = strprintf("pkh(%s)", wif);
        FlatSigningProvider provider;
        std::string error;
        auto parsed = Parse(body, provider, error, /* require_checksum = */ false);
        BOOST_REQUIRE_MESSAGE(parsed != nullptr, error);
        std::string private_desc;
        BOOST_REQUIRE(parsed->ToPrivateString(provider, private_desc));
        BOOST_CHECK(private_desc.find(wif) != std::string::npos);

        const std::string public_desc = parsed->ToString();
        BOOST_CHECK(public_desc.find(wif) == std::string::npos);

        const SecureString passphrase("descriptor-test-passphrase");
        {
            std::unique_ptr<CWallet> wallet = MakeWalletOnDatabase(dir, format);

            // An unencrypted wallet stores the private descriptor in the clear,
            // in the same way as it stores private keys in the clear.
            {
                LOCK(wallet->cs_wallet);
                BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
            }
            BOOST_CHECK(wallet->IsCrypted());
            BOOST_CHECK(wallet->IsLocked());
            {
                LOCK(wallet->cs_wallet);
                BOOST_REQUIRE(wallet->Unlock(passphrase));
            }
            BOOST_CHECK(!wallet->IsLocked());

            // The private descriptor is encrypted with the wallet master key.
            std::string encrypted;
            {
                LOCK(wallet->cs_wallet);
                BOOST_REQUIRE(wallet->EncryptDescriptorString(public_desc, private_desc, encrypted));
            }
            BOOST_CHECK(encrypted != private_desc);
            BOOST_CHECK(encrypted.find(wif) == std::string::npos);

            // The encrypted blob decrypts back into the private descriptor.
            {
                LOCK(wallet->cs_wallet);
                WalletDescriptor record;
                record.desc = public_desc;
                record.desc_private = encrypted;
                record.encrypted = true;
                std::string decrypted;
                BOOST_REQUIRE(wallet->GetPrivateDescriptorString(record, decrypted));
                BOOST_CHECK_EQUAL(decrypted, private_desc);
            }

            // The descriptor record of an encrypted wallet holds the encrypted
            // private descriptor only.
            WalletDescriptor descriptor;
            descriptor.desc = public_desc;
            descriptor.desc_private = encrypted;
            descriptor.encrypted = true;
            descriptor.timestamp = 1600000000;
            {
                LOCK(wallet->cs_wallet);
                BOOST_REQUIRE(wallet->AddDescriptor(descriptor));
            }
        }

        // The raw database record does not contain the private key in plaintext.
        {
            std::unique_ptr<WalletDatabase> db = WalletDatabase::Create(dir, format);
            std::unique_ptr<DatabaseBatch> batch = db->MakeBatch(DatabaseBatchMode::READ_ONLY);
            WalletDescriptor loaded;
            BOOST_REQUIRE(batch->Read(std::make_pair(DBKeys::DESC, DescriptorID(public_desc)), loaded));
            BOOST_CHECK(loaded.encrypted);
            BOOST_CHECK(loaded.desc_private.find(wif) == std::string::npos);
            BOOST_CHECK(loaded.desc_private != private_desc);
        }
    });
}

BOOST_AUTO_TEST_SUITE_END()
