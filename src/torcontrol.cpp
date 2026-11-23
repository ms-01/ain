// Copyright (c) 2015-2018 The Bitcoin Core developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <torcontrol.h>
#include <util/strencodings.h>
#include <net.h>
#include <netaddress.h>
#include <netbase.h>
#include <util/sock.h>
#include <util/system.h>
#include <threadinterrupt.h>
#include <crypto/hmac_sha256.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <deque>
#include <set>
#include <stdlib.h>

#include <boost/signals2/signal.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>

/** Default control port */
const std::string DEFAULT_TOR_CONTROL = "127.0.0.1:9051";
/** The key type requested for the hidden service: a Tor v3 (ED25519-V3) service */
const std::string TOR_ONION_KEY_TYPE = "NEW:ED25519-V3";
/** Tor cookie size (from control-spec.txt) */
static const int TOR_COOKIE_SIZE = 32;
/** Size of client/server nonce for SAFECOOKIE */
static const int TOR_NONCE_SIZE = 32;
/** For computing serverHash in SAFECOOKIE */
static const std::string TOR_SAFE_SERVERKEY = "Tor safe cookie authentication server-to-controller hash";
/** For computing clientHash in SAFECOOKIE */
static const std::string TOR_SAFE_CLIENTKEY = "Tor safe cookie authentication controller-to-server hash";
/** Exponential backoff configuration - initial timeout in seconds */
static const float RECONNECT_TIMEOUT_START = 1.0;
/** Exponential backoff configuration - growth factor */
static const float RECONNECT_TIMEOUT_EXP = 1.5;
/** Maximum length for lines received on TorControlConnection.
 * tor-control-spec.txt mentions that there is explicitly no limit defined to line length,
 * this is belt-and-suspenders sanity limit to prevent memory exhaustion.
 */
static const int MAX_LINE_LENGTH = 100000;

/****** Low-level TorControlConnection ********/

/** Reply from Tor, can be single or multi-line */
class TorControlReply
{
public:
    TorControlReply() { Clear(); }

    int code;
    std::vector<std::string> lines;

    void Clear()
    {
        code = 0;
        lines.clear();
    }
};

/** Low-level handling for Tor control connection.
 * Speaks the SMTP-like protocol as defined in torspec/control-spec.txt
 */
class TorControlConnection
{
public:
    typedef std::function<void(TorControlConnection&)> ConnectionCB;
    typedef std::function<void(TorControlConnection &,const TorControlReply &)> ReplyHandlerCB;

    /** Create a new TorControlConnection.
     */
    TorControlConnection();
    ~TorControlConnection();

    /**
     * Connect to a Tor control port.
     * target is address of the form host:port.
     * connected is the handler that is called when connection is successfully established.
     * disconnected is a handler that is called when the connection is broken.
     * Return true on success.
     */
    bool Connect(const std::string &target, const ConnectionCB& connected, const ConnectionCB& disconnected);

    /**
     * Disconnect from Tor control port.
     */
    void Disconnect();

    /**
     * Check if the connection is established.
     */
    bool IsConnected() const;

    /**
     * Wait for data to become available on the socket.
     * @param[in] timeout Maximum time to wait
     * @return true if data is available, false on timeout or error
     */
    bool WaitForData(std::chrono::milliseconds timeout);

    /**
     * Read the available data from the socket and process the complete replies.
     * Dispatches to the registered reply handlers.
     * @return true if the connection is still open, false if it was closed
     */
    bool ReceiveAndProcess();

    /** Send a command, register a handler for the reply.
     * A trailing CRLF is automatically added.
     * Return true on success.
     */
    bool Command(const std::string &cmd, const ReplyHandlerCB& reply_handler);

