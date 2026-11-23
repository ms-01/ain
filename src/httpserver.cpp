// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>

#include <chainparamsbase.h>
#include <compat.h>
#include <netbase.h>
#include <rpc/protocol.h>
#include <serialize.h>
#include <shutdown.h>
#include <sync.h>
#include <tinyformat.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/threadpool.h>
#include <util/sock.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32
#include <netdb.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/** Maximum size of the http request (request line + headers) */
static const size_t MAX_HEADERS_SIZE = 8192;

/** Maximum size of an http request body */
static const size_t MAX_BODY_SIZE = MAX_DESER_SIZE;

/** Time the I/O thread waits for socket activity in one iteration */
static const auto SELECT_TIMEOUT = std::chrono::milliseconds{50};

/** Maximum number of bytes read from a client socket in one go */
static const size_t RECV_CHUNK_SIZE = 8192;

namespace {

/** Lowercase an ASCII character (locale independent). */
char ToLowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** Case-insensitive comparison of two header names. */
bool HeaderNameEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

/** Find a header by (case-insensitive) name. */
const std::string* FindHeader(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name)
{
    for (const auto& header : headers) {
        if (HeaderNameEquals(header.first, name)) return &header.second;
    }
    return nullptr;
}

/** Check whether a comma separated header value contains the given token. */
bool HeaderHasToken(const std::string& value, const std::string& token)
{
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t end = value.find(',', pos);
        if (end == std::string::npos) end = value.size();
        std::string item = value.substr(pos, end - pos);
        const size_t start = item.find_first_not_of(" \t");
        const size_t stop = item.find_last_not_of(" \t");
        if (start != std::string::npos && item.substr(start, stop - start + 1) == token) {
            return true;
        }
        if (end == value.size()) break;
        pos = end + 1;
    }
    return false;
}

/** The reason phrase for a HTTP status code. */
std::string StatusReason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

/**
 * Build a complete HTTP response.
 *
 * @param[in] status The HTTP status code.
 * @param[in] body The response body.
 * @param[in] keep_alive Whether the connection is kept alive after the response.
 * @param[in] headers Additional response headers (may be empty).
 * @param[in] head_only If true, only the headers are sent (HEAD request).
 */
std::string BuildHTTPResponse(int status,
                               const std::string& body,
                               bool keep_alive,
                               const std::vector<std::pair<std::string, std::string>>& headers = {},
                               bool head_only = false)
{
    std::string response;
    response.reserve(body.size() + 256);
    response += "HTTP/1.1 " + std::to_string(status) + " " + StatusReason(status) + "\r\n";
    for (const auto& header : headers) {
        response += header.first + ": " + header.second + "\r\n";
    }
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += std::string("Connection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n";
    response += "\r\n";
    if (!head_only) response += body;
    return response;
}

/** HTTP request method as string - use for logging only */
std::string RequestMethodString(HTTPRequest::RequestMethod m)
{
    switch (m) {
    case HTTPRequest::GET:
        return "GET";
    case HTTPRequest::POST:
        return "POST";
    case HTTPRequest::HEAD:
        return "HEAD";
    case HTTPRequest::PUT:
        return "PUT";
    case HTTPRequest::OPTIONS:
        return "OPTIONS";
    default:
        return "unknown";
    }
}

} // namespace

/*********************************************************************
 * HTTPRequest
 *********************************************************************/

HTTPRequest::HTTPRequest(CService peer,
                         RequestMethod method,
                         std::string uri,
                         std::vector<std::pair<std::string, std::string>> headers,
                         std::string body,
                         bool keep_alive,
                         ReplySender reply_sender)
    : m_peer(std::move(peer)),
      m_method(method),
      m_uri(std::move(uri)),
      m_request_headers(std::move(headers)),
      m_body(std::move(body)),
      m_keep_alive(keep_alive),
      m_reply_sender(std::move(reply_sender))
{
}

HTTPRequest::~HTTPRequest()
{
    if (!m_reply_sent) {
        // Keep track of whether reply was sent to avoid request leaks
        LogPrintf("%s: Unhandled request\n", __func__);
        WriteReply(HTTP_INTERNAL_SERVER_ERROR, "Unhandled request");
    }
}

