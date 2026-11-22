// Copyright (c) 2026 DeFiChain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <dfi/errors.h>
#include <dfi/evm.h>
#include <flushablestorage.h>
#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/**
 * Test view over an injectable storage.
 *
 * CVMDomainGraphView has no constructor that accepts a storage and CStorageView is a *virtual* base,
 * therefore the most derived class has to initialize the virtual base explicitly.
 */
class CTestVMDomainGraphView : public CVMDomainGraphView {
public:
    explicit CTestVMDomainGraphView(CStorageKV *storage) : CStorageView(storage) {}
};

/**
 * Storage double which always fails to write.
 *
 * CFlushableStorageKV::Write() always returns true, hence the write error path of the domain graph
 * view can only be reached with such a double.
 */
class CFailingStorageKV : public CStorageKV {
public:
    bool Exists(const TBytes &) const override {
        return false;
    }
    bool Write(const TBytes &, const TBytes &) override {
        return false;
    }
    bool Erase(const TBytes &) override {
        return false;
    }
    bool Read(const TBytes &, TBytes &) const override {
        return false;
    }
    std::unique_ptr<CStorageKVIterator> NewIterator() override {
        return std::make_unique<CStorageKVEmptyIterator>();
    }
    size_t SizeEstimate() const override {
        return 0;
    }
    bool Flush() override {
        return true;
    }
};

/// Appends a COMPACTSIZE encoded length, mirroring WriteCompactSize() of serialize.h.
void AppendCompactSize(TBytes &out, size_t size) {
    if (size < 253) {
        out.push_back(static_cast<uint8_t>(size));
    } else if (size <= std::numeric_limits<unsigned short>::max()) {
        out.push_back(253);
        out.push_back(static_cast<uint8_t>(size & 0xFF));
        out.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    } else {
        out.push_back(254);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((size >> (8 * i)) & 0xFF));
        }
    }
}

/// Raw DB key of a domain edge entry: prefix | direction | COMPACTSIZE(len) | key bytes.
TBytes ExpectedRawKey(uint8_t prefix, VMDomainEdge direction, const std::string &key) {
    TBytes raw;
    raw.push_back(prefix);
    raw.push_back(static_cast<uint8_t>(direction));
    AppendCompactSize(raw, key.size());
    raw.insert(raw.end(), key.begin(), key.end());
    return raw;
}

/// Raw DB value of a domain edge entry: COMPACTSIZE(len) | value bytes.
TBytes ExpectedRawValue(const std::string &value) {
    TBytes raw;
    AppendCompactSize(raw, value.size());
    raw.insert(raw.end(), value.begin(), value.end());
    return raw;
}

using EdgeKey = std::pair<VMDomainEdge, std::string>;
using EdgeList = std::vector<std::pair<EdgeKey, std::string>>;

} // namespace

BOOST_FIXTURE_TEST_SUITE(evm_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(evm_domain_edge_prefixes_are_pinned)
{
    // A rename of the prefixes would silently break the DVM <-> EVM domain mapping, therefore the
    // prefixes are pinned here.
    BOOST_CHECK_EQUAL(static_cast<int>(CVMDomainGraphView::VMDomainBlockEdge::prefix()), 0x4E);
    BOOST_CHECK_EQUAL(static_cast<int>(CVMDomainGraphView::VMDomainTxEdge::prefix()), 0x65);
    BOOST_CHECK_EQUAL(static_cast<int>(CVMDomainGraphView::VMDomainBlockEdge::prefix()), static_cast<int>('N'));
    BOOST_CHECK_EQUAL(static_cast<int>(CVMDomainGraphView::VMDomainTxEdge::prefix()), static_cast<int>('e'));
    BOOST_CHECK(CVMDomainGraphView::VMDomainBlockEdge::prefix() != CVMDomainGraphView::VMDomainTxEdge::prefix());
}

BOOST_AUTO_TEST_CASE(evm_block_edge_set_get_round_trip)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_block"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    for (const auto &direction : {VMDomainEdge::DVMToEVM, VMDomainEdge::EVMToDVM}) {
        const std::string key = "blockhashkey";
        const std::string value = "blockhashvalue";

        const auto res = view.SetVMDomainBlockEdge(direction, key, value);
        BOOST_CHECK(res.ok);

        const auto got = view.GetVMDomainBlockEdge(direction, key);
        BOOST_CHECK(got.ok);
        BOOST_CHECK_EQUAL(*got, value);

        // The entry lives under the block edge prefix and holds the length prefixed value.
        const auto expectedKey = ExpectedRawKey(CVMDomainGraphView::VMDomainBlockEdge::prefix(), direction, key);
        BOOST_CHECK_EQUAL(raw->GetRaw().count(expectedKey), 1U);
        BOOST_CHECK(raw->GetRaw().at(expectedKey).has_value());
        BOOST_CHECK(raw->GetRaw().at(expectedKey).value() == ExpectedRawValue(value));
    }
}

