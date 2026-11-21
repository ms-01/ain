// Copyright (c) 2024-present The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <i2p.h>

#include <compat/endian.h>
#include <crypto/sha256.h>
#include <fs.h>
#include <logging.h>
#include <random.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace i2p {

/**
 * Swap Standard Base64 <-> I2P Base64.
 * Standard Base64 uses `+` and `/` as last two characters of its alphabet.
 * I2P Base64 uses `-` and `~` respectively.
 * So it is easy to detect in which one is the input and convert to the other.
 * @param[in] from Input to convert.
 * @return converted `from`
 */
std::string SwapBase64(const std::string& from)
{
    std::string to;
    to.resize(from.size());
    for (size_t i = 0; i < from.size(); ++i) {
        switch (from[i]) {
        case '-':
            to[i] = '+';
            break;
        case '~':
            to[i] = '/';
            break;
        case '+':
            to[i] = '-';
            break;
        case '/':
            to[i] = '~';
            break;
        default:
            to[i] = from[i];
            break;
        }
    }
    return to;
}

/**
 * Decode an I2P-style Base64 string.
 *
 * The decoded value is never part of the error message: the value can be
 * sensitive (it is the private key of the node in the "DEST GENERATE" reply)
 * and the error message ends up in the debug log.
 *
 * @param[in] i2p_b64 I2P-style Base64 string.
 * @return decoded `i2p_b64`
 * @throw std::runtime_error if decoding fails
 */
Binary DecodeI2PBase64(const std::string& i2p_b64)
{
    const std::string std_b64 = SwapBase64(i2p_b64);
    bool invalid{false};
    const std::string decoded = DecodeBase64(std_b64, &invalid);
    if (invalid) {
        throw std::runtime_error(strprintf("Cannot decode the Base64 value (length: %d)", i2p_b64.size()));
    }
    return Binary(decoded.begin(), decoded.end());
}

/**
 * Derive the .b32.i2p address of an I2P destination (binary).
 * @param[in] dest I2P destination.
 * @return the address that corresponds to `dest`
 * @throw std::runtime_error if conversion fails
 */
CNetAddr DestBinToAddr(const Binary& dest)
{
    CSHA256 hasher;
    hasher.Write(dest.data(), dest.size());
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(hash);

    CNetAddr addr;
    const std::string addr_str = EncodeBase32(hash, sizeof(hash), false /* don't pad with = */) + ".b32.i2p";
    if (!addr.SetSpecial(addr_str)) {
        throw std::runtime_error(strprintf("Cannot parse I2P address: \"%s\"", addr_str));
    }

    return addr;
}

/**
 * Derive the .b32.i2p address of an I2P destination (I2P-style Base64).
 * @param[in] dest I2P destination.
 * @return the address that corresponds to `dest`
 * @throw std::runtime_error if conversion fails
 */
CNetAddr DestB64ToAddr(const std::string& dest)
{
    const Binary decoded = DecodeI2PBase64(dest);
    return DestBinToAddr(decoded);
}