std::pair<bool, std::string> HTTPRequest::GetHeader(const std::string& hdr) const
{
    const std::string* value = FindHeader(m_request_headers, hdr);
    if (value) return std::make_pair(true, *value);
    return std::make_pair(false, std::string());
}

std::string HTTPRequest::ReadBody()
{
    if (m_body_consumed) return "";
    m_body_consumed = true;
    return m_body;
}

void HTTPRequest::WriteHeader(const std::string& hdr, const std::string& value)
{
    m_response_headers.emplace_back(hdr, value);
}

void HTTPRequest::WriteReply(int nStatus, const std::string& strReply)
{
    assert(!m_reply_sent);
    if (m_reply_sender) {
        m_reply_sender(BuildHTTPResponse(nStatus, strReply, m_keep_alive, m_response_headers, m_method == HEAD));
    }
    m_reply_sent = true;
}

/*********************************************************************
 * Request parsing
 *********************************************************************/

HTTPParseStatus ParseHTTPRequest(std::string_view buffer, HTTPRequestData& out)
{
    // Find the end of the header section. Both CRLF and LF line endings are
    // accepted, just like the previous libevent based implementation did.
    size_t header_end = buffer.find("\r\n\r\n");
    size_t separator_len = 4;
    if (header_end == std::string_view::npos) {
        header_end = buffer.find("\n\n");
        separator_len = 2;
    }
    if (header_end == std::string_view::npos) {
        if (buffer.size() > MAX_HEADERS_SIZE) {
            out.error = "HTTP request headers too large";
            return HTTPParseStatus::ERROR;
        }
        return HTTPParseStatus::INCOMPLETE;
    }
    if (header_end > MAX_HEADERS_SIZE) {
        out.error = "HTTP request headers too large";
        return HTTPParseStatus::ERROR;
    }

    const std::string_view head = buffer.substr(0, header_end);

    // Split the head into lines
    std::vector<std::string_view> lines;
    size_t line_start = 0;
    while (line_start <= head.size()) {
        const size_t line_end = head.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            if (line_start < head.size()) lines.push_back(head.substr(line_start));
            break;
        }
        std::string_view line = head.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        line_start = line_end + 1;
    }
    if (lines.empty()) {
        out.error = "Empty HTTP request";
        return HTTPParseStatus::ERROR;
    }

    // Parse the request line: METHOD SP TARGET SP VERSION
    const std::string_view request_line = lines.front();
    const size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        out.error = "Malformed HTTP request line";
        return HTTPParseStatus::ERROR;
    }
    const size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        out.error = "Malformed HTTP request line";
        return HTTPParseStatus::ERROR;
    }
    const std::string_view method = request_line.substr(0, first_space);
    const std::string_view target = request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);

    if (method.empty() || target.empty()) {
        out.error = "Malformed HTTP request line";
        return HTTPParseStatus::ERROR;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        out.error = "Unsupported HTTP version";
        return HTTPParseStatus::ERROR;
    }
    if (method == "GET") {
        out.method = HTTPRequest::GET;
    } else if (method == "POST") {
        out.method = HTTPRequest::POST;
    } else if (method == "HEAD") {
        out.method = HTTPRequest::HEAD;
    } else if (method == "PUT") {
        out.method = HTTPRequest::PUT;
    } else if (method == "OPTIONS") {
        out.method = HTTPRequest::OPTIONS;
    } else {
        // Unknown (or not allowed) methods are rejected by the server.
        out.method = HTTPRequest::UNKNOWN;
    }
    out.uri = std::string(target);

    // Parse the headers
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            out.error = "Malformed HTTP header";
            return HTTPParseStatus::ERROR;
        }
        const std::string_view name = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        // Trim leading whitespace of the value
        const size_t value_start = value.find_first_not_of(" \t");
        if (value_start == std::string_view::npos) {
            value = std::string_view{};
        } else {
            value = value.substr(value_start);
        }
        out.headers.emplace_back(std::string(name), std::string(value));
    }

    // Determine whether the connection is kept alive
    const bool http_1_1 = version == "HTTP/1.1";
    const std::string* connection = FindHeader(out.headers, "Connection");
    const bool connection_close = connection && HeaderHasToken(*connection, "close");
    const bool connection_keep_alive = connection && HeaderHasToken(*connection, "keep-alive");
    if (connection_close) {
        out.keep_alive = false;
    } else if (connection_keep_alive) {
        out.keep_alive = true;
    } else {
        out.keep_alive = http_1_1;
    }

    const size_t body_start = header_end + separator_len;

    // A chunked body is not supported by the RPC interface, but it is detected
    // nevertheless to be able to reject such requests gracefully.
    const std::string* transfer_encoding = FindHeader(out.headers, "Transfer-Encoding");
    if (transfer_encoding && HeaderHasToken(*transfer_encoding, "chunked")) {
        out.error = "Chunked transfer encoding is not supported";
        return HTTPParseStatus::ERROR;
    }

    const std::string* content_length = FindHeader(out.headers, "Content-Length");
    size_t body_length = 0;
    if (content_length) {
        size_t parsed_length{0};
        const auto& value = *content_length;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed_length);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
            out.error = "Invalid Content-Length header";
            return HTTPParseStatus::ERROR;
        }
        body_length = parsed_length;
        if (body_length > MAX_BODY_SIZE) {
            out.error = "HTTP request body too large";
            return HTTPParseStatus::ERROR;
        }
    }

    if (buffer.size() < body_start + body_length) {
        // The body is not complete yet
        return HTTPParseStatus::INCOMPLETE;
    }

    out.body = std::string(buffer.substr(body_start, body_length));
    out.consumed = body_start + body_length;
    return HTTPParseStatus::COMPLETE;
}