BOOST_AUTO_TEST_CASE(evm_tx_edge_set_get_round_trip)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_tx"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    for (const auto &direction : {VMDomainEdge::DVMToEVM, VMDomainEdge::EVMToDVM}) {
        const std::string key = "txhashkey";
        const std::string value = "txhashvalue";

        const auto res = view.SetVMDomainTxEdge(direction, key, value);
        BOOST_CHECK(res.ok);

        const auto got = view.GetVMDomainTxEdge(direction, key);
        BOOST_CHECK(got.ok);
        BOOST_CHECK_EQUAL(*got, value);

        const auto expectedKey = ExpectedRawKey(CVMDomainGraphView::VMDomainTxEdge::prefix(), direction, key);
        BOOST_CHECK_EQUAL(raw->GetRaw().count(expectedKey), 1U);
        BOOST_CHECK(raw->GetRaw().at(expectedKey).has_value());
        BOOST_CHECK(raw->GetRaw().at(expectedKey).value() == ExpectedRawValue(value));
    }
}

BOOST_AUTO_TEST_CASE(evm_block_and_tx_namespaces_do_not_collide)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_ns"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    const std::string key = "sharedkey";
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key, "blockvalue").ok);
    BOOST_CHECK(view.SetVMDomainTxEdge(VMDomainEdge::DVMToEVM, key, "txvalue").ok);

    const auto blockGot = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key);
    const auto txGot = view.GetVMDomainTxEdge(VMDomainEdge::DVMToEVM, key);
    BOOST_CHECK(blockGot.ok);
    BOOST_CHECK(txGot.ok);
    BOOST_CHECK_EQUAL(*blockGot, "blockvalue");
    BOOST_CHECK_EQUAL(*txGot, "txvalue");

    BOOST_CHECK_EQUAL(raw->GetRaw().size(), 2U);
    const auto blockKey = ExpectedRawKey(CVMDomainGraphView::VMDomainBlockEdge::prefix(), VMDomainEdge::DVMToEVM, key);
    const auto txKey = ExpectedRawKey(CVMDomainGraphView::VMDomainTxEdge::prefix(), VMDomainEdge::DVMToEVM, key);
    BOOST_CHECK_EQUAL(raw->GetRaw().count(blockKey), 1U);
    BOOST_CHECK_EQUAL(raw->GetRaw().count(txKey), 1U);
    BOOST_CHECK(blockKey != txKey);
    BOOST_CHECK_EQUAL(static_cast<int>(blockKey.front()), static_cast<int>('N'));
    BOOST_CHECK_EQUAL(static_cast<int>(txKey.front()), static_cast<int>('e'));
}

BOOST_AUTO_TEST_CASE(evm_get_missing_key_returns_error)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_missing"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    const std::string key = "missingkey";
    const auto blockGot = view.GetVMDomainBlockEdge(VMDomainEdge::EVMToDVM, key);
    BOOST_CHECK(!blockGot.ok);
    BOOST_CHECK_EQUAL(blockGot.msg, DeFiErrors::DatabaseKeyNotFound(key).msg);

    const auto txGot = view.GetVMDomainTxEdge(VMDomainEdge::EVMToDVM, key);
    BOOST_CHECK(!txGot.ok);
    BOOST_CHECK_EQUAL(txGot.msg, DeFiErrors::DatabaseKeyNotFound(key).msg);
}

BOOST_AUTO_TEST_CASE(evm_set_overwrites_existing_entry)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_overwrite"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    const std::string key = "samekey";
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key, "first").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key, "second").ok);

    BOOST_CHECK_EQUAL(raw->GetRaw().size(), 1U);
    const auto got = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key);
    BOOST_CHECK(got.ok);
    BOOST_CHECK_EQUAL(*got, "second");
}