    /** Response handlers for async replies */
    boost::signals2::signal<void(TorControlConnection &,const TorControlReply &)> async_handler;
private:
    /** Callback when ready for use */
    std::function<void(TorControlConnection&)> connected;
    /** Callback when connection lost */
    std::function<void(TorControlConnection&)> disconnected;
    /** Connection to control socket */
    std::shared_ptr<Sock> b_conn;
    /** Buffer for the incoming data */
    std::string recv_buffer;
    /** Interrupt used to abort a pending send */
    CThreadInterrupt send_interrupt;
    /** Message being received */
    TorControlReply message;
    /** Response handlers */
    std::deque<ReplyHandlerCB> reply_handlers;

    /** Process the complete lines in the receive buffer. */
    bool ProcessBuffer();
};

TorControlConnection::TorControlConnection() = default;

TorControlConnection::~TorControlConnection()
{
    Disconnect();
}

bool TorControlConnection::ProcessBuffer()
{
    size_t pos = 0;
    while (true) {
        const size_t line_end = recv_buffer.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        const std::string s = recv_buffer.substr(pos, line_end - pos);
        pos = line_end + 2;
        if (s.size() < 4) // Short line
            continue;
        // <status>(-|+| )<data><CRLF>
        message.code = atoi(s.substr(0,3));
        message.lines.push_back(s.substr(4));
        char ch = s[3]; // '-','+' or ' '
        if (ch == ' ') {
            // Final line, dispatch reply and clean up
            if (message.code >= 600) {
                // Dispatch async notifications to async handler
                // Synchronous and asynchronous messages are never interleaved
                async_handler(*this, message);
            } else {
                if (!reply_handlers.empty()) {
                    // Invoke reply handler with message
                    reply_handlers.front()(*this, message);
                    reply_handlers.pop_front();
                } else {
                    LogPrint(BCLog::TOR, "tor: Received unexpected sync reply %i\n", message.code);
                }
            }
            message.Clear();
        }
    }
    // Remove the processed data from the buffer
    recv_buffer.erase(0, pos);
    //  Check for size of buffer - protect against memory exhaustion with very long lines
    //  Do this after the lines have been removed from the buffer. Everything left is an incomplete line.
    if (recv_buffer.size() > MAX_LINE_LENGTH) {
        LogPrintf("tor: Disconnecting because MAX_LINE_LENGTH exceeded\n");
        Disconnect();
        return false;
    }
    return true;
}

bool TorControlConnection::IsConnected() const
{
    if (!b_conn) return false;
    std::string errmsg;
    return b_conn->IsConnected(errmsg);
}

bool TorControlConnection::WaitForData(std::chrono::milliseconds timeout)
{
    if (!b_conn) return false;
    Sock::Event event{0};
    if (!b_conn->Wait(timeout, Sock::RecvEvent, &event)) {
        return false;
    }
    if (event & Sock::ErrorEvent) {
        LogPrint(BCLog::TOR, "tor: Error connecting to Tor control socket\n");
        Disconnect();
        if (disconnected) disconnected(*this);
        return false;
    }
    return (event & Sock::RecvEvent);
}

bool TorControlConnection::ReceiveAndProcess()
{
    if (!b_conn) return false;

    char buf[4096];
    const ssize_t nread = b_conn->Recv(buf, sizeof(buf), MSG_DONTWAIT);

    if (nread < 0) {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) {
            // No data available currently
            return true;
        }
        LogPrint(BCLog::TOR, "tor: Error reading from socket: %s\n", NetworkErrorString(err));
        Disconnect();
        if (disconnected) disconnected(*this);
        return false;
    }
    if (nread == 0) {
        LogPrint(BCLog::TOR, "tor: End of stream\n");
        Disconnect();
        if (disconnected) disconnected(*this);
        return false;
    }
    recv_buffer.append(buf, buf + nread);
    return ProcessBuffer();
}