namespace {

/**
 * Split a string at every occurrence of the given separator.
 * (The chain does not have a `spanparsing::Split()` helper.)
 */
std::vector<std::string> SplitString(const std::string& in, char sep)
{
    std::vector<std::string> out;
    std::string::size_type start{0};
    while (true) {
        const std::string::size_type pos = in.find(sep, start);
        if (pos == std::string::npos) {
            out.emplace_back(in.substr(start));
            break;
        }
        out.emplace_back(in.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

/**
 * Wait until the socket is readable (or the timeout expires).
 * @param[in] sock The socket to wait for.
 * @param[in] timeout The maximum time to wait.
 * @return 1 if the socket is readable, 0 on timeout, -1 on error
 */
int WaitForReadable(SOCKET sock, std::chrono::milliseconds timeout)
{
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv;
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    const int ret = select(sock + 1, &fdset, nullptr, nullptr, &tv);
    if (ret < 0) return -1;
    return ret > 0 ? 1 : 0;
}

/**
 * Wait until the socket is writable (or the timeout expires).
 * @param[in] sock The socket to wait for.
 * @param[in] timeout The maximum time to wait.
 * @return 1 if the socket is writable, 0 on timeout, -1 on error
 */
int WaitForWritable(SOCKET sock, std::chrono::milliseconds timeout)
{
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv;
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    const int ret = select(sock + 1, nullptr, &fdset, nullptr, &tv);
    if (ret < 0) return -1;
    return ret > 0 ? 1 : 0;
}

/**
 * Send the whole buffer over the socket.
 *
 * The socket is non-blocking, so a partial write or a "would block" has to be
 * retried (the socket is waited for until it becomes writable).
 *
 * @param[in] sock The socket to write to.
 * @param[in] data The data to send.
 * @return true on success
 */
bool SendAll(SOCKET sock, const std::string& data)
{
    // The SAM requests are short, so a send that blocks for more than a few
    // seconds means that the bridge is not reading anything.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    size_t sent{0};
    while (sent < data.size()) {
        const ssize_t n = send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            const int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            if (err == WSAEWOULDBLOCK) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                const int waited = WaitForWritable(sock, std::chrono::milliseconds(100));
                if (waited < 0) return false;
                continue;
            }
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

/**
 * Mask the sensitive parts of a request before it is put into a message.
 *
 * The "SESSION CREATE" request contains the private key of the node, so it
 * must never appear in a log message or in an error message.
 *
 * @param[in] request The request to mask.
 * @return the masked request
 */
std::string MaskRequest(const std::string& request)
{
    return request.rfind("SESSION CREATE", 0) == 0 ? "SESSION CREATE ..." : request;
}

/**
 * Read from the socket until the terminator is found.
 *
 * No byte past the terminator is consumed: the data is peeked at first and
 * only the bytes up to (and including) the terminator are read.
 *
 * @param[in] sock The socket to read from.
 * @param[in] terminator The terminator character.
 * @param[in] timeout The maximum time to wait for the complete message.
 * @param[in] interrupt If this is signaled the read is aborted.
 * @param[in] max_size The maximum size of the message.
 * @return the message without the terminator
 * @throw std::runtime_error on timeout, interruption, overflow or socket error
 */
std::string RecvUntilTerminator(SOCKET sock, char terminator, std::chrono::milliseconds timeout,
                                 CThreadInterrupt* interrupt, size_t max_size)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string data;

    while (true) {
        if (interrupt != nullptr && static_cast<bool>(*interrupt)) {
            throw std::runtime_error("interrupted");
        }
        if (data.size() >= max_size) {
            throw std::runtime_error(strprintf("Received too many bytes without a terminator (%d)", data.size()));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("timeout while waiting for a reply from the SAM bridge");
        }

        char buf[512];
        const size_t want = std::min(sizeof(buf), max_size - data.size());
        const ssize_t peeked = recv(sock, buf, want, MSG_PEEK);
        if (peeked < 0) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
                // No data (yet): wait for the socket to become readable.
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                const int waited = WaitForReadable(sock, std::min(remaining, std::chrono::milliseconds(100)));
                if (waited < 0) {
                    throw std::runtime_error(strprintf("select(): %s", NetworkErrorString(WSAGetLastError())));
                }
                continue;
            }
            throw std::runtime_error(strprintf("recv(): %s", NetworkErrorString(err)));
        }
        if (peeked == 0) {
            throw std::runtime_error("Connection unexpectedly closed by peer");
        }

        char* end = buf + peeked;
        char* term = std::find(buf, end, terminator);
        const bool terminator_found = term != end;
        const size_t try_len = terminator_found ? static_cast<size_t>(term - buf) + 1 : static_cast<size_t>(peeked);

        const ssize_t read_ret = recv(sock, buf, try_len, 0);
        if (read_ret < 0 || static_cast<size_t>(read_ret) != try_len) {
            throw std::runtime_error("recv() returned an unexpected number of bytes");
        }

        // Don't include the terminator in the output.
        data.append(buf, buf + (terminator_found ? try_len - 1 : try_len));

        if (terminator_found) return data;
    }
}

/**
 * Whether the given private key holds a structurally valid I2P destination.
 */
bool IsValidPrivateKey(const Binary& key)
{
    // From https://i2p.net/en/docs/specs/common-structures/#destination:
    // "They are 387 bytes plus the certificate length specified at bytes 385-386,
    // which may be non-zero"
    static constexpr size_t DEST_LEN_BASE = 387;
    static constexpr size_t CERT_LEN_POS = 385;
    if (key.size() < CERT_LEN_POS + sizeof(uint16_t)) return false;
    uint16_t cert_len{0};
    memcpy(&cert_len, &key.at(CERT_LEN_POS), sizeof(cert_len));
    cert_len = be16toh(cert_len);
    return key.size() >= DEST_LEN_BASE + cert_len;
}

/**
 * Read the private key from the given file.
 * @return true if the file was read and holds a structurally valid key
 */
bool ReadPrivateKeyFile(const fs::path& path, Binary& key_out)
{
    FILE* file = fsbridge::fopen(path, "rb");
    if (file == nullptr) return false;
    Binary key;
    char buf[512];
    size_t read{0};
    while ((read = fread(buf, 1, sizeof(buf), file)) > 0) {
        key.insert(key.end(), buf, buf + read);
        if (key.size() > 4096) break; // A private key is a few hundred bytes.
    }
    fclose(file);
    if (!IsValidPrivateKey(key)) return false;
    key_out = std::move(key);
    return true;
}

/**
 * Write the private key to the given file (atomically, with mode 0600).
 * @return true on success
 */
bool WritePrivateKeyFile(const fs::path& path, const Binary& key)
{
    const fs::path tmp_path = fs::PathFromString(fs::PathToString(path) + ".tmp");
    FILE* file = fsbridge::fopen(tmp_path, "wb");
    if (file == nullptr) return false;
    const size_t written = fwrite(key.data(), 1, key.size(), file);
    const bool ok = written == key.size();
    fclose(file);
    if (!ok) {
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return false;
    }
    // The private key must not be readable by anyone but the owner.
    std::error_code ec;
    fs::permissions(tmp_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) {
        LogPrint(BCLog::I2P, "Cannot set the permissions of the I2P private key file: %s\n", ec.message());
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        LogPrint(BCLog::I2P, "Cannot rename the I2P private key file: %s\n", ec.message());
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        return false;
    }
    return true;
}

} // namespace

CService NormalizeI2PService(const CService& service)
{
    if (!service.IsI2P()) return service;
    return CService(service, sam::I2P_SAM31_PORT);
}

namespace sam {

bool Reply::Has(const std::string& key) const
{
    return keys.find(key) != keys.end();
}

std::string Reply::Get(const std::string& key) const
{
    const auto& pos = keys.find(key);
    if (pos == keys.end() || !pos->second.has_value()) {
        throw std::runtime_error(
            strprintf("Missing %s= in the reply to \"%s\"", key, request));
    }
    return pos->second.value();
}

Session::Session(const fs::path& private_key_file, const CService& control_host, CThreadInterrupt* interrupt)
    : m_private_key_file{private_key_file},
      m_control_host{control_host},
      m_interrupt{interrupt}
{
}

Session::~Session()
{
    LOCK(m_mutex);
    Disconnect();
}

bool Session::Listen(Connection& conn)
{
    try {
        LOCK(m_mutex);
        CreateIfNotCreatedAlready();
        conn.me = m_my_addr;
        conn.sock = StreamAccept();
        return true;
    } catch (const std::runtime_error& e) {
        LogPrint(BCLog::I2P, "Couldn't listen: %s\n", e.what());
        CheckControlSock();
    }
    return false;
}

bool Session::Accept(Connection& conn)
{
    std::string errmsg;
    bool disconnect{false};

    while (true) {
        if (m_interrupt != nullptr && static_cast<bool>(*m_interrupt)) {
            LogPrint(BCLog::I2P, "Accept was interrupted\n");
            return false;
        }

        std::string peer_dest;
        try {
            peer_dest = RecvUntilTerminator(conn.sock, '\n', std::chrono::minutes(3), m_interrupt, MAX_MSG_SIZE);
        } catch (const std::runtime_error& e) {
            errmsg = e.what();
            break;
        }

        CNetAddr peer_addr;
        try {
            peer_addr = DestB64ToAddr(peer_dest);
        } catch (const std::runtime_error& e) {
            // The I2P router is expected to send the Base64 of the connecting peer,
            // but it may happen that something like this is sent instead:
            // STREAM STATUS RESULT=I2P_ERROR MESSAGE="Session was closed"
            // In that case consider the session damaged and close it right away,
            // even if the control socket is alive.
            if (peer_dest.find("RESULT=I2P_ERROR") != std::string::npos) {
                errmsg = strprintf("unexpected reply that hints the session is unusable: %s", peer_dest);
                disconnect = true;
            } else {
                errmsg = e.what();
            }
            break;
        }

        conn.peer = CService(peer_addr, I2P_SAM31_PORT);

        return true;
    }

    if (m_interrupt != nullptr && static_cast<bool>(*m_interrupt)) {
        LogPrint(BCLog::I2P, "Accept was interrupted\n");
    } else {
        LogPrint(BCLog::I2P, "Error accepting%s: %s\n", disconnect ? " (will close the session)" : "", errmsg);
    }
    if (disconnect) {
        LOCK(m_mutex);
        Disconnect();
    } else {
        CheckControlSock();
    }
    return false;
}

bool Session::Connect(const CService& to, Connection& conn, bool& proxy_error)
{
    proxy_error = true;

    // I2P destinations are portless (SAM 3.1). The port of the destination is
    // accepted but ignored, the peer is always normalized to port 0.
    if (to.GetPort() != I2P_SAM31_PORT) {
        LogPrint(BCLog::I2P, "Connecting to %s, ignoring the port %d (I2P destinations are portless)\n",
                 to.ToStringIP(), to.GetPort());
    }
    conn.peer = CService(to, I2P_SAM31_PORT);

    std::string session_id;
    SOCKET sock{INVALID_SOCKET};

    try {
        {
            LOCK(m_mutex);
            CreateIfNotCreatedAlready();
            session_id = m_session_id;
            conn.me = m_my_addr;
            sock = Hello();
        }

        const Reply lookup_reply =
            SendRequestAndGetReply(sock, strprintf("NAMING LOOKUP NAME=%s", to.ToStringIP()));

        // Upstream uses "VALUE", some bridges use "DESTINATION".
        std::string dest;
        if (lookup_reply.Has("VALUE")) {
            dest = lookup_reply.Get("VALUE");
        } else if (lookup_reply.Has("DESTINATION")) {
            dest = lookup_reply.Get("DESTINATION");
        } else {
            throw std::runtime_error(
                strprintf("Missing VALUE= in the reply to \"%s\"", lookup_reply.request));
        }

        const Reply connect_reply = SendRequestAndGetReply(
            sock, strprintf("STREAM CONNECT ID=%s DESTINATION=%s SILENT=false", session_id, dest),
            false);

        const std::string& result = connect_reply.Get("RESULT");

        if (result == "OK") {
            conn.sock = sock;
            return true;
        }

        if (result == "INVALID_ID") {
            LOCK(m_mutex);
            Disconnect();
            throw std::runtime_error("Invalid session id");
        }

        if (result == "CANT_REACH_PEER" || result == "TIMEOUT") {
            // The peer is (most likely) down, this is not a problem of the bridge.
            proxy_error = false;
        }

        throw std::runtime_error(strprintf("\"%s\"", connect_reply.full));
    } catch (const std::runtime_error& e) {
        LogPrint(BCLog::I2P, "Error connecting to %s: %s\n", to.ToStringIPPort(), e.what());
        if (sock != INVALID_SOCKET) CloseSocket(sock);
        CheckControlSock();
        return false;
    }
}

// Private methods

Reply Session::SendRequestAndGetReply(SOCKET sock, const std::string& request, bool check_result_ok) const
{
    if (!SendAll(sock, request + "\n")) {
        // Don't log the full "SESSION CREATE ..." because it contains our private key.
        throw std::runtime_error(
            strprintf("Cannot send the request to the SAM bridge: \"%s\"", MaskRequest(request)));
    }

    Reply reply;

    // Don't log the full "SESSION CREATE ..." because it contains our private key.
    reply.request = MaskRequest(request);

    // It could take a few minutes for the I2P router to reply as it is querying the I2P network
    // (when doing a name lookup, for example). The read is interrupted regularly,
    // so we are not stuck here for long if the interrupt is signaled.
    static constexpr auto recv_timeout = std::chrono::minutes(3);

    reply.full = RecvUntilTerminator(sock, '\n', recv_timeout, m_interrupt, MAX_MSG_SIZE);

    for (const auto& kv : SplitString(reply.full, ' ')) {
        const auto pos = kv.find('=');
        if (pos != std::string::npos) {
            reply.keys.emplace(kv.substr(0, pos), kv.substr(pos + 1));
        } else {
            reply.keys.emplace(kv, std::nullopt);
        }
    }

    if (check_result_ok && reply.Get("RESULT") != "OK") {
        throw std::runtime_error(
            strprintf("Reply to \"%s\": had a RESULT not equal to OK.", reply.request));
    }

    return reply;
}

SOCKET Session::Hello() const
{
    SOCKET sock = CreateSocket(m_control_host);
    if (sock == INVALID_SOCKET) {
        throw std::runtime_error(
            strprintf("Cannot create a socket for the SAM bridge %s", m_control_host.ToStringIPPort()));
    }
    if (!ConnectSocketDirectly(m_control_host, sock, nConnectTimeout, false)) {
        CloseSocket(sock);
        throw std::runtime_error(strprintf("Cannot connect to the SAM bridge %s", m_control_host.ToStringIPPort()));
    }

    SendRequestAndGetReply(sock, "HELLO VERSION MIN=3.1 MAX=3.1");

    return sock;
}

void Session::CheckControlSock()
{
    LOCK(m_mutex);

    if (m_control_sock == INVALID_SOCKET) return;

    // A peek on a non-blocking socket returns EWOULDBLOCK if the bridge is
    // still there and 0 if it closed the connection.
    char buf{0};
    const ssize_t nRet = recv(m_control_sock, &buf, 1, MSG_PEEK);
    if (nRet == 0) {
        LogPrint(BCLog::I2P, "The SAM bridge closed the control socket\n");
        Disconnect();
    } else if (nRet < 0 && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINTR) {
        LogPrint(BCLog::I2P, "Control socket error: %s\n", NetworkErrorString(WSAGetLastError()));
        Disconnect();
    }
}

void Session::DestGenerate(SOCKET sock)
{
    // https://i2p.net/en/docs/specs/common-structures/#key-certificates
    // "7" or "EdDSA_SHA512_Ed25519" - "Recent Router Identities and Destinations".
    // Use "7" because i2pd <2.24.0 does not recognize the textual form.
    // If SIGNATURE_TYPE is not specified, then the default one is DSA_SHA1.
    const Reply reply = SendRequestAndGetReply(sock, "DEST GENERATE SIGNATURE_TYPE=7", false);

    // Upstream uses "PRIV", some bridges use "PRIVATEKEY".
    std::string priv;
    if (reply.Has("PRIV")) {
        priv = reply.Get("PRIV");
    } else if (reply.Has("PRIVATEKEY")) {
        priv = reply.Get("PRIVATEKEY");
    } else {
        throw std::runtime_error(
            strprintf("Missing PRIV= in the reply to \"%s\"", reply.request));
    }

    try {
        m_private_key = DecodeI2PBase64(priv);
    } catch (const std::runtime_error&) {
        // The value is the private key of the node: it must never end up in an
        // error message (the error message is written to the debug log).
        throw std::runtime_error(
            strprintf("Cannot decode the Base64 value of the reply to \"%s\" (length: %d)",
                      reply.request,
                      priv.size()));
    }
}

void Session::GenerateAndSavePrivateKey(SOCKET sock)
{
    DestGenerate(sock);

    if (!WritePrivateKeyFile(m_private_key_file, m_private_key)) {
        throw std::runtime_error(
            strprintf("Cannot save the I2P private key to %s", fs::quoted(fs::PathToString(m_private_key_file))));
    }
}

Binary Session::MyDestination() const
{
    static constexpr size_t DEST_LEN_BASE = 387;
    static constexpr size_t CERT_LEN_POS = 385;

    uint16_t cert_len{0};

    if (m_private_key.size() < CERT_LEN_POS + sizeof(cert_len)) {
        throw std::runtime_error(strprintf("The private key is too short (%d < %d)",
                                           m_private_key.size(),
                                           CERT_LEN_POS + sizeof(cert_len)));
    }

    memcpy(&cert_len, &m_private_key.at(CERT_LEN_POS), sizeof(cert_len));
    cert_len = be16toh(cert_len);

    const size_t dest_len = DEST_LEN_BASE + cert_len;

    if (dest_len > m_private_key.size()) {
        throw std::runtime_error(strprintf("Certificate length (%d) designates that the private key should "
                                           "be %d bytes, but it is only %d bytes",
                                           cert_len,
                                           dest_len,
                                           m_private_key.size()));
    }

    return Binary{m_private_key.begin(), m_private_key.begin() + dest_len};
}

void Session::CreateIfNotCreatedAlready()
{
    if (m_control_sock != INVALID_SOCKET) {
        // Check whether the control socket is still alive.
        char buf{0};
        const ssize_t nRet = recv(m_control_sock, &buf, 1, MSG_PEEK);
        if (nRet != 0) return;
        // The bridge closed the connection: the session has to be re-created.
        LogPrint(BCLog::I2P, "The SAM bridge closed the control socket, recreating the session\n");
        Disconnect();
    }

    // The full session id is overkill, it is too verbose in the logs.
    const std::string session_id = GetRandHash().GetHex().substr(0, 10);

    LogPrint(BCLog::I2P, "Creating persistent I2P SAM session %s with %s\n", session_id,
             m_control_host.ToStringIPPort());

    SOCKET sock = Hello();

    // Read our persistent destination (private key) from disk or generate one
    // and save it to disk. Then use it when creating the session.
    Binary key;
    if (ReadPrivateKeyFile(m_private_key_file, key)) {
        m_private_key = std::move(key);
    } else {
        if (fs::exists(m_private_key_file)) {
            LogPrint(BCLog::I2P, "The I2P private key file %s is invalid, generating a new key\n",
                     fs::PathToString(m_private_key_file));
        }
        GenerateAndSavePrivateKey(sock);
    }

    const std::string private_key_b64 =
        SwapBase64(EncodeBase64(std::string(m_private_key.begin(), m_private_key.end())));

    SendRequestAndGetReply(sock,
                           strprintf("SESSION CREATE STYLE=STREAM ID=%s DESTINATION=%s "
                                     "i2cp.leaseSetEncType=6,4 inbound.quantity=3 outbound.quantity=3",
                                     session_id,
                                     private_key_b64));

    m_my_addr = CService(DestBinToAddr(MyDestination()), I2P_SAM31_PORT);
    m_session_id = session_id;
    m_control_sock = sock;

    LogPrint(BCLog::I2P, "Persistent I2P SAM session %s created, my address=%s\n",
             m_session_id, m_my_addr.ToStringIP());
}

SOCKET Session::StreamAccept()
{
    SOCKET sock = Hello();

    const Reply reply = SendRequestAndGetReply(
        sock, strprintf("STREAM ACCEPT ID=%s SILENT=false", m_session_id), false);

    const std::string& result = reply.Get("RESULT");

    if (result == "OK") {
        return sock;
    }

    CloseSocket(sock);

    if (result == "INVALID_ID") {
        // If our session id is invalid, then force the session re-creation on the next usage.
        Disconnect();
    }

    throw std::runtime_error(strprintf("\"%s\"", reply.full));
}

void Session::Disconnect()
{
    if (m_control_sock != INVALID_SOCKET) {
        if (m_session_id.empty()) {
            LogPrint(BCLog::I2P, "Destroying incomplete I2P SAM session\n");
        } else {
            LogPrint(BCLog::I2P, "Destroying I2P SAM session %s\n", m_session_id);
        }
        CloseSocket(m_control_sock);
        m_control_sock = INVALID_SOCKET;
    }
    m_session_id.clear();
}

} // namespace sam
} // namespace i2p
