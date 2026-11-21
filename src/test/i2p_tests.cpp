// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/sha256.h>
#include <i2p.h>
#include <logging.h>
#include <netbase.h>
#include <test/setup_common.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(i2p_tests, BasicTestingSetup)

namespace {

/**
 * The behaviour of the mock SAM bridge.
 */
enum class BridgeMode {
    NORMAL,
    NO_VALUE,        //!< The "NAMING LOOKUP" reply has no VALUE=.
    CANT_REACH_PEER, //!< The "STREAM CONNECT" reply reports an unreachable peer.
    TIMEOUT,         //!< The "STREAM CONNECT" reply reports a timeout.
    INVALID_ID,      //!< The "STREAM CONNECT" reply reports an invalid session id.
    HUGE_REPLY,      //!< The bridge sends more than MAX_MSG_SIZE without a terminator.
    MALFORMED_PRIV,  //!< The "DEST GENERATE" reply holds a malformed private key.
};

/**
 * The malformed private key that the mock bridge sends in the
 * BridgeMode::MALFORMED_PRIV mode. It is not valid Base64 (it holds a "!")
 * and it must never appear in the log output of the node.
 */
static const std::string MALFORMED_PRIVATE_KEY{"sECReTKeYmAtErIaL!"};

/**
 * Encode the given bytes the way the I2P SAM bridge does (I2P-style Base64).
 */
std::string ToB64(const std::vector<uint8_t>& data)
{
    return i2p::SwapBase64(EncodeBase64(std::string(data.begin(), data.end())));
}

/**
 * A structurally valid I2P private key (a destination without a certificate).
 */
std::vector<uint8_t> MakePrivateKey()
{
    std::vector<uint8_t> key(387);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    // The certificate length (bytes 385-386) is zero.
    key[385] = 0;
    key[386] = 0;
    return key;
}

/**
 * The destination of the peer that the mock bridge "connects" to.
 */
std::vector<uint8_t> MakePeerDestination()
{
    std::vector<uint8_t> dest(32);
    for (size_t i = 0; i < dest.size(); ++i) {
        dest[i] = static_cast<uint8_t>(255 - i);
    }
    return dest;
}

/**
 * A mock SAM v3.1 bridge.
 *
 * The bridge runs inside the test process (no background daemon) and speaks
 * the subset of the SAM protocol that the node uses.
 */
class MockSamBridge
{
public:
    explicit MockSamBridge(BridgeMode mode = BridgeMode::NORMAL)
        : m_mode{mode},
          m_private_key{MakePrivateKey()},
          m_peer_destination{MakePeerDestination()}
    {
        m_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_REQUIRE(m_listen_sock != INVALID_SOCKET);
        int reuse{1};
        setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        BOOST_REQUIRE(SetSocketNonBlocking(m_listen_sock, true));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        BOOST_REQUIRE(bind(m_listen_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        BOOST_REQUIRE(listen(m_listen_sock, 8) == 0);

        socklen_t len{sizeof(addr)};
        BOOST_REQUIRE(getsockname(m_listen_sock, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread(&MockSamBridge::AcceptLoop, this);
    }

    ~MockSamBridge()
    {
        m_stop = true;
        if (m_listen_sock != INVALID_SOCKET) {
            CloseSocket(m_listen_sock);
            m_listen_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
        for (std::thread& t : m_connection_threads) {
            if (t.joinable()) t.join();
        }
    }

    CService GetAddress() const
    {
        CNetAddr addr;
        struct in_addr in;
        in.s_addr = htonl(INADDR_LOOPBACK);
        addr.SetIP(CNetAddr(in));
        return CService(addr, m_port);
    }

    int GetDestGenerateCount() const { return m_dest_generate_count; }
    int GetSessionCreateCount() const { return m_session_create_count; }
    std::string GetSessionDestination() const { return m_session_destination; }

private:
    static bool WaitFor(SOCKET sock, bool& readable)
    {
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        readable = false;
        const int ret = select(sock + 1, &fdset, nullptr, nullptr, &tv);
        if (ret > 0) readable = true;
        return ret >= 0;
    }

    bool ReadLine(SOCKET sock, std::string& out)
    {
        out.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            bool readable{false};
            if (!WaitFor(sock, readable)) return false;
            if (!readable) continue;
            char buf[512];
            const ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') return true;
                out.push_back(buf[i]);
            }
        }
        return false;
    }

    bool SendAll(SOCKET sock, const std::string& data)
    {
        size_t sent{0};
        while (sent < data.size()) {
            const ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void SendLine(SOCKET sock, const std::string& line)
    {
        SendAll(sock, line + "\n");
    }

    void SendHugeReply(SOCKET sock)
    {
        // More than i2p::sam::MAX_MSG_SIZE bytes without a terminator.
        const std::string huge(i2p::sam::MAX_MSG_SIZE + 1024, 'x');
        SendAll(sock, huge);
    }

    void EchoLoop(SOCKET sock)
    {
        while (true) {
            bool readable{false};
            if (!WaitFor(sock, readable)) return;
            if (!readable) continue;
            char buf[512];
            const ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) return;
            if (!SendAll(sock, std::string(buf, buf + n))) return;
        }
    }

    void HandleConnection(SOCKET sock)
    {
        std::string line;
        while (ReadLine(sock, line)) {
            if (line.rfind("HELLO", 0) == 0) {
                if (m_mode == BridgeMode::HUGE_REPLY) {
                    SendHugeReply(sock);
                    return;
                }
                SendLine(sock, "RESULT=OK VERSION=3.1");
            } else if (line.rfind("DEST GENERATE", 0) == 0) {
                ++m_dest_generate_count;
                if (m_mode == BridgeMode::MALFORMED_PRIV) {
                    // A private key that cannot be decoded.
                    SendLine(sock, "RESULT=OK PRIVATEKEY=" + MALFORMED_PRIVATE_KEY);
                } else {
                    SendLine(sock, "RESULT=OK DESTINATION=" + ToB64(m_private_key) + " PRIVATEKEY=" + ToB64(m_private_key));
                }
            } else if (line.rfind("SESSION CREATE", 0) == 0) {
                ++m_session_create_count;
                const std::string::size_type pos = line.find("DESTINATION=");
                if (pos != std::string::npos) {
                    const std::string key("DESTINATION=");
                    const std::string::size_type end = line.find(' ', pos);
                    const std::string::size_type len = end == std::string::npos ? std::string::npos : end - pos - key.size();
                    m_session_destination = line.substr(pos + key.size(), len);
                }
                SendLine(sock, "RESULT=OK");
            } else if (line.rfind("NAMING LOOKUP", 0) == 0) {
                if (m_mode == BridgeMode::NO_VALUE) {
                    SendLine(sock, "RESULT=OK");
                } else {
                    SendLine(sock, "RESULT=OK VALUE=" + ToB64(m_peer_destination));
                }
            } else if (line.rfind("STREAM CONNECT", 0) == 0) {
                switch (m_mode) {
                case BridgeMode::CANT_REACH_PEER:
                    SendLine(sock, "RESULT=CANT_REACH_PEER");
                    break;
                case BridgeMode::TIMEOUT:
                    SendLine(sock, "RESULT=TIMEOUT");
                    break;
                case BridgeMode::INVALID_ID:
                    SendLine(sock, "RESULT=INVALID_ID");
                    break;
                default:
                    SendLine(sock, "RESULT=OK");
                    // The socket is now a data stream.
                    EchoLoop(sock);
                    return;
                }
            } else if (line.rfind("STREAM ACCEPT", 0) == 0) {
                SendLine(sock, "RESULT=OK");
                // The bridge pushes the destination of the connecting peer.
                SendLine(sock, ToB64(m_peer_destination));
            } else {
                SendLine(sock, "RESULT=ERROR UNKNOWN_COMMAND");
            }
        }
    }

    void AcceptLoop()
    {
        while (!m_stop) {
            bool readable{false};
            if (!WaitFor(m_listen_sock, readable)) return;
            if (!readable) continue;
            const SOCKET sock = accept(m_listen_sock, nullptr, nullptr);
            if (sock == INVALID_SOCKET) continue;
            m_connection_threads.emplace_back(&MockSamBridge::HandleConnection, this, sock);
        }
    }

    BridgeMode m_mode;
    std::vector<uint8_t> m_private_key;
    std::vector<uint8_t> m_peer_destination;
    SOCKET m_listen_sock{INVALID_SOCKET};
    uint16_t m_port{0};
    std::thread m_thread;
    std::vector<std::thread> m_connection_threads;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_dest_generate_count{0};
    std::atomic<int> m_session_create_count{0};
    std::string m_session_destination;
};

/**
 * The path of the private key file of the tests.
 */
fs::path PrivateKeyPath()
{
    return GetDataDir() / "i2p_private_key";
}

/**
 * Send the given data over the socket.
 */
bool SendRaw(SOCKET sock, const std::string& data)
{
    size_t sent{0};
    while (sent < data.size()) {
        const ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

/**
 * Receive up to `len` bytes from the socket (waiting for at most 10 seconds).
 */
ssize_t RecvRaw(SOCKET sock, char* buf, size_t len)
{
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (select(sock + 1, &fdset, nullptr, nullptr, &tv) <= 0) return -1;
    return recv(sock, buf, len, 0);
}

} // namespace

BOOST_AUTO_TEST_CASE(base64_swap)
{
    BOOST_CHECK_EQUAL(i2p::SwapBase64("-~+/"), "+/-~");
    BOOST_CHECK_EQUAL(i2p::SwapBase64(i2p::SwapBase64("-~+/")), "-~+/");
    // The characters that are not part of the alphabet are not touched.
    BOOST_CHECK_EQUAL(i2p::SwapBase64("abc123"), "abc123");
}

BOOST_AUTO_TEST_CASE(base64_decode)
{
    const std::vector<uint8_t> data{0x00, 0x01, 0x02, 0xfd, 0xfe, 0xff};
    const std::string b64 = ToB64(data);
    BOOST_CHECK(i2p::DecodeI2PBase64(b64) == data);

    // An invalid Base64 string is rejected.
    BOOST_CHECK_THROW(i2p::DecodeI2PBase64("!!!!"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(base64_decode_error_does_not_leak_the_value)
{
    // The error message of the decoder must not contain the value itself: it
    // can be sensitive (the private key of the node) and the error message
    // ends up in the debug log.
    const std::string secret{"sECReTKeYmAtErIaL!"};
    try {
        i2p::DecodeI2PBase64(secret);
        BOOST_CHECK(false);
    } catch (const std::runtime_error& e) {
        const std::string msg{e.what()};
        BOOST_CHECK(msg.find(secret) == std::string::npos);
        // The message is still helpful enough to identify the problem.
        BOOST_CHECK(msg.find("Cannot decode") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(dest_bin_to_addr)
{
    const std::vector<uint8_t> dest = MakePeerDestination();
    const CNetAddr addr = i2p::DestBinToAddr(dest);

    BOOST_CHECK(addr.IsI2P());
    BOOST_CHECK(addr.IsValid());

    // The address is the Base32 encoding of the SHA256 of the destination.
    unsigned char hash[32];
    CSHA256 hasher;
    hasher.Write(dest.data(), dest.size());
    hasher.Finalize(hash);
    BOOST_CHECK_EQUAL(addr.ToStringIP(), EncodeBase32(hash, sizeof(hash), false) + ".b32.i2p");

    // The address round-trips through the text representation.
    CNetAddr parsed;
    BOOST_CHECK(parsed.SetSpecial(addr.ToStringIP()));
    BOOST_CHECK(parsed == addr);
}

BOOST_AUTO_TEST_CASE(dest_b64_to_addr)
{
    const std::vector<uint8_t> dest = MakePeerDestination();
    BOOST_CHECK(i2p::DestB64ToAddr(ToB64(dest)) == i2p::DestBinToAddr(dest));
    BOOST_CHECK_THROW(i2p::DestB64ToAddr("!!!"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(destination_parsing)
{
    CNetAddr addr;
    // A valid destination.
    BOOST_CHECK(addr.SetSpecial(i2p::DestBinToAddr(MakePeerDestination()).ToStringIP()));
    // A destination of the wrong length.
    BOOST_CHECK(!addr.SetSpecial("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b32.i2p"));
    // An invalid Base32 character.
    BOOST_CHECK(!addr.SetSpecial("11111111111111111111111111111111111111111111111a.b32.i2p"));
}

BOOST_AUTO_TEST_CASE(port_normalization)
{
    const CNetAddr addr = i2p::DestBinToAddr(MakePeerDestination());
    const CService with_port(addr, 1234);
    const CService without_port(addr, 0);

    // I2P destinations are portless: the port is always normalized to 0.
    BOOST_CHECK_EQUAL(i2p::NormalizeI2PService(with_port).GetPort(), 0);
    BOOST_CHECK(i2p::NormalizeI2PService(with_port) == i2p::NormalizeI2PService(without_port));
    BOOST_CHECK(i2p::NormalizeI2PService(with_port) == without_port);

    // A non-I2P service is not touched.
    const CService ipv4(CNetAddr(in_addr{htonl(0x7f000001)}), 1234);
    BOOST_CHECK_EQUAL(i2p::NormalizeI2PService(ipv4).GetPort(), 1234);
}

BOOST_AUTO_TEST_CASE(lookup_i2p_destination)
{
    const std::string name = i2p::DestBinToAddr(MakePeerDestination()).ToStringIP();
    CService service;
    BOOST_REQUIRE(Lookup(name.c_str(), service, 8556, false));
    BOOST_CHECK(service.IsI2P());
    // The lookup hands out the default port of the chain, the transport
    // normalizes it to 0.
    BOOST_CHECK_EQUAL(service.GetPort(), 8556);
    BOOST_CHECK_EQUAL(i2p::NormalizeI2PService(service).GetPort(), 0);
}

BOOST_AUTO_TEST_CASE(session_connect)
{
    MockSamBridge bridge;
    const fs::path key_path = PrivateKeyPath();
    std::error_code ec;
    fs::remove(key_path, ec);

    {
        i2p::sam::Session session(key_path, bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_REQUIRE(session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
        // The proxy error is only meaningful if the connection failed.
        BOOST_CHECK(conn.sock != INVALID_SOCKET);
        // The peer is normalized to port 0.
        BOOST_CHECK_EQUAL(conn.peer.GetPort(), 0);
        BOOST_CHECK(conn.peer.IsI2P());
        // Our own address is an I2P destination as well.
        BOOST_CHECK(conn.me.IsI2P());
        BOOST_CHECK_EQUAL(conn.me.GetPort(), 0);

        // The data of the tunnel is carried by the socket of the bridge.
        BOOST_REQUIRE(SendRaw(conn.sock, "ping"));
        char buf[16]{};
        const ssize_t n = RecvRaw(conn.sock, buf, sizeof(buf));
        BOOST_REQUIRE(n > 0);
        BOOST_CHECK_EQUAL(std::string(buf, buf + n), "ping");

        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
    }

    // The private key was written to the disk.
    BOOST_CHECK(fs::exists(key_path));
    const fs::perms perms = fs::status(key_path).permissions();
    BOOST_CHECK(perms == (fs::perms::owner_read | fs::perms::owner_write));
    BOOST_CHECK_EQUAL(bridge.GetDestGenerateCount(), 1);
    BOOST_CHECK_EQUAL(bridge.GetSessionCreateCount(), 1);
}

BOOST_AUTO_TEST_CASE(session_reuses_the_private_key)
{
    MockSamBridge bridge;
    const fs::path key_path = PrivateKeyPath();
    std::error_code ec;
    fs::remove(key_path, ec);

    {
        i2p::sam::Session session(key_path, bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_REQUIRE(session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
    }
    const std::string first_destination = bridge.GetSessionDestination();
    BOOST_CHECK(!first_destination.empty());

    {
        // The second session re-uses the private key of the first one.
        i2p::sam::Session session(key_path, bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_REQUIRE(session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
    }

    BOOST_CHECK_EQUAL(bridge.GetDestGenerateCount(), 1);
    BOOST_CHECK_EQUAL(bridge.GetSessionCreateCount(), 2);
    BOOST_CHECK_EQUAL(bridge.GetSessionDestination(), first_destination);
}

BOOST_AUTO_TEST_CASE(session_regenerates_a_corrupt_private_key)
{
    MockSamBridge bridge;
    const fs::path key_path = PrivateKeyPath();

    // A corrupt private key file must not wedge the node.
    {
        FILE* file = fsbridge::fopen(key_path, "wb");
        BOOST_REQUIRE(file != nullptr);
        const std::string garbage("this is not a private key");
        fwrite(garbage.data(), 1, garbage.size(), file);
        fclose(file);
    }

    {
        i2p::sam::Session session(key_path, bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_REQUIRE(session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
        CloseSocket(conn.sock);
        conn.sock = INVALID_SOCKET;
    }

    // The key was regenerated.
    BOOST_CHECK_EQUAL(bridge.GetDestGenerateCount(), 1);
    BOOST_CHECK(fs::exists(key_path));
    std::error_code size_ec;
    BOOST_CHECK(fs::file_size(key_path, size_ec) >= 387);
}

BOOST_AUTO_TEST_CASE(session_error_paths)
{
    const CService peer(i2p::DestBinToAddr(MakePeerDestination()), 0);

    // An unreachable peer is not an error of the bridge.
    {
        MockSamBridge bridge(BridgeMode::CANT_REACH_PEER);
        i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_CHECK(!session.Connect(peer, conn, proxy_error));
        BOOST_CHECK(!proxy_error);
    }

    // A timeout of the bridge is an unreachable peer as well.
    {
        MockSamBridge bridge(BridgeMode::TIMEOUT);
        i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_CHECK(!session.Connect(peer, conn, proxy_error));
        BOOST_CHECK(!proxy_error);
    }

    // A missing key in the reply is an error of the bridge.
    {
        MockSamBridge bridge(BridgeMode::NO_VALUE);
        i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_CHECK(!session.Connect(peer, conn, proxy_error));
        BOOST_CHECK(proxy_error);
    }

    // An invalid session id invalidates the session: the next attempt creates
    // a new one.
    {
        MockSamBridge bridge(BridgeMode::INVALID_ID);
        i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_CHECK(!session.Connect(peer, conn, proxy_error));
        BOOST_CHECK(proxy_error);
        BOOST_CHECK_EQUAL(bridge.GetSessionCreateCount(), 1);
        // The session was invalidated, so the next attempt creates a new one.
        BOOST_CHECK(!session.Connect(peer, conn, proxy_error));
        BOOST_CHECK_EQUAL(bridge.GetSessionCreateCount(), 2);
    }
}

BOOST_AUTO_TEST_CASE(session_huge_reply)
{
    // A bridge that streams more than MAX_MSG_SIZE bytes without a terminator
    // must not be able to exhaust the memory of the node.
    MockSamBridge bridge(BridgeMode::HUGE_REPLY);
    i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);
    i2p::Connection conn;
    bool proxy_error{true};
    BOOST_CHECK(!session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
    BOOST_CHECK(proxy_error);
}

BOOST_AUTO_TEST_CASE(session_error_does_not_log_the_private_key)
{
    // A bridge that answers "DEST GENERATE" with a malformed private key must
    // not make the private key (or any part of it) end up in the debug log.
    MockSamBridge bridge(BridgeMode::MALFORMED_PRIV);
    const fs::path key_path = PrivateKeyPath();
    std::error_code ec;
    fs::remove(key_path, ec);

    const fs::path log_path = GetDataDir() / "debug.log";

    LogInstance().EnableCategory(BCLog::I2P);
    {
        i2p::sam::Session session(key_path, bridge.GetAddress(), nullptr);
        i2p::Connection conn;
        bool proxy_error{true};
        BOOST_CHECK(!session.Connect(CService(i2p::DestBinToAddr(MakePeerDestination()), 0), conn, proxy_error));
    }
    LogInstance().DisableCategory(BCLog::I2P);

    // The bridge was asked for a destination, so the failing path was hit.
    BOOST_CHECK_EQUAL(bridge.GetDestGenerateCount(), 1);

    std::string log;
    if (fs::exists(log_path)) {
        std::ifstream file(log_path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        log = buffer.str();
    }

    // The error is reported ...
    BOOST_CHECK(log.find("Cannot decode the Base64 value") != std::string::npos);
    // ... but the private key is not in the log.
    BOOST_CHECK(log.find(MALFORMED_PRIVATE_KEY) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(session_accept)
{
    MockSamBridge bridge;
    i2p::sam::Session session(PrivateKeyPath(), bridge.GetAddress(), nullptr);

    i2p::Connection conn;
    BOOST_REQUIRE(session.Listen(conn));
    BOOST_CHECK(conn.sock != INVALID_SOCKET);
    BOOST_CHECK(conn.me.IsI2P());

    BOOST_REQUIRE(session.Accept(conn));
    BOOST_CHECK(conn.peer.IsI2P());
    BOOST_CHECK_EQUAL(conn.peer.GetPort(), 0);
    BOOST_CHECK(conn.peer == CService(i2p::DestBinToAddr(MakePeerDestination()), 0));

    CloseSocket(conn.sock);
    conn.sock = INVALID_SOCKET;
}

BOOST_AUTO_TEST_SUITE_END()