/*********************************************************************
 * HTTP server
 *********************************************************************/

namespace {

/** A connected HTTP client. */
class HTTPRemoteClient
{
public:
    HTTPRemoteClient(CService peer, std::shared_ptr<Sock> sock)
        : m_peer(std::move(peer)), m_sock(std::move(sock)), m_last_activity(std::chrono::steady_clock::now())
    {
    }

    const CService& GetPeer() const { return m_peer; }
    const std::shared_ptr<Sock>& GetSock() const { return m_sock; }

    /** Queue a response for sending. */
    void QueueResponse(const std::string& response)
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_send_buffer += response;
        m_response_pending = true;
    }

    /** Whether there is data that still has to be sent to the client. */
    bool HasPendingSend() const
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        return !m_send_buffer.empty();
    }

    /** Move the pending data out of the send buffer. */
    std::string TakePendingSend()
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        std::string data{std::move(m_send_buffer)};
        m_send_buffer.clear();
        return data;
    }

    /** Put unsent data back into the send buffer. */
    void RestorePendingSend(const std::string& data)
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_send_buffer.insert(0, data);
    }

    /** Whether the complete response was handed over to the socket. */
    bool ResponseCompleted() const
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        return m_response_pending && m_send_buffer.empty();
    }

    void MarkResponseCompleted()
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_response_pending = false;
    }

    CService m_peer;
    std::shared_ptr<Sock> m_sock;
    /** Data received from the client but not yet parsed. */
    std::string m_recv_buffer;
    /** Data to be sent to the client. */
    std::string m_send_buffer;
    mutable std::mutex m_send_mutex;
    bool m_response_pending{false};
    /** A request is currently being processed (or its response is sent). */
    bool m_req_busy{false};
    /** Keep the connection open after the response was sent. */
    bool m_keep_alive{true};
    /** The connection has to be closed. */
    bool m_disconnect{false};
    /** Timestamp of the last read or write activity. */
    std::chrono::steady_clock::time_point m_last_activity;
};

/** Handler registration. */
struct HTTPPathHandler {
    HTTPPathHandler(std::string _prefix, bool _exactMatch, HTTPRequestHandler _handler)
        : prefix(_prefix), exactMatch(_exactMatch), handler(_handler)
    {
    }
    std::string prefix;
    bool exactMatch;
    HTTPRequestHandler handler;
};

