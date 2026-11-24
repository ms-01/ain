// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <addrdb.h>
#include <banman.h>
#include <chainparams.h>
#include <fs.h>
#include <net.h>
#include <netbase.h>
#include <protocol.h>
#include <test/setup_common.h>
#include <util/system.h>
#include <util/time.h>

#include <fstream>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(anchor_peers_tests, BasicTestingSetup)

namespace {

CAddress MakeAnchor(const std::string& address)
{
    CService service;
    BOOST_REQUIRE(Lookup(address.c_str(), service, Params().GetDefaultPort(), false));
    const CAddress addr(service, NODE_NETWORK);
    BOOST_REQUIRE(addr.IsValid());
    return addr;
}

fs::path AnchorsFile()
{
    const fs::path dir = GetDataDir() / "net";
    fs::create_directories(dir);
    return dir / ANCHORS_DATABASE_FILENAME;
}

void WriteFile(const fs::path& path, const std::string& content)
{
    std::ofstream stream{path, std::ios::out | std::ios::trunc | std::ios::binary};
    BOOST_REQUIRE(stream.good());
    stream << content;
    stream.flush();
    BOOST_REQUIRE(stream.good());
}

} // namespace

/**
 * Peer anchoring is a connectivity/priority concept only and must be disabled
 * by default on every network.
 */
BOOST_AUTO_TEST_CASE(chainparams_peer_anchoring_disabled_by_default)
{
    const std::vector<std::string> networks{
        CBaseChainParams::MAIN,
        CBaseChainParams::TESTNET,
        CBaseChainParams::CHANGI,
        CBaseChainParams::DEVNET,
        CBaseChainParams::REGTEST,
    };
    for (const auto& network : networks) {
        const auto params = CreateChainParams(network);
        BOOST_REQUIRE(params != nullptr);
        BOOST_CHECK(!params->PeerAnchoring());
    }
}

/**
 * The anchors file is written and read back without loss.
 */
BOOST_AUTO_TEST_CASE(anchors_file_roundtrip)
{
    const fs::path path = AnchorsFile();
    const CAddress first = MakeAnchor("127.0.0.1:12345");
    const CAddress second = MakeAnchor("127.0.0.2:12346");

    DumpAnchors(path, {first, second});
    const std::vector<CAddress> anchors = ReadAnchors(path);
    BOOST_REQUIRE_EQUAL(anchors.size(), 2U);
    BOOST_CHECK_EQUAL(anchors[0].ToStringIPPort(), first.ToStringIPPort());
    BOOST_CHECK_EQUAL(anchors[1].ToStringIPPort(), second.ToStringIPPort());
}

/**
 * At most MAX_BLOCK_RELAY_ONLY_ANCHORS anchors are kept.
 */
BOOST_AUTO_TEST_CASE(anchors_file_cap)
{
    const fs::path path = AnchorsFile();
    DumpAnchors(path, {MakeAnchor("127.0.0.1:12345"), MakeAnchor("127.0.0.2:12346"), MakeAnchor("127.0.0.3:12347")});
    const std::vector<CAddress> anchors = ReadAnchors(path);
    BOOST_REQUIRE_EQUAL(anchors.size(), MAX_BLOCK_RELAY_ONLY_ANCHORS);
}

/**
 * A corrupted (garbage) anchors file is ignored: the node keeps running and
 * no anchor is configured.
 */
BOOST_AUTO_TEST_CASE(anchors_file_garbage_tolerated)
{
    const fs::path path = AnchorsFile();
    WriteFile(path, std::string("\x00\x01\x02garbage\xff\nnot-an-address\n", 22));
    const std::vector<CAddress> anchors = ReadAnchors(path);
    BOOST_CHECK(anchors.empty());
}

/**
 * Invalid and duplicate entries are skipped individually, valid entries are
 * kept.
 */
BOOST_AUTO_TEST_CASE(anchors_file_invalid_entries_skipped)
{
    const fs::path path = AnchorsFile();
    const CAddress valid = MakeAnchor("127.0.0.1:12345");
    WriteFile(path, "# a comment\n\nnot-an-address\n" + valid.ToStringIPPort() + "\n" + valid.ToStringIPPort() + "\n");
    const std::vector<CAddress> anchors = ReadAnchors(path);
    BOOST_REQUIRE_EQUAL(anchors.size(), 1U);
    BOOST_CHECK_EQUAL(anchors[0].ToStringIPPort(), valid.ToStringIPPort());
}

/**
 * A missing anchors file yields an empty anchor set.
 */
BOOST_AUTO_TEST_CASE(anchors_file_missing)
{
    const fs::path path = GetDataDir() / "net" / "does-not-exist.dat";
    BOOST_CHECK(ReadAnchors(path).empty());
}

/**
 * The anchor peer set can be managed at runtime and is capped.
 */