bool TorControlConnection::Connect(const std::string &target, const ConnectionCB& _connected, const ConnectionCB&  _disconnected)
{
    if (b_conn)
        Disconnect();
    // Parse target address:port
    CService connect_to;
    if (!Lookup(target.c_str(), connect_to, 9051, fNameLookup)) {
        LogPrintf("tor: Error parsing socket address %s\n", target);
        return false;
    }

    this->connected = _connected;
    this->disconnected = _disconnected;

    // Create a new socket and connect to the target
    const SOCKET hSocket = CreateSocket(connect_to);
    if (hSocket == INVALID_SOCKET) {
        LogPrintf("tor: Error connecting to address %s\n", target);
        return false;
    }
    if (!ConnectSocketDirectly(connect_to, hSocket, nConnectTimeout, true)) {
        LogPrintf("tor: Error connecting to address %s\n", target);
        SOCKET socket_to_close = hSocket;
        CloseSocket(socket_to_close);
        return false;
    }
    b_conn = std::make_shared<Sock>(hSocket);
    (void)b_conn->SetNonBlocking();
    recv_buffer.clear();
    message.Clear();
    reply_handlers.clear();

    LogPrint(BCLog::TOR, "tor: Successfully connected!\n");
    if (connected) connected(*this);
    return true;
}

void TorControlConnection::Disconnect()
{
    b_conn.reset();
    recv_buffer.clear();
    message.Clear();
    reply_handlers.clear();
}

bool TorControlConnection::Command(const std::string &cmd, const ReplyHandlerCB& reply_handler)
{
    if (!b_conn)
        return false;
    const std::string command = cmd + "\r\n";
    try {
        b_conn->SendComplete(command, std::chrono::seconds{10}, send_interrupt);
    } catch (const std::runtime_error& e) {
        LogPrint(BCLog::TOR, "tor: Error sending command: %s\n", e.what());
        return false;
    }
    reply_handlers.push_back(reply_handler);
    return true;
}

/****** General parsing utilities ********/

/* Split reply line in the form 'AUTH METHODS=...' into a type
 * 'AUTH' and arguments 'METHODS=...'.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21) and AUTHCHALLENGE (S3.24).
 */
std::pair<std::string,std::string> SplitTorReplyLine(const std::string &s)
{
    size_t ptr=0;
    std::string type;
    while (ptr < s.size() && s[ptr] != ' ') {
        type.push_back(s[ptr]);
        ++ptr;
    }
    if (ptr < s.size())
        ++ptr; // skip ' '
    return make_pair(type, s.substr(ptr));
}

/** Parse reply arguments in the form 'METHODS=COOKIE,SAFECOOKIE COOKIEFILE=".../control_auth_cookie"'.
 * Returns a map of keys to values, or an empty map if there was an error.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21), AUTHCHALLENGE (S3.24),
 * and ADD_ONION (S3.27). See also sections 2.1 and 2.3.
 */