BOOST_AUTO_TEST_CASE(evm_for_each_block_edges_returns_all_entries)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_foreach"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a1", "v1").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::EVMToDVM, "b1", "v2").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a2", "v3").ok);

    EdgeList seen;
    view.ForEachVMDomainBlockEdges([&seen](const EdgeKey &key, const std::string &value) {
        seen.emplace_back(key, value);
        return true;
    });

    const EdgeList expected{
        {EdgeKey{VMDomainEdge::DVMToEVM, "a1"}, "v1"},
        {EdgeKey{VMDomainEdge::DVMToEVM, "a2"}, "v3"},
        {EdgeKey{VMDomainEdge::EVMToDVM, "b1"}, "v2"},
    };
    BOOST_REQUIRE_EQUAL(seen.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        BOOST_CHECK(seen[i].first.first == expected[i].first.first);
        BOOST_CHECK_EQUAL(seen[i].first.second, expected[i].first.second);
        BOOST_CHECK_EQUAL(seen[i].second, expected[i].second);
    }
}

BOOST_AUTO_TEST_CASE(evm_for_each_tx_edges_returns_all_entries)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_foreach_tx"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    // Block edges must not show up in the tx iteration.
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a1", "blockvalue").ok);
    BOOST_CHECK(view.SetVMDomainTxEdge(VMDomainEdge::DVMToEVM, "a1", "txvalue1").ok);
    BOOST_CHECK(view.SetVMDomainTxEdge(VMDomainEdge::EVMToDVM, "b1", "txvalue2").ok);

    EdgeList seen;
    view.ForEachVMDomainTxEdges([&seen](const EdgeKey &key, const std::string &value) {
        seen.emplace_back(key, value);
        return true;
    });

    const EdgeList expected{
        {EdgeKey{VMDomainEdge::DVMToEVM, "a1"}, "txvalue1"},
        {EdgeKey{VMDomainEdge::EVMToDVM, "b1"}, "txvalue2"},
    };
    BOOST_REQUIRE_EQUAL(seen.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        BOOST_CHECK(seen[i].first.first == expected[i].first.first);
        BOOST_CHECK_EQUAL(seen[i].first.second, expected[i].first.second);
        BOOST_CHECK_EQUAL(seen[i].second, expected[i].second);
    }
}

BOOST_AUTO_TEST_CASE(evm_for_each_honours_start_key)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_start"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a1", "v1").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a2", "v2").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::EVMToDVM, "b1", "v3").ok);

    // The start key selects the direction as well.
    EdgeList seen;
    view.ForEachVMDomainBlockEdges(
        [&seen](const EdgeKey &key, const std::string &value) {
            seen.emplace_back(key, value);
            return true;
        },
        EdgeKey{VMDomainEdge::EVMToDVM, ""});
    BOOST_REQUIRE_EQUAL(seen.size(), 1U);
    BOOST_CHECK(seen[0].first.first == VMDomainEdge::EVMToDVM);
    BOOST_CHECK_EQUAL(seen[0].first.second, "b1");

    // The start key also acts as a lower bound inside the direction.
    seen.clear();
    view.ForEachVMDomainBlockEdges(
        [&seen](const EdgeKey &key, const std::string &value) {
            seen.emplace_back(key, value);
            return true;
        },
        EdgeKey{VMDomainEdge::DVMToEVM, "a2"});
    BOOST_REQUIRE_EQUAL(seen.size(), 2U);
    BOOST_CHECK_EQUAL(seen[0].first.second, "a2");
    BOOST_CHECK_EQUAL(seen[1].first.second, "b1");
}

BOOST_AUTO_TEST_CASE(evm_for_each_can_stop_early)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_stop"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a1", "v1").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "a2", "v2").ok);

    size_t calls = 0;
    view.ForEachVMDomainBlockEdges([&calls](const EdgeKey &, const std::string &) {
        ++calls;
        return false;
    });
    BOOST_CHECK_EQUAL(calls, 1U);
}