std::vector<HTTPPathHandler> pathHandlers;
std::mutex g_pathHandlers_mutex;

/** The HTTP server. */
class HTTPServer
{
public:
    HTTPServer() = default;

    bool Init();
    void Start();
    void Interrupt();
    void Stop();

private:
    bool InitHTTPAllowList();
    bool BindAddresses();
    void ThreadSocketHandler();
    void AcceptConnections();
    void ReadFromClient(const std::shared_ptr<HTTPRemoteClient>& client);
    void WriteToClient(const std::shared_ptr<HTTPRemoteClient>& client);
    bool ProcessClientBuffer(const std::shared_ptr<HTTPRemoteClient>& client);
    void DispatchRequest(const std::shared_ptr<HTTPRemoteClient>& client, HTTPRequestData& data);
    void SendSimpleResponse(const std::shared_ptr<HTTPRemoteClient>& client, int status, const std::string& body);
    void FlushPendingResponses();
    bool ClientAllowed(const CNetAddr& netaddr) const;
    void RemoveDisconnectedClients();

    std::vector<std::shared_ptr<Sock>> m_listen;
    std::vector<std::shared_ptr<HTTPRemoteClient>> m_connected;
    std::thread m_io_thread;
    //! Set when the server was interrupted: no new requests are accepted.
    std::atomic<bool> m_interrupted{false};
    //! Set when the server has to stop: the I/O thread exits.
    std::atomic<bool> m_exit{false};
    //! Set when requests have to be rejected (shutdown).
    std::atomic<bool> m_reject_requests{false};
    std::unique_ptr<ThreadPool> m_threadpool;
    std::vector<CSubNet> m_allow_subnets;
    std::chrono::seconds m_rpcservertimeout{DEFAULT_HTTP_SERVER_TIMEOUT};
    size_t m_max_queue_depth{DEFAULT_HTTP_WORKQUEUE};
};

bool HTTPServer::ClientAllowed(const CNetAddr& netaddr) const
{
    if (!netaddr.IsValid()) return false;
    for (const CSubNet& subnet : m_allow_subnets) {
        if (subnet.Match(netaddr)) return true;
    }
    return false;
}

bool HTTPServer::InitHTTPAllowList()
{
    m_allow_subnets.clear();
    CNetAddr localv4;
    CNetAddr localv6;
    LookupHost("127.0.0.1", localv4, false);
    LookupHost("::1", localv6, false);
    m_allow_subnets.push_back(CSubNet(localv4, 8));  // always allow IPv4 local subnet
    m_allow_subnets.push_back(CSubNet(localv6));     // always allow IPv6 localhost
    for (const std::string& strAllow : gArgs.GetArgs("-rpcallowip")) {
        CSubNet subnet;
        LookupSubNet(strAllow.c_str(), subnet);
        if (!subnet.IsValid()) {
            uiInterface.ThreadSafeMessageBox(
                strprintf("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24).", strAllow),
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
        m_allow_subnets.push_back(subnet);
    }
    std::string strAllowed;
    for (const CSubNet& subnet : m_allow_subnets) strAllowed += subnet.ToString() + " ";
    LogPrint(BCLog::HTTP, "Allowing HTTP connections from: %s\n", strAllowed);
    return true;
}

/** Create a listening socket for the given address and port. */
std::shared_ptr<Sock> CreateListenSocket(const std::string& address, int port, std::string& error)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    const std::string service = std::to_string(port);
    struct addrinfo* result{nullptr};
    const int rc = getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints, &result);
    if (rc != 0) {
        error = gai_strerror(rc);
        return nullptr;
    }

    std::shared_ptr<Sock> sock;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCKET_ERROR) continue;

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifdef IPV6_V6ONLY
        if (ai->ai_family == AF_INET6) {
            // Allow IPv4 connections on IPv6 sockets (dual stack), like the
            // previous libevent based implementation did.
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
        }