std::map<std::string,std::string> ParseTorReplyMapping(const std::string &s)
{
    std::map<std::string,std::string> mapping;
    size_t ptr=0;
    while (ptr < s.size()) {
        std::string key, value;
        while (ptr < s.size() && s[ptr] != '=' && s[ptr] != ' ') {
            key.push_back(s[ptr]);
            ++ptr;
        }
        if (ptr == s.size()) // unexpected end of line
            return std::map<std::string,std::string>();
        if (s[ptr] == ' ') // The remaining string is an OptArguments
            break;
        ++ptr; // skip '='
        if (ptr < s.size() && s[ptr] == '"') { // Quoted string
            ++ptr; // skip opening '"'
            bool escape_next = false;
            while (ptr < s.size() && (escape_next || s[ptr] != '"')) {
                // Repeated backslashes must be interpreted as pairs
                escape_next = (s[ptr] == '\\' && !escape_next);
                value.push_back(s[ptr]);
                ++ptr;
            }
            if (ptr == s.size()) // unexpected end of line
                return std::map<std::string,std::string>();
            ++ptr; // skip closing '"'
            /**
             * Unescape value. Per https://spec.torproject.org/control-spec section 2.1.1:
             *
             *   For future-proofing, controller implementors MAY use the following
             *   rules to be compatible with buggy Tor implementations and with
             *   future ones that implement the spec as intended:
             *
             *     Read \n \t \r and \0 ... \377 as C escapes.
             *     Treat a backslash followed by any other character as that character.
             */
            std::string escaped_value;
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\') {
                    // This will always be valid, because if the QuotedString
                    // ended in an odd number of backslashes, then the parser
                    // would already have returned above, due to a missing
                    // terminating double-quote.
                    ++i;
                    if (value[i] == 'n') {
                        escaped_value.push_back('\n');
                    } else if (value[i] == 't') {
                        escaped_value.push_back('\t');
                    } else if (value[i] == 'r') {
                        escaped_value.push_back('\r');
                    } else if ('0' <= value[i] && value[i] <= '7') {
                        size_t j;
                        // Octal escape sequences have a limit of three octal digits,
                        // but terminate at the first character that is not a valid
                        // octal digit if encountered sooner.
                        for (j = 1; j < 3 && (i+j) < value.size() && '0' <= value[i+j] && value[i+j] <= '7'; ++j) {}
                        // Tor restricts first digit to 0-3 for three-digit octals.
                        // A leading digit of 4-7 would therefore be interpreted as
                        // a two-digit octal.
                        if (j == 3 && value[i] > '3') {
                            j--;
                        }
                        escaped_value.push_back(strtol(value.substr(i, j).c_str(), nullptr, 8));
                        // Account for automatic incrementing at loop end
                        i += j - 1;
                    } else {
                        escaped_value.push_back(value[i]);
                    }
                } else {
                    escaped_value.push_back(value[i]);
                }
            }
            value = escaped_value;
        } else { // Unquoted value. Note that values can contain '=' at will, just no spaces
            while (ptr < s.size() && s[ptr] != ' ') {
                value.push_back(s[ptr]);
                ++ptr;
            }
        }
        if (ptr < s.size() && s[ptr] == ' ')
            ++ptr; // skip ' ' after key=value
        mapping[key] = value;
    }
    return mapping;
}

/** Read full contents of a file and return them in a std::string.
 * Returns a pair <status, string>.
 * If an error occurred, status will be false, otherwise status will be true and the data will be returned in string.
 *
 * @param maxsize Puts a maximum size limit on the file that is read. If the file is larger than this, truncated data
 *         (with len > maxsize) will be returned.
 */
static std::pair<bool,std::string> ReadBinaryFile(const fs::path &filename, size_t maxsize=std::numeric_limits<size_t>::max())
{
    FILE *f = fsbridge::fopen(filename, "rb");
    if (f == nullptr)
        return std::make_pair(false,"");
    std::string retval;
    char buffer[128];
    size_t n;
    while ((n=fread(buffer, 1, sizeof(buffer), f)) > 0) {
        // Check for reading errors so we don't return any data if we couldn't
        // read the entire file (or up to maxsize)
        if (ferror(f)) {
            fclose(f);
            return std::make_pair(false,"");
        }
        retval.append(buffer, buffer+n);
        if (retval.size() > maxsize)
            break;
    }
    fclose(f);
    return std::make_pair(true,retval);
}

/** Write contents of std::string to a file.
 * @return true on success.
 */