BOOST_AUTO_TEST_CASE(evm_for_each_on_empty_storage_yields_nothing)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_empty"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    size_t calls = 0;
    view.ForEachVMDomainBlockEdges([&calls](const EdgeKey &, const std::string &) {
        ++calls;
        return true;
    });
    view.ForEachVMDomainTxEdges([&calls](const EdgeKey &, const std::string &) {
        ++calls;
        return true;
    });
    BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_CASE(evm_edge_key_ordering_is_length_prefixed)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_order"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    // The key is length prefixed, therefore the shorter key sorts first.
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "z", "short").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "aa", "long").ok);

    EdgeList seen;
    view.ForEachVMDomainBlockEdges([&seen](const EdgeKey &key, const std::string &value) {
        seen.emplace_back(key, value);
        return true;
    });

    BOOST_REQUIRE_EQUAL(seen.size(), 2U);
    BOOST_CHECK_EQUAL(seen[0].first.second, "z");
    BOOST_CHECK_EQUAL(seen[0].second, "short");
    BOOST_CHECK_EQUAL(seen[1].first.second, "aa");
    BOOST_CHECK_EQUAL(seen[1].second, "long");
}

BOOST_AUTO_TEST_CASE(evm_edge_value_with_embedded_null_bytes_round_trips)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_nul"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CTestVMDomainGraphView view(flushable.release());

    std::string value("a\0b\0c", 5);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "key", value).ok);

    const auto got = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "key");
    BOOST_CHECK(got.ok);
    BOOST_REQUIRE_EQUAL((*got).size(), value.size());
    BOOST_CHECK(*got == value);
}

BOOST_AUTO_TEST_CASE(evm_set_with_empty_key_is_isolated)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_emptykey"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "", "emptyvalue").ok);
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "key", "othervalue").ok);

    BOOST_CHECK_EQUAL(raw->GetRaw().size(), 2U);
    const auto emptyGot = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "");
    BOOST_CHECK(emptyGot.ok);
    BOOST_CHECK_EQUAL(*emptyGot, "emptyvalue");

    const auto otherGot = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, "key");
    BOOST_CHECK(otherGot.ok);
    BOOST_CHECK_EQUAL(*otherGot, "othervalue");
}

BOOST_AUTO_TEST_CASE(evm_set_with_large_key_round_trips)
{
    std::unique_ptr<CStorageLevelDB> leveldb = std::make_unique<CStorageLevelDB>(fs::path("evm_tests_large"), 1 << 20, true);
    auto flushable = std::make_unique<CFlushableStorageKV>(*leveldb);
    CFlushableStorageKV *raw = flushable.get();
    CTestVMDomainGraphView view(flushable.release());

    const std::string key(64 * 1024, 'k');
    const std::string value(64 * 1024, 'v');
    BOOST_CHECK(view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key, value).ok);

    const auto expectedKey = ExpectedRawKey(CVMDomainGraphView::VMDomainBlockEdge::prefix(), VMDomainEdge::DVMToEVM, key);
    BOOST_CHECK_EQUAL(raw->GetRaw().count(expectedKey), 1U);

    const auto got = view.GetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key);
    BOOST_CHECK(got.ok);
    BOOST_CHECK_EQUAL((*got).size(), value.size());
    BOOST_CHECK(*got == value);

    EdgeList seen;
    view.ForEachVMDomainBlockEdges([&seen](const EdgeKey &k, const std::string &v) {
        seen.emplace_back(k, v);
        return true;
    });
    BOOST_REQUIRE_EQUAL(seen.size(), 1U);
    BOOST_CHECK_EQUAL(seen[0].first.second.size(), key.size());
    BOOST_CHECK(seen[0].second == value);
}

BOOST_AUTO_TEST_CASE(evm_set_failure_returns_rw_error)
{
    auto failing = std::make_unique<CFailingStorageKV>();
    CTestVMDomainGraphView view(failing.release());

    const std::string key = "failkey";
    const auto res = view.SetVMDomainBlockEdge(VMDomainEdge::DVMToEVM, key, "value");
    BOOST_CHECK(!res.ok);
    BOOST_CHECK_EQUAL(res.msg, DeFiErrors::DatabaseRWFailure(key).msg);

    const auto txRes = view.SetVMDomainTxEdge(VMDomainEdge::DVMToEVM, key, "value");
    BOOST_CHECK(!txRes.ok);
    BOOST_CHECK_EQUAL(txRes.msg, DeFiErrors::DatabaseRWFailure(key).msg);
}

BOOST_AUTO_TEST_CASE(evm_scoped_template_create_fails_for_invalid_miner_address)
{
    // IMPORTANT: only an unparsable miner address may be used here. The Rust side rejects it before
    // touching the EVM services, so no node context is required. A valid (or empty) address would
    // reach getAttributeValues() with a null CCustomCSView and crash the test binary.
    const auto scoped = CScopedTemplate::Create(0, "not-an-address", 0, 0, 0);
    BOOST_CHECK(scoped == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