#endif
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == SOCKET_ERROR) {
#ifdef WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            continue;
        }
        if (listen(fd, SOMAXCONN) == SOCKET_ERROR) {
#ifdef WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            continue;
        }
        sock = std::make_shared<Sock>(fd);
        break;
    }
    freeaddrinfo(result);

    if (!sock) {
        error = strprintf("could not bind to %s:%d", address.empty() ? "*" : address.c_str(), port);
    }
    return sock;
}

bool HTTPServer::BindAddresses()
{
    int http_port = gArgs.GetArg("-rpcport", BaseParams().RPCPort());
    std::vector<std::pair<std::string, int>> endpoints;

    if (const auto autoPort = gArgs.GetArg("-ports", ""); autoPort == "auto") {
        http_port = 0;
    }

    // Determine what addresses to bind to
    if (!(gArgs.IsArgSet("-rpcallowip") && gArgs.IsArgSet("-rpcbind"))) { // Default to loopback if not allowing external IPs
        endpoints.push_back(std::make_pair("::1", http_port));
        endpoints.push_back(std::make_pair("127.0.0.1", http_port));
        if (gArgs.IsArgSet("-rpcallowip")) {
            LogPrintf("WARNING: option -rpcallowip was specified without -rpcbind; this doesn't usually make sense\n");
        }
        if (gArgs.IsArgSet("-rpcbind")) {
            LogPrintf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
        }
    } else if (gArgs.IsArgSet("-rpcbind")) { // Specific bind address
        for (const std::string& strRPCBind : gArgs.GetArgs("-rpcbind")) {
            int port = http_port;
            std::string host;
            SplitHostPort(strRPCBind, port, host);
            endpoints.push_back(std::make_pair(host, port));
        }
    }

    int autoHTTPPort{0};

    for (auto& [address, port] : endpoints) {
        if (!http_port && autoHTTPPort) {
            port = autoHTTPPort;
        }
        std::string error;
        std::shared_ptr<Sock> sock = CreateListenSocket(address, port, error);
        if (sock) {
            CNetAddr addr;
            if (address.empty() || (LookupHost(address.c_str(), addr, false) && addr.IsBindAny())) {
                LogPrintf("WARNING: the RPC server is not safe to expose to untrusted networks such as the public internet\n");
            }

            // Retrieve the actual bound address and port
            autoHTTPPort = GetAndPrintActualPort(sock->GetSocket(), autoHTTPPort == 0, AutoPort::RPC, address);

            if (!sock->SetNonBlocking()) {
                LogPrintf("Failed to set the RPC listening socket to non-blocking mode\n");
                continue;
            }
            m_listen.push_back(sock);
        } else {
            LogPrintf("Binding RPC on address %s port %i failed: %s\n", address, port, error);
        }
    }
    return !m_listen.empty();
}

bool HTTPServer::Init()
{
    if (!InitHTTPAllowList()) return false;

    m_rpcservertimeout = std::chrono::seconds{gArgs.GetArg("-rpcservertimeout", DEFAULT_HTTP_SERVER_TIMEOUT)};
    m_max_queue_depth = std::max((long)gArgs.GetArg("-rpcworkqueue", DEFAULT_HTTP_WORKQUEUE), 1L);

    if (!BindAddresses()) {
        LogPrintf("Unable to bind any endpoint for RPC server\n");
        return false;
    }

    LogPrint(BCLog::HTTP, "Initialized HTTP server\n");
    LogPrintf("HTTP: creating work queue of depth %d\n", m_max_queue_depth);
    return true;
}

void HTTPServer::Start()
{
    const int rpcThreads = std::max((long)gArgs.GetArg("-rpcthreads", DEFAULT_HTTP_THREADS), 1L);
    LogPrintf("HTTP: starting %d worker threads\n", rpcThreads);
    m_threadpool = std::make_unique<ThreadPool>("http", m_max_queue_depth);
    m_threadpool->Start(rpcThreads);
    m_io_thread = std::thread(&HTTPServer::ThreadSocketHandler, this);
}