static bool WriteBinaryFile(const fs::path &filename, const std::string &data)
{
    FILE *f = fsbridge::fopen(filename, "wb");
    if (f == nullptr)
        return false;
    if (fwrite(data.data(), 1, data.size(), f) != data.size()) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

/**
 * Write contents of std::string to a file that only the owner may read.
 *
 * Used for the hidden service private key, which must not be world readable.
 * @return true on success.
 */
static bool WritePrivateKeyFile(const fs::path &filename, const std::string &data)
{
    if (!WriteBinaryFile(filename, data)) {
        return false;
    }
    std::error_code ec;
    fs::permissions(filename, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) {
        LogPrintf("tor: Error setting permissions on the private key file %s\n", fs::PathToString(filename));
        return false;
    }
    return true;
}

/****** Defi specific TorController implementation ********/

CService TorServiceIdToService(const std::string& service_id, int port)
{
    if (service_id.empty()) {
        return CService();
    }
    CService service = LookupNumeric((service_id + ".onion").c_str(), port);
    if (!service.IsValid() || !service.IsTorV3()) {
        // The service id is not a valid Tor v3 onion address.
        return CService();
    }
    return service;
}

/** Controller that connects to Tor control socket, authenticate, then create
 * and maintain an ephemeral hidden service.
 */
class TorController
{
public:
    TorController(const std::string& target);
    ~TorController();

    /** Get name of file to store private key in */
    fs::path GetPrivateKeyFile();

    /** Reconnect, after getting disconnected */
    void Reconnect();

    /** Run the control loop until the controller is interrupted. */
    void Run();

    /** Interrupt the control loop. */
    void Interrupt();
private:
    std::string target;
    TorControlConnection conn;
    std::string private_key;
    std::string service_id;
    bool reconnect;
    float reconnect_timeout;
    CService service;
    /** Cookie for SAFECOOKIE auth */
    std::vector<uint8_t> cookie;
    /** ClientNonce for SAFECOOKIE auth */
    std::vector<uint8_t> clientNonce;
    /** Interrupt object that is used to stop the control loop */
    CThreadInterrupt interrupt;

    /** Callback for ADD_ONION result */
    void add_onion_cb(TorControlConnection& conn, const TorControlReply& reply);
    /** Callback for AUTHENTICATE result */
    void auth_cb(TorControlConnection& conn, const TorControlReply& reply);
    /** Callback for AUTHCHALLENGE result */
    void authchallenge_cb(TorControlConnection& conn, const TorControlReply& reply);
    /** Callback for PROTOCOLINFO result */
    void protocolinfo_cb(TorControlConnection& conn, const TorControlReply& reply);
    /** Callback after successful connection */
    void connected_cb(TorControlConnection& conn);
    /** Callback after connection lost or failed connection attempt */
    void disconnected_cb(TorControlConnection& conn);
};

TorController::TorController(const std::string& _target):
    target(_target), reconnect(true),
    reconnect_timeout(RECONNECT_TIMEOUT_START)
{
    // Read service private key if cached
    std::pair<bool,std::string> pkf = ReadBinaryFile(GetPrivateKeyFile());
    if (pkf.first) {
        LogPrint(BCLog::TOR, "tor: Reading cached private key from %s\n", fs::PathToString(GetPrivateKeyFile()));
        private_key = pkf.second;
    }
}

TorController::~TorController()
{
    if (service.IsValid()) {
        RemoveLocal(service);
    }
}

void TorController::add_onion_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: ADD_ONION successful\n");
        for (const std::string &s : reply.lines) {
            std::map<std::string,std::string> m = ParseTorReplyMapping(s);
            std::map<std::string,std::string>::iterator i;
            if ((i = m.find("ServiceID")) != m.end())
                service_id = i->second;
            if ((i = m.find("PrivateKey")) != m.end())
                private_key = i->second;
        }
        if (service_id.empty()) {
            LogPrintf("tor: Error parsing ADD_ONION parameters:\n");
            for (const std::string &s : reply.lines) {
                LogPrintf("    %s\n", SanitizeString(s));
            }
            return;
        }
        // The service id of a Tor v3 hidden service is the 56 character base32
        // encoded ed25519 public key; the onion address is derived from it.
        service = TorServiceIdToService(service_id, Params().GetDefaultPort());
        if (!service.IsValid()) {
            LogPrintf("tor: Error parsing ADD_ONION parameters: Tor returned a service id that is not a valid Tor v3 onion address\n");
            service = CService();
            return;
        }
        LogPrintf("tor: Got service ID %s, advertising service %s\n", service_id, service.ToString());
        if (WritePrivateKeyFile(GetPrivateKeyFile(), private_key)) {
            LogPrint(BCLog::TOR, "tor: Cached service private key to %s\n", fs::PathToString(GetPrivateKeyFile()));
        } else {
            LogPrintf("tor: Error writing service private key to %s\n", fs::PathToString(GetPrivateKeyFile()));
        }
        AddLocal(service, LOCAL_MANUAL);
        // ... onion requested - keep connection open
    } else if (reply.code == 510) { // 510 Unrecognized command
        LogPrintf("tor: Add onion failed with unrecognized command (You probably need to upgrade Tor)\n");
    } else if (reply.code == 552) { // 552 Unrecognized key type
        LogPrintf("tor: Add onion failed: Tor rejected the requested hidden service key type. Tor v3 (ED25519-V3) services require Tor >= 0.3.5.\n");
    } else {
        LogPrintf("tor: Add onion failed; error code %d\n", reply.code);
    }
}