BOOST_AUTO_TEST_CASE(anchor_peer_set_management)
{
    CConnman connman{0, 0};
    const CAddress first = MakeAnchor("127.0.0.1:12345");
    const CAddress second = MakeAnchor("127.0.0.2:12346");
    const CAddress third = MakeAnchor("127.0.0.3:12347");

    BOOST_CHECK(connman.GetAnchorPeers().empty());
    BOOST_CHECK(!connman.IsAnchorPeer(first));

    BOOST_CHECK(connman.AddAnchorPeer(first));
    BOOST_CHECK(connman.IsAnchorPeer(first));
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 1U);

    // Duplicates are rejected.
    BOOST_CHECK(!connman.AddAnchorPeer(first));
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 1U);

    // The number of anchor peers is capped.
    BOOST_CHECK(connman.AddAnchorPeer(second));
    BOOST_CHECK(!connman.AddAnchorPeer(third));
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), MAX_BLOCK_RELAY_ONLY_ANCHORS);

    // Removing an anchor peer works, removing an unknown one fails.
    BOOST_CHECK(connman.RemoveAnchorPeer(first));
    BOOST_CHECK(!connman.IsAnchorPeer(first));
    BOOST_CHECK(!connman.RemoveAnchorPeer(first));
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 1U);

    // Setting the anchor set replaces the previous one.
    connman.SetAnchorPeers({first, second});
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 2U);
    BOOST_CHECK(connman.IsAnchorPeer(first));
    BOOST_CHECK(connman.IsAnchorPeer(second));
}

/**
 * A failed connection attempt to an anchor peer does not remove the anchor peer
 * from the configured set, it only delays the next attempt.
 */
BOOST_AUTO_TEST_CASE(anchor_retry_backoff)
{
    CConnman connman{0, 0};
    const CAddress addr = MakeAnchor("127.0.0.1:12345");

    connman.SetAnchorPeers({addr});
    BOOST_CHECK(connman.HasAnchorPeers());
    BOOST_CHECK_EQUAL(connman.GetNextAnchorAttempt(addr), 0);

    // A failed attempt keeps the anchor peer configured and schedules a retry.
    connman.MarkAnchorAttemptFailed(addr);
    const int64_t next_attempt = connman.GetNextAnchorAttempt(addr);
    BOOST_CHECK(next_attempt > GetSystemTimeInSeconds());
    BOOST_CHECK(next_attempt <= GetSystemTimeInSeconds() + ANCHOR_RETRY_INTERVAL_SECONDS + 1);
    BOOST_CHECK(connman.IsAnchorPeer(addr));
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 1U);

    // The backoff is cleared after a successful connection attempt.
    connman.ClearAnchorBackoff(addr);
    BOOST_CHECK_EQUAL(connman.GetNextAnchorAttempt(addr), 0);

    // Removing the anchor peer also removes its backoff.
    connman.MarkAnchorAttemptFailed(addr);
    BOOST_CHECK(connman.RemoveAnchorPeer(addr));
    BOOST_CHECK_EQUAL(connman.GetNextAnchorAttempt(addr), 0);
    BOOST_CHECK(!connman.HasAnchorPeers());
}

/**
 * The anchor selection always terminates: when every configured anchor peer is
 * skipped (e.g. because it is banned), an invalid address is returned instead
 * of selecting the same anchor peer over and over again.
 */
BOOST_AUTO_TEST_CASE(anchor_selection_terminates)
{
    BanMan banman{GetDataDir() / "banlist.dat", nullptr, 60 * 60};
    CConnman connman{0, 0};
    CConnman::Options options;
    options.m_banman = &banman;
    connman.Init(options);

    const CAddress first = MakeAnchor("127.0.0.1:12345");
    const CAddress second = MakeAnchor("127.0.0.2:12346");
    connman.SetAnchorPeers({first, second});

    // The anchor peers are selected in a round-robin fashion.
    const CAddress selected_first = connman.SelectNextAnchorPeer();
    BOOST_REQUIRE(selected_first.IsValid());
    BOOST_CHECK_EQUAL(selected_first.ToStringIPPort(), first.ToStringIPPort());
    const CAddress selected_second = connman.SelectNextAnchorPeer();
    BOOST_REQUIRE(selected_second.IsValid());
    BOOST_CHECK_EQUAL(selected_second.ToStringIPPort(), second.ToStringIPPort());

    // Both anchor peers are banned: the selection returns an invalid address
    // and does not loop forever.
    banman.Ban(CNetAddr(first), BanReasonManuallyAdded);
    banman.Ban(CNetAddr(second), BanReasonManuallyAdded);
    BOOST_CHECK(!connman.SelectNextAnchorPeer().IsValid());
    BOOST_CHECK(!connman.SelectNextAnchorPeer().IsValid());

    // The anchor peers stay configured, they are only skipped.
    BOOST_CHECK_EQUAL(connman.GetAnchorPeers().size(), 2U);
}

/**
 * An anchor peer inside its retry backoff is skipped, the other anchor peer is
 * selected.
 */
BOOST_AUTO_TEST_CASE(anchor_selection_skips_backoff)
{
    CConnman connman{0, 0};
    const CAddress first = MakeAnchor("127.0.0.1:12345");
    const CAddress second = MakeAnchor("127.0.0.2:12346");
    connman.SetAnchorPeers({first, second});

    connman.MarkAnchorAttemptFailed(first);
    const CAddress selected = connman.SelectNextAnchorPeer();
    BOOST_REQUIRE(selected.IsValid());
    BOOST_CHECK_EQUAL(selected.ToStringIPPort(), second.ToStringIPPort());

    // Without an eligible anchor peer the selection returns an invalid
    // address.
    connman.MarkAnchorAttemptFailed(second);
    BOOST_CHECK(!connman.SelectNextAnchorPeer().IsValid());
}

BOOST_AUTO_TEST_SUITE_END()