void HTTPServer::Interrupt()
{
    // No new requests are accepted, but the responses of the requests that are
    // still in flight are sent to the clients.
    m_reject_requests = true;
    m_interrupted = true;
    if (m_threadpool) m_threadpool->Interrupt();
}

void HTTPServer::Stop()
{
    m_reject_requests = true;
    m_interrupted = true;
    // Wait for the requests that are still in flight, so that their responses
    // can be sent to the clients.
    if (m_threadpool) {
        m_threadpool->Stop();
        m_threadpool.reset();
    }
    // Stop the I/O thread; it sends the pending responses before it exits.
    m_exit = true;
    if (m_io_thread.joinable()) {
        m_io_thread.join();
    }
    m_connected.clear();
    m_listen.clear();
}

void HTTPServer::AcceptConnections()
{
    for (auto& listen : m_listen) {
        while (true) {
            struct sockaddr_storage sockaddr;
            socklen_t len = sizeof(sockaddr);
            std::unique_ptr<Sock> sock = listen->Accept(reinterpret_cast<struct sockaddr*>(&sockaddr), &len);
            if (!sock) break;

            if (!sock->SetNonBlocking()) {
                LogPrintf("Failed to set the accepted socket to non-blocking mode\n");
                continue;
            }
            int nodelay = 1;
            if (sock->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
                LogPrintf("Failed to set TCP_NODELAY on the accepted socket\n");
            }

            CService peer;
            if (sockaddr.ss_family == AF_INET) {
                peer = CService(*reinterpret_cast<struct sockaddr_in*>(&sockaddr));
            } else if (sockaddr.ss_family == AF_INET6) {
                peer = CService(*reinterpret_cast<struct sockaddr_in6*>(&sockaddr));
            }
            m_connected.push_back(std::make_shared<HTTPRemoteClient>(peer, std::move(sock)));
        }
    }
}

void HTTPServer::ReadFromClient(const std::shared_ptr<HTTPRemoteClient>& client)
{
    char buf[RECV_CHUNK_SIZE];
    while (true) {
        const ssize_t n = client->GetSock()->Recv(buf, sizeof(buf), 0);
        if (n > 0) {
            client->m_recv_buffer.append(buf, n);
            client->m_last_activity = std::chrono::steady_clock::now();
            if (!ProcessClientBuffer(client)) return;
            continue;
        }
        if (n == 0) {
            // The peer closed the connection
            client->m_disconnect = true;
            return;
        }
        const int err = WSAGetLastError();
        if (IOErrorIsPermanent(err)) {
            client->m_disconnect = true;
        }
        return;
    }
}

void HTTPServer::WriteToClient(const std::shared_ptr<HTTPRemoteClient>& client)
{
    const std::string data = client->TakePendingSend();
    if (data.empty()) return;

    const ssize_t n = client->GetSock()->Send(data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0) {
        const int err = WSAGetLastError();
        if (IOErrorIsPermanent(err)) {
            client->m_disconnect = true;
        } else {
            client->RestorePendingSend(data);
        }
        return;
    }
    if (static_cast<size_t>(n) < data.size()) {
        client->RestorePendingSend(data.substr(n));
    }
    client->m_last_activity = std::chrono::steady_clock::now();
}

bool HTTPServer::ProcessClientBuffer(const std::shared_ptr<HTTPRemoteClient>& client)
{
    HTTPRequestData data;
    const HTTPParseStatus status = ParseHTTPRequest(client->m_recv_buffer, data);
    if (status == HTTPParseStatus::INCOMPLETE) return true;
    if (status == HTTPParseStatus::ERROR) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: %s\n", client->GetPeer().ToString(), data.error);
        // The connection is closed after the error response was sent
        client->m_keep_alive = false;
        SendSimpleResponse(client, HTTP_BAD_REQUEST, "Bad Request\n");
        return false;
    }

    client->m_recv_buffer.erase(0, data.consumed);
    client->m_req_busy = true;
    client->m_keep_alive = data.keep_alive;

    DispatchRequest(client, data);
    return !client->m_req_busy;
}