void TorController::auth_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: Authentication successful\n");

        // Now that we know Tor is running setup the proxy for onion addresses
        // if -onion isn't set to something else.
        if (gArgs.GetArg("-onion", "") == "") {
            CService resolved(LookupNumeric("127.0.0.1", 9050));
            proxyType addrOnion = proxyType(resolved, true);
            SetProxy(NET_ONION, addrOnion);
            SetReachable(NET_ONION, true);
        }

        // Finally - now create the service
        if (private_key.empty()) { // No private key, generate one
            private_key = TOR_ONION_KEY_TYPE; // Explicitly request a Tor v3 key type
        }
        // Request hidden service, redirect port.
        // Note that the 'virtual' port is always the default port to avoid decloaking nodes using other ports.
        _conn.Command(strprintf("ADD_ONION %s Port=%i,127.0.0.1:%i", private_key, Params().GetDefaultPort(), GetListenPort()),
            std::bind(&TorController::add_onion_cb, this, std::placeholders::_1, std::placeholders::_2));
    } else {
        LogPrintf("tor: Authentication failed\n");
    }
}

/** Compute Tor SAFECOOKIE response.
 *
 *    ServerHash is computed as:
 *      HMAC-SHA256("Tor safe cookie authentication server-to-controller hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *    (with the HMAC key as its first argument)
 *
 *    After a controller sends a successful AUTHCHALLENGE command, the
 *    next command sent on the connection must be an AUTHENTICATE command,
 *    and the only authentication string which that AUTHENTICATE command
 *    will accept is:
 *
 *      HMAC-SHA256("Tor safe cookie authentication controller-to-server hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *
 */
static std::vector<uint8_t> ComputeResponse(const std::string &key, const std::vector<uint8_t> &cookie,  const std::vector<uint8_t> &clientNonce, const std::vector<uint8_t> &serverNonce)
{
    CHMAC_SHA256 computeHash((const uint8_t*)key.data(), key.size());
    std::vector<uint8_t> computedHash(CHMAC_SHA256::OUTPUT_SIZE, 0);
    computeHash.Write(cookie.data(), cookie.size());
    computeHash.Write(clientNonce.data(), clientNonce.size());
    computeHash.Write(serverNonce.data(), serverNonce.size());
    computeHash.Finalize(computedHash.data());
    return computedHash;
}