void HTTPServer::SendSimpleResponse(const std::shared_ptr<HTTPRemoteClient>& client, int status, const std::string& body)
{
    if (!client) return;
    client->QueueResponse(BuildHTTPResponse(status, body, client->m_keep_alive));
}

void HTTPServer::DispatchRequest(const std::shared_ptr<HTTPRemoteClient>& client, HTTPRequestData& data)
{
    // Reject requests while shutting down
    if (m_reject_requests) {
        LogPrint(BCLog::HTTP, "Rejecting request while shutting down\n");
        client->m_keep_alive = false;
        SendSimpleResponse(client, HTTP_SERVICE_UNAVAILABLE, "Service Unavailable\n");
        return;
    }

    // Early address-based allow check
    if (!ClientAllowed(client->GetPeer())) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Client network is not allowed RPC access\n",
                 client->GetPeer().ToString());
        SendSimpleResponse(client, HTTP_FORBIDDEN, "");
        return;
    }

    // Early reject unknown HTTP methods
    if (data.method == HTTPRequest::UNKNOWN) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Unknown HTTP request method\n",
                 client->GetPeer().ToString());
        SendSimpleResponse(client, HTTP_BAD_METHOD, "Method Not Allowed\n");
        return;
    }

    LogPrint(BCLog::HTTP, "Received a %s request for %s from %s\n",
             RequestMethodString(data.method),
             SanitizeString(data.uri, SAFE_CHARS_URI).substr(0, 100),
             client->GetPeer().ToString());

    // Find registered handler for prefix
    std::string path;
    HTTPRequestHandler handler;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_pathHandlers_mutex);
        for (const auto& pathHandler : pathHandlers) {
            bool match = false;
            if (pathHandler.exactMatch) {
                match = (data.uri == pathHandler.prefix);
            } else {
                match = (data.uri.substr(0, pathHandler.prefix.size()) == pathHandler.prefix);
            }
            if (match) {
                path = data.uri.substr(pathHandler.prefix.size());
                handler = pathHandler.handler;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        SendSimpleResponse(client, HTTP_NOT_FOUND, "");
        return;
    }

    // Dispatch to a worker thread
    const bool keep_alive = data.keep_alive;
    const std::shared_ptr<HTTPRemoteClient> client_ptr = client;
    auto work = [client_ptr, data, path, handler, keep_alive]() {
        auto request = std::make_unique<HTTPRequest>(
            client_ptr->GetPeer(), data.method, data.uri, data.headers, data.body, keep_alive,
            [client_ptr](const std::string& response) {
                client_ptr->QueueResponse(response);
            });
        handler(request.get(), path);
    };

    if (!m_threadpool || !m_threadpool->Submit(work)) {
        LogPrintf("WARNING: request rejected because http work queue depth exceeded, it can be increased with the -rpcworkqueue= setting\n");
        SendSimpleResponse(client, HTTP_SERVICE_UNAVAILABLE, "Work queue depth exceeded");
    }
}

void HTTPServer::FlushPendingResponses()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    for (const auto& client : m_connected) {
        while (client->HasPendingSend() && std::chrono::steady_clock::now() < deadline) {
            WriteToClient(client);
            if (!client->HasPendingSend()) break;
            Sock::Event occurred{0};
            (void)client->GetSock()->Wait(std::chrono::milliseconds{50}, Sock::SendEvent, &occurred);
        }
    }
}

void HTTPServer::RemoveDisconnectedClients()
{
    m_connected.erase(std::remove_if(m_connected.begin(), m_connected.end(),
                                     [](const std::shared_ptr<HTTPRemoteClient>& client) { return client->m_disconnect; }),
                      m_connected.end());
}

void HTTPServer::ThreadSocketHandler()
{
    util::ThreadRename("http");
    LogPrint(BCLog::HTTP, "Entering http event loop\n");

    while (!m_exit) {
        // Build the set of sockets to wait for
        Sock::EventsPerSock events_per_sock;
        std::unordered_map<SOCKET, std::shared_ptr<HTTPRemoteClient>> clients_by_socket;
        for (const auto& listen : m_listen) {
            events_per_sock.emplace(listen, Sock::Events{Sock::RecvEvent});
        }
        for (const auto& client : m_connected) {
            if (client->m_disconnect) continue;
            Sock::Event requested{0};
            if (!client->m_req_busy) requested |= Sock::RecvEvent;
            {
                std::lock_guard<std::mutex> lock(client->m_send_mutex);
                if (!client->m_send_buffer.empty()) requested |= Sock::SendEvent;
            }
            if (requested == 0) continue;
            events_per_sock.emplace(client->GetSock(), Sock::Events{requested});
            clients_by_socket.emplace(client->GetSock()->GetSocket(), client);
        }

        if (events_per_sock.empty() || m_listen.empty()) {
            std::this_thread::sleep_for(SELECT_TIMEOUT);
            continue;
        }

        // Wait for socket activity. The wait is done in short intervals, so
        // that the shutdown is not delayed by more than SELECT_TIMEOUT.
        if (!m_listen.front()->WaitMany(SELECT_TIMEOUT, events_per_sock)) {
            LogPrint(BCLog::HTTP, "Waiting for socket activity failed\n");
        }

        // Handle new connections
        for (const auto& [sock, events] : events_per_sock) {
            if (clients_by_socket.find(sock->GetSocket()) != clients_by_socket.end()) continue;
            if ((events.occurred & Sock::RecvEvent) && !m_interrupted) {
                AcceptConnections();
                break;
            }
        }

        // Handle client activity
        for (auto& [sock, events] : events_per_sock) {
            auto it = clients_by_socket.find(sock->GetSocket());
            if (it == clients_by_socket.end()) continue;
            const std::shared_ptr<HTTPRemoteClient> client = it->second;

            if (events.occurred & Sock::ErrorEvent) {
                client->m_disconnect = true;
                continue;
            }
            if (events.occurred & Sock::RecvEvent) {
                ReadFromClient(client);
            }
            if (events.occurred & Sock::SendEvent) {
                WriteToClient(client);
            }
        }

        // Check whether a response was completely sent
        for (const auto& client : m_connected) {
            if (client->ResponseCompleted()) {
                client->MarkResponseCompleted();
                if (client->m_keep_alive) {
                    client->m_req_busy = false;
                } else {
                    client->m_disconnect = true;
                }
            }
        }

        // Enforce the idle timeout
        const auto now = std::chrono::steady_clock::now();
        for (const auto& client : m_connected) {
            if (now - client->m_last_activity > m_rpcservertimeout) {
                client->m_disconnect = true;
            }
        }

        RemoveDisconnectedClients();
    }

    // Send the responses that are still pending before the server is stopped.
    FlushPendingResponses();

    LogPrint(BCLog::HTTP, "Exited http event loop\n");
}

/** The global HTTP server instance. */
HTTPServer g_http_server;

} // namespace

/*********************************************************************
 * Public interface
 *********************************************************************/

bool InitHTTPServer()
{
    return g_http_server.Init();
}

void StartHTTPServer()
{
    LogPrint(BCLog::HTTP, "Starting HTTP server\n");
    g_http_server.Start();
}

void InterruptHTTPServer()
{
    LogPrint(BCLog::HTTP, "Interrupting HTTP server\n");
    g_http_server.Interrupt();
}

void StopHTTPServer()
{
    LogPrint(BCLog::HTTP, "Stopping HTTP server\n");
    g_http_server.Stop();
}

void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler)
{
    LogPrint(BCLog::HTTP, "Registering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
    std::lock_guard<std::mutex> lock(g_pathHandlers_mutex);
    pathHandlers.push_back(HTTPPathHandler(prefix, exactMatch, handler));
}

void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch)
{
    std::lock_guard<std::mutex> lock(g_pathHandlers_mutex);
    std::vector<HTTPPathHandler>::iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::iterator iend = pathHandlers.end();
    for (; i != iend; ++i) {
        if (i->prefix == prefix && i->exactMatch == exactMatch) break;
    }
    if (i != iend) {
        LogPrint(BCLog::HTTP, "Unregistering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
        pathHandlers.erase(i);
    }
}