void TorController::authchallenge_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: SAFECOOKIE authentication challenge successful\n");
        std::pair<std::string,std::string> l = SplitTorReplyLine(reply.lines[0]);
        if (l.first == "AUTHCHALLENGE") {
            std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
            if (m.empty()) {
                LogPrintf("tor: Error parsing AUTHCHALLENGE parameters: %s\n", SanitizeString(l.second));
                return;
            }
            std::vector<uint8_t> serverHash = ParseHex(m["SERVERHASH"]);
            std::vector<uint8_t> serverNonce = ParseHex(m["SERVERNONCE"]);
            LogPrint(BCLog::TOR, "tor: AUTHCHALLENGE ServerHash %s ServerNonce %s\n", HexStr(serverHash), HexStr(serverNonce));
            if (serverNonce.size() != 32) {
                LogPrintf("tor: ServerNonce is not 32 bytes, as required by spec\n");
                return;
            }

            std::vector<uint8_t> computedServerHash = ComputeResponse(TOR_SAFE_SERVERKEY, cookie, clientNonce, serverNonce);
            if (computedServerHash != serverHash) {
                LogPrintf("tor: ServerHash %s does not match expected ServerHash %s\n", HexStr(serverHash), HexStr(computedServerHash));
                return;
            }

            std::vector<uint8_t> computedClientHash = ComputeResponse(TOR_SAFE_CLIENTKEY, cookie, clientNonce, serverNonce);
            _conn.Command("AUTHENTICATE " + HexStr(computedClientHash), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            LogPrintf("tor: Invalid reply to AUTHCHALLENGE\n");
        }
    } else {
        LogPrintf("tor: SAFECOOKIE authentication challenge failed\n");
    }
}

void TorController::protocolinfo_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        std::set<std::string> methods;
        std::string cookiefile;
        /*
         * 250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE="/home/x/.tor/control_auth_cookie"
         * 250-AUTH METHODS=NULL
         * 250-AUTH METHODS=HASHEDPASSWORD
         */
        for (const std::string &s : reply.lines) {
            std::pair<std::string,std::string> l = SplitTorReplyLine(s);
            if (l.first == "AUTH") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("METHODS")) != m.end())
                    boost::split(methods, i->second, boost::is_any_of(","));
                if ((i = m.find("COOKIEFILE")) != m.end())
                    cookiefile = i->second;
            } else if (l.first == "VERSION") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("Tor")) != m.end()) {
                    LogPrint(BCLog::TOR, "tor: Connected to Tor version %s\n", i->second);
                }
            }
        }
        for (const std::string &s : methods) {
            LogPrint(BCLog::TOR, "tor: Supported authentication method: %s\n", s);
        }
        // Prefer NULL, otherwise SAFECOOKIE. If a password is provided, use HASHEDPASSWORD
        /* Authentication:
         *   cookie:   hex-encoded ~/.tor/control_auth_cookie
         *   password: "password"
         */
        std::string torpassword = gArgs.GetArg("-torpassword", "");
        if (!torpassword.empty()) {
            if (methods.count("HASHEDPASSWORD")) {
                LogPrint(BCLog::TOR, "tor: Using HASHEDPASSWORD authentication\n");
                boost::replace_all(torpassword, "\"", "\\\"");
                _conn.Command("AUTHENTICATE \"" + torpassword + "\"", std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                LogPrintf("tor: Password provided with -torpassword, but HASHEDPASSWORD authentication is not available\n");
            }
        } else if (methods.count("NULL")) {
            LogPrint(BCLog::TOR, "tor: Using NULL authentication\n");
            _conn.Command("AUTHENTICATE", std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else if (methods.count("SAFECOOKIE")) {
            // Cookie: hexdump -e '32/1 "%02x""\n"'  ~/.tor/control_auth_cookie
            LogPrint(BCLog::TOR, "tor: Using SAFECOOKIE authentication, reading cookie authentication from %s\n", cookiefile);
            std::pair<bool,std::string> status_cookie = ReadBinaryFile(fs::PathFromString(cookiefile), TOR_COOKIE_SIZE);
            if (status_cookie.first && status_cookie.second.size() == TOR_COOKIE_SIZE) {
                // _conn.Command("AUTHENTICATE " + HexStr(status_cookie.second), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
                cookie = std::vector<uint8_t>(status_cookie.second.begin(), status_cookie.second.end());
                clientNonce = std::vector<uint8_t>(TOR_NONCE_SIZE, 0);
                GetRandBytes(clientNonce.data(), TOR_NONCE_SIZE);
                _conn.Command("AUTHCHALLENGE SAFECOOKIE " + HexStr(clientNonce), std::bind(&TorController::authchallenge_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                if (status_cookie.first) {
                    LogPrintf("tor: Authentication cookie %s is not exactly %i bytes, as is required by the spec\n", cookiefile, TOR_COOKIE_SIZE);
                } else {
                    LogPrintf("tor: Authentication cookie %s could not be opened (check permissions)\n", cookiefile);
                }
            }
        } else if (methods.count("HASHEDPASSWORD")) {
            LogPrintf("tor: The only supported authentication mechanism left is password, but no password provided with -torpassword\n");
        } else {
            LogPrintf("tor: No supported authentication method\n");
        }
    } else {
        LogPrintf("tor: Requesting protocol info failed\n");
    }
}

void TorController::connected_cb(TorControlConnection& _conn)
{
    reconnect_timeout = RECONNECT_TIMEOUT_START;
    // First send a PROTOCOLINFO command to figure out what authentication is expected
    if (!_conn.Command("PROTOCOLINFO 1", std::bind(&TorController::protocolinfo_cb, this, std::placeholders::_1, std::placeholders::_2)))
        LogPrintf("tor: Error sending initial protocolinfo command\n");
}

void TorController::disconnected_cb(TorControlConnection& _conn)
{
    // Stop advertising service when disconnected
    if (service.IsValid())
        RemoveLocal(service);
    service = CService();
    if (!reconnect)
        return;

    LogPrint(BCLog::TOR, "tor: Not connected to Tor control port %s, trying to reconnect\n", target);
}

void TorController::Interrupt()
{
    reconnect = false;
    interrupt();
}

void TorController::Run()
{
    LogPrint(BCLog::TOR, "tor: Entering Tor control thread\n");

    while (!interrupt) {
        if (!conn.Connect(target, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
                          std::bind(&TorController::disconnected_cb, this, std::placeholders::_1))) {
            LogPrintf("tor: Initiating connection to Tor control port %s failed\n", target);
        } else {
            // Process the replies of the control connection until it breaks or
            // the controller is interrupted.
            while (!interrupt) {
                if (!conn.WaitForData(std::chrono::seconds(1))) {
                    if (!conn.IsConnected()) {
                        LogPrint(BCLog::TOR, "tor: Lost connection to Tor control port\n");
                        break;
                    }
                    continue;
                }
                if (!conn.ReceiveAndProcess()) break;
            }
        }
        conn.Disconnect();

        if (!reconnect) break;

        // Wait before the next connection attempt. Use exponential backoff.
        const auto timeout = std::chrono::milliseconds(static_cast<int64_t>(reconnect_timeout * 1000.0));
        reconnect_timeout *= RECONNECT_TIMEOUT_EXP;
        if (interrupt.sleep_for(timeout)) break;
    }

    LogPrint(BCLog::TOR, "tor: Exited Tor control thread\n");
}

void TorController::Reconnect()
{
    /* Try to reconnect and reestablish if we get booted - for example, Tor
     * may be restarting.
     */
    if (!conn.Connect(target, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogPrintf("tor: Re-initiating connection to Tor control port %s failed\n", target);
    }
}

fs::path TorController::GetPrivateKeyFile()
{
    return GetDataDir() / "onion_v3_private_key";
}

/****** Thread ********/
static TorController* g_tor_controller{nullptr};
static std::thread torControlThread;

static void TorControlThread()
{
    TorController ctrl(gArgs.GetArg("-torcontrol", DEFAULT_TOR_CONTROL));
    g_tor_controller = &ctrl;

    ctrl.Run();

    g_tor_controller = nullptr;
}

void StartTorControl()
{
    assert(!g_tor_controller);
    torControlThread = std::thread(std::bind(&TraceThread<void (*)()>, "torcontrol", &TorControlThread));
}

void InterruptTorControl()
{
    if (g_tor_controller) {
        LogPrintf("tor: Thread interrupt\n");
        g_tor_controller->Interrupt();
    }
}

void StopTorControl()
{
    if (torControlThread.joinable()) {
        torControlThread.join();
    }
}
