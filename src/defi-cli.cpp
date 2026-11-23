// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/defi-config.h>
#endif

#include <chainparamsbase.h>
#include <clientversion.h>
#include <fs.h>
#include <netbase.h>
#include <rpc/client.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <util/url.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <poll.h>
#include <stdio.h>
#include <tuple>
#include <utility>

#include <univalue.h>
#include <dfi/coinselect.h>

#ifndef WIN32
#include <netdb.h>
#endif

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;

static const char DEFAULT_RPCCONNECT[] = "127.0.0.1";
static const int DEFAULT_HTTP_CLIENT_TIMEOUT=900;
static const bool DEFAULT_NAMED=false;
static const int CONTINUE_EXECUTION=-1;
static const int DEFAULT_WAIT_CLIENT_TIMEOUT=0;
// Upper bound for -rpcwaittimeout, so that the monotonic deadline computation
// cannot overflow (one year in seconds).
static const int64_t MAX_WAIT_CLIENT_TIMEOUT=31536000;

static void SetupCliArgs()
{
    SetupHelpOptions(gArgs);

    const auto defaultBaseParams = CreateBaseChainParams(CBaseChainParams::MAIN);
    const auto testnetBaseParams = CreateBaseChainParams(CBaseChainParams::TESTNET);
    const auto changiBaseParams = CreateBaseChainParams(CBaseChainParams::CHANGI);
    const auto devnetBaseParams  = CreateBaseChainParams(CBaseChainParams::DEVNET);
    const auto regtestBaseParams = CreateBaseChainParams(CBaseChainParams::REGTEST);

    gArgs.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-conf=<file>", strprintf("Specify configuration file. Relative paths will be prefixed by datadir location. (default: %s)", DEFI_CONF_FILENAME), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-datadir=<dir>", "Specify data directory", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-addrinfo", "Get the number of addresses known to the node, per network and total. Prints an object of the form {\"addresses_known\": {\"ipv4\": n, \"ipv6\": n, \"onion\": n, \"total\": n}}. Requires a node that supports the getaddrmaninfo RPC.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-getinfo", "Get general information from the remote server. Note that unlike server-side RPC calls, the results of -getinfo is the result of multiple non-atomic requests. Some entries in the result may represent results from different states (e.g. wallet balance may be as of a different block from the chain state reported)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    SetupChainParamsBaseOptions();
    gArgs.AddArg("-named", strprintf("Pass named instead of positional arguments (default: %s)", DEFAULT_NAMED), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcclienttimeout=<n>", strprintf("Timeout in seconds during HTTP requests, or 0 for no timeout. (default: %d)", DEFAULT_HTTP_CLIENT_TIMEOUT), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcconnect=<ip>", strprintf("Send commands to node running on <ip> (default: %s)", DEFAULT_RPCCONNECT), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpccookiefile=<loc>", "Location of the auth cookie. Relative paths will be prefixed by a net-specific datadir location. (default: data dir)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcpassword=<pw>", "Password for JSON-RPC connections", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcport=<port>", strprintf("Connect to JSON-RPC on <port> (default: %u, testnet: %u,  changi: %u, devnet: %u, regtest: %u)", defaultBaseParams->RPCPort(), testnetBaseParams->RPCPort(), changiBaseParams->RPCPort(), devnetBaseParams->RPCPort(), regtestBaseParams->RPCPort()), ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcuser=<user>", "Username for JSON-RPC connections", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcwait", "Wait for RPC server to start", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcwaittimeout=<n>", strprintf("Timeout in seconds to wait for the RPC server to start, or 0 for no timeout. (default: %d)", DEFAULT_WAIT_CLIENT_TIMEOUT), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-rpcwallet=<walletname>", "Send RPC for non-default wallet on RPC server (needs to exactly match corresponding -wallet option passed to defid). This changes the RPC endpoint used, e.g. http://127.0.0.1:8554/wallet/<walletname>", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-stdin", "Read extra arguments from standard input, one per line until EOF/Ctrl-D (recommended for sensitive information such as passphrases). When combined with -stdinrpcpass, the first line from standard input is used for the RPC password.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-stdinrpcpass", "Read RPC password from standard input as a single line. When combined with -stdin, the first line from standard input is used for the RPC password.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-ethrpcport=<port>", strprintf("Listen for ETH-JSON-RPC connections on <port>> (default: %u, testnet: %u, changi: %u, devnet: %u, regtest: %u)", defaultBaseParams->ETHRPCPort(), testnetBaseParams->ETHRPCPort(), changiBaseParams->ETHRPCPort(), devnetBaseParams->ETHRPCPort(), regtestBaseParams->ETHRPCPort()), ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::RPC);
    gArgs.AddArg("-wsport=<port>", strprintf("Listen for ETH-WebSockets connections on <port>> (default: %u, testnet: %u, changi: %u, devnet: %u, regtest: %u)", defaultBaseParams->WSPort(), testnetBaseParams->WSPort(), changiBaseParams->WSPort(), devnetBaseParams->WSPort(), regtestBaseParams->ETHRPCPort()), ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::RPC);
    RPCMetadata::SetupArgs(gArgs);
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//

//
// Exception thrown on connection error.  This error is used to determine
// when to wait if -rpcwait is given.
//
class CConnectionFailed : public std::runtime_error
{
public:

    explicit inline CConnectionFailed(const std::string& msg) :
        std::runtime_error(msg)
    {}

};

//
// This function returns either one of EXIT_ codes when it's expected to stop the process or
// CONTINUE_EXECUTION when it's expected to continue further.
//
static int AppInitRPC(int argc, char* argv[])
{
    //
    // Parameters
    //
    SetupCliArgs();
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        tfm::format(std::cerr, "Error parsing command line arguments: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    if (argc < 2 || HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        std::string strUsage = PACKAGE_NAME " RPC client version " + FormatVersionAndSuffix() + "\n";
        if (!gArgs.IsArgSet("-version")) {
            strUsage += "\n"
                "Usage:  defi-cli [options] <command> [params]  Send command to " PACKAGE_NAME "\n"
                "or:     defi-cli [options] -named <command> [name=value]...  Send command to " PACKAGE_NAME " (with named arguments)\n"
                "or:     defi-cli [options] help                List commands\n"
                "or:     defi-cli [options] help <command>      Get help for a command\n";
            strUsage += "\n" + gArgs.GetHelpMessage();
        }

        tfm::format(std::cout, "%s", strUsage.c_str());
        if (argc < 2) {
            tfm::format(std::cerr, "Error: too few parameters\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (!CheckDataDirOption()) {
        tfm::format(std::cerr, "Error: Specified data directory \"%s\" does not exist.\n", gArgs.GetArg("-datadir", "").c_str());
        return EXIT_FAILURE;
    }
    if (!gArgs.ReadConfigFiles(error, true)) {
        tfm::format(std::cerr, "Error reading configuration file: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    RPCMetadata::InitFromArgs(gArgs);

    // Check for -testnet, -changi or -regtest parameter (BaseParams() calls are only valid after this clause)
    try {
        SelectBaseParams(gArgs.GetChainName());
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return CONTINUE_EXECUTION;
}


/** Reply structure of a HTTP request. */
struct HTTPReply
{
    HTTPReply(): status(0) {}

    int status;
    std::string body;
};

/**
 * Minimal HTTP/1.1 client used to talk to the RPC server.
 *
 * The client sends a single request over a plain socket and reads the
 * complete response. It replaces the libevent based client that was used
 * before.
 */
class HTTPClient
{
public:
    HTTPClient(std::string host, uint16_t port, int timeout)
        : m_host(std::move(host)), m_port(port), m_timeout(timeout) {}

    ~HTTPClient()
    {
        if (m_socket != INVALID_SOCKET) {
#ifdef WIN32
            closesocket(m_socket);
#else
            close(m_socket);
#endif
        }
    }

    HTTPClient(const HTTPClient&) = delete;
    HTTPClient& operator=(const HTTPClient&) = delete;

    /** Connect to the server. Returns false and sets `error` on failure. */
    bool Connect(std::string& error)
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        const std::string service = std::to_string(m_port);
        struct addrinfo* result{nullptr};
        const int rc = getaddrinfo(m_host.c_str(), service.c_str(), &hints, &result);
        if (rc != 0) {
            error = gai_strerror(rc);
            return false;
        }

        for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
            const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd == SOCKET_ERROR) continue;

            // Set the socket timeouts
            struct timeval tv;
            tv.tv_sec = m_timeout;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

            if (ConnectSocket(fd, ai->ai_addr, ai->ai_addrlen, error)) {
                m_socket = fd;
                break;
            }
#ifdef WIN32
            closesocket(fd);
#else
            close(fd);
#endif
        }
        freeaddrinfo(result);

        if (m_socket == INVALID_SOCKET) {
            if (error.empty()) error = "no connection could be established";
            return false;
        }
        return true;
    }

    /**
     * Send a request and read the response.
     *
     * @param[in] method The HTTP method (e.g. "POST").
     * @param[in] path The request target (e.g. "/").
     * @param[in] headers Additional request headers.
     * @param[in] body The request body.
     * @param[out] reply The status code and the body of the response.
     * @param[out] error The error description in case of a failure.
     */
    bool Request(const std::string& method,
                 const std::string& path,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body,
                 HTTPReply& reply,
                 std::string& error)
    {
        std::string request;
        request.reserve(body.size() + 256);
        request += method + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + m_host + "\r\n";
        for (const auto& header : headers) {
            request += header.first + ": " + header.second + "\r\n";
        }
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "\r\n";
        request += body;

        if (!SendAll(request, error)) return false;

        std::string response;
        if (!RecvAll(response, error)) return false;

        return ParseResponse(response, reply, error);
    }

private:
    bool ConnectSocket(int fd, const struct sockaddr* addr, socklen_t addr_len, std::string& error)
    {
        // Connect with a timeout: the socket is put into non-blocking mode for
        // the connect and polled afterwards.
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags == SOCKET_ERROR || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == SOCKET_ERROR) {
            error = "could not set the socket to non-blocking mode";
            return false;
        }
        if (connect(fd, addr, addr_len) == 0) {
            fcntl(fd, F_SETFL, flags);
            return true;
        }
        if (errno != EINPROGRESS) {
            error = strprintf("connect(): %s", strerror(errno));
            return false;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        const int rc = poll(&pfd, 1, m_timeout * 1000);
        if (rc == 0) {
            error = "connection timed out";
            return false;
        }
        if (rc < 0) {
            error = strprintf("poll(): %s", strerror(errno));
            return false;
        }
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len) != 0 || err != 0) {
            error = strprintf("connect(): %s", strerror(err ? err : errno));
            return false;
        }
        fcntl(fd, F_SETFL, flags);
        return true;
    }

    bool SendAll(const std::string& data, std::string& error)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = send(m_socket, data.data() + sent, data.size() - sent, 0);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    error = "timeout reached";
                    return false;
                }
                continue;
            }
            error = strprintf("send(): %s", strerror(errno));
            return false;
        }
        return true;
    }

    bool RecvAll(std::string& data, std::string& error)
    {
        char buf[4096];
        while (true) {
            const ssize_t n = recv(m_socket, buf, sizeof(buf), 0);
            if (n > 0) {
                data.append(buf, n);
                continue;
            }
            if (n == 0) {
                // The server closed the connection, the response is complete.
                return true;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                error = "timeout reached";
                return false;
            }
            error = strprintf("recv(): %s", strerror(errno));
            return false;
        }
    }

    static bool ParseResponse(const std::string& response, HTTPReply& reply, std::string& error)
    {
        const size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            error = "invalid response from server";
            return false;
        }
        const std::string status_line = response.substr(0, response.find("\r\n"));
        const size_t first_space = status_line.find(' ');
        if (first_space == std::string::npos) {
            error = "invalid response from server";
            return false;
        }
        const size_t second_space = status_line.find(' ', first_space + 1);
        const std::string status_str = status_line.substr(first_space + 1,
                                                          second_space == std::string::npos ? std::string::npos : second_space - first_space - 1);
        int parsed_status{0};
        const auto result = std::from_chars(status_str.data(), status_str.data() + status_str.size(), parsed_status);
        if (result.ec != std::errc{} || result.ptr != status_str.data() + status_str.size()) {
            error = "invalid response from server";
            return false;
        }
        reply.status = parsed_status;
        reply.body = response.substr(header_end + 4);
        return true;
    }

    std::string m_host;
    uint16_t m_port;
    int m_timeout;
    SOCKET m_socket{INVALID_SOCKET};
};

/** Class that handles the conversion from a command-line to a JSON-RPC request,
 * as well as converting back to a JSON object that can be shown as result.
 */
class BaseRequestHandler
{
public:
    virtual ~BaseRequestHandler() {}
    virtual UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) = 0;
    virtual UniValue ProcessReply(const UniValue &batch_in) = 0;
};

/** Process addrinfo requests */
class AddrinfoRequestHandler: public BaseRequestHandler
{
public:
    /** Create a simulated `getaddrmaninfo` request. */
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        if (!args.empty()) {
            throw std::runtime_error("-addrinfo takes no arguments");
        }
        return JSONRPCRequestObj("getaddrmaninfo", NullUniValue, 1);
    }

    /** Collect the counts from the reply and form a simulated `addrinfo` reply. */
    UniValue ProcessReply(const UniValue &reply) override
    {
        if (!reply["error"].isNull()) {
            if (reply["error"].isObject() && !reply["error"]["code"].isNull() &&
                reply["error"]["code"].getInt<int>() == RPC_METHOD_NOT_FOUND) {
                throw std::runtime_error("-addrinfo requires a node that supports the getaddrmaninfo RPC. Please upgrade your node or use defi-cli from the same version.");
            }
            return reply;
        }

        const std::vector<std::string>& network_types{reply["result"].getKeys()};
        const std::vector<UniValue>& addrman_counts{reply["result"].getValues()};

        UniValue result(UniValue::VOBJ), addresses(UniValue::VOBJ);
        for (size_t i = 0; i < network_types.size(); ++i) {
            const int64_t addr_count = addrman_counts[i]["total"].getInt<int64_t>();
            if (network_types[i] == "all_networks") {
                addresses.pushKV("total", addr_count);
            } else {
                addresses.pushKV(network_types[i], addr_count);
            }
        }
        result.pushKV("addresses_known", std::move(addresses));
        return JSONRPCReplyObj(result, NullUniValue, 1);
    }
};

/** Process getinfo requests */
class GetinfoRequestHandler: public BaseRequestHandler
{
public:
    const int ID_NETWORKINFO = 0;
    const int ID_BLOCKCHAININFO = 1;
    const int ID_WALLETINFO = 2;

    /** Create a simulated `getinfo` request. */
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        if (!args.empty()) {
            throw std::runtime_error("-getinfo takes no arguments");
        }
        UniValue result(UniValue::VARR);
        result.push_back(JSONRPCRequestObj("getnetworkinfo", NullUniValue, ID_NETWORKINFO));
        result.push_back(JSONRPCRequestObj("getblockchaininfo", NullUniValue, ID_BLOCKCHAININFO));
        result.push_back(JSONRPCRequestObj("getwalletinfo", NullUniValue, ID_WALLETINFO));
        return result;
    }

    /** Collect values from the batch and form a simulated `getinfo` reply. */
    UniValue ProcessReply(const UniValue &batch_in) override
    {
        UniValue result(UniValue::VOBJ);
        std::vector<UniValue> batch = JSONRPCProcessBatchReply(batch_in, 3);
        // Errors in getnetworkinfo() and getblockchaininfo() are fatal, pass them on
        // getwalletinfo() is allowed to fail in case there is no wallet.
        if (!batch[ID_NETWORKINFO]["error"].isNull()) {
            return batch[ID_NETWORKINFO];
        }
        if (!batch[ID_BLOCKCHAININFO]["error"].isNull()) {
            return batch[ID_BLOCKCHAININFO];
        }
        result.pushKV("version", batch[ID_NETWORKINFO]["result"]["version"]);
        result.pushKV("protocolversion", batch[ID_NETWORKINFO]["result"]["protocolversion"]);
        result.pushKV("blocks", batch[ID_BLOCKCHAININFO]["result"]["blocks"]);
        result.pushKV("timeoffset", batch[ID_NETWORKINFO]["result"]["timeoffset"]);
        result.pushKV("connections", batch[ID_NETWORKINFO]["result"]["connections"]);
        result.pushKV("proxy", batch[ID_NETWORKINFO]["result"]["networks"][0]["proxy"]);
        result.pushKV("difficulty", batch[ID_BLOCKCHAININFO]["result"]["difficulty"]);
        result.pushKV("chain", UniValue(batch[ID_BLOCKCHAININFO]["result"]["chain"]));
        if (!batch[ID_WALLETINFO].isNull()) {
            result.pushKV("walletversion", batch[ID_WALLETINFO]["result"]["walletversion"]);
            result.pushKV("balance", batch[ID_WALLETINFO]["result"]["balance"]);
            result.pushKV("keypoololdest", batch[ID_WALLETINFO]["result"]["keypoololdest"]);
            result.pushKV("keypoolsize", batch[ID_WALLETINFO]["result"]["keypoolsize"]);
            if (!batch[ID_WALLETINFO]["result"]["unlocked_until"].isNull()) {
                result.pushKV("unlocked_until", batch[ID_WALLETINFO]["result"]["unlocked_until"]);
            }
            result.pushKV("paytxfee", batch[ID_WALLETINFO]["result"]["paytxfee"]);
        }
        result.pushKV("relayfee", batch[ID_NETWORKINFO]["result"]["relayfee"]);
        result.pushKV("warnings", batch[ID_NETWORKINFO]["result"]["warnings"]);
        return JSONRPCReplyObj(result, NullUniValue, 1);
    }
};

/** Process default single requests */
class DefaultRequestHandler: public BaseRequestHandler {
public:
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        UniValue params;
        if(gArgs.GetBoolArg("-named", DEFAULT_NAMED)) {
            params = RPCConvertNamedValues(method, args);
        } else {
            params = RPCConvertValues(method, args);
        }
        return JSONRPCRequestObj(method, params, 1);
    }

    UniValue ProcessReply(const UniValue &reply) override
    {
        return reply.get_obj();
    }
};

static UniValue CallRPC(BaseRequestHandler *rh, const std::string& strMethod, const std::vector<std::string>& args)
{
    std::string host;
    // Specify a specific host IP, using -rpcconnect.
    // For DVM RPCs, in preference order, we choose the following for the port:
    //     1. use ports defined in the ports.lock file
    //     2. -rpcport
    //     3. port in -rpcconnect (ie following : in ipv4 or ]: in ipv6)
    //     4. default port for chain
    int dvmport = BaseParams().RPCPort();
    SplitHostPort(gArgs.GetArg("-rpcconnect", DEFAULT_RPCCONNECT), dvmport, host);
    dvmport = gArgs.GetArg("-rpcport", dvmport);

    // For EVM RPCs, in preference order, we choose the following for the evm port:
    //     1. use ports defined in the ports.lock file
    //     2. -ethrpcport
    //     3. default evm port for chain
    
    int evmport = BaseParams().ETHRPCPort();
    evmport = gArgs.GetArg("-ethrpcport", evmport);

    if (const auto port = GetPortFromLockFile(AutoPort::RPC); port) {
        dvmport = port;
    }
    if (const auto port = GetPortFromLockFile(AutoPort::ETHRPC); port) {
        evmport = port;
    }

    // Check if DVM or EVM RPC
    int port = dvmport;
    if (strMethod.rfind("eth_", 0) == 0 || strMethod.rfind("debug_", 0) == 0 || strMethod.rfind("net_", 0) == 0 || strMethod.rfind("web3_", 0) == 0)
        port = evmport;

    // Get credentials
    std::string strRPCUserColonPass;
    bool failedToGetAuthCookie = false;
    if (gArgs.GetArg("-rpcpassword", "") == "") {
        // Try fall back to cookie-based authentication if no password is provided
        if (!GetAuthCookie(&strRPCUserColonPass)) {
            failedToGetAuthCookie = true;
        }
    } else {
        strRPCUserColonPass = gArgs.GetArg("-rpcuser", "") + ":" + gArgs.GetArg("-rpcpassword", "");
    }

    std::vector<std::pair<std::string, std::string>> request_headers;
    request_headers.emplace_back("Content-Type", "application/json");
    request_headers.emplace_back("Connection", "close");
    request_headers.emplace_back("Authorization", std::string("Basic ") + EncodeBase64(strRPCUserColonPass));

    const auto meta = RPCMetadata::CreateDefault();
    const auto writerFunc = [&](const std::string& key, const std::string& val) { request_headers.emplace_back(key, val); };
    RPCMetadata::ToHTTPHeader(meta, writerFunc);

    // Attach request data
    // The JSON-RPC version is added per request object by JSONRPCRequestObj(),
    // so that batch requests (e.g. "-getinfo") carry it in every member.
    std::string strRequest = rh->PrepareRequest(strMethod, args).write() + "\n";

    // check if we should use a special wallet endpoint
    std::string endpoint = "/";
    if (!gArgs.GetArgs("-rpcwallet").empty()) {
        std::string walletName = gArgs.GetArg("-rpcwallet", "");
        endpoint = "/wallet/" + urlEncode(walletName);
    }

    const int timeout = gArgs.GetArg("-rpcclienttimeout", DEFAULT_HTTP_CLIENT_TIMEOUT);
    HTTPClient client(host, static_cast<uint16_t>(port), timeout);
    std::string clientError;
    if (!client.Connect(clientError)) {
        throw CConnectionFailed(strprintf("Could not connect to the server %s:%d (%s)\n\nMake sure the defid server is running and that you are connecting to the correct RPC port.", host, port, clientError));
    }

    HTTPReply response;
    if (!client.Request("POST", endpoint, request_headers, strRequest, response, clientError)) {
        throw CConnectionFailed(strprintf("Could not connect to the server %s:%d (%s)\n\nMake sure the defid server is running and that you are connecting to the correct RPC port.", host, port, clientError));
    }

    if (response.status == 0) {
        throw CConnectionFailed(strprintf("Could not connect to the server %s:%d\n\nMake sure the defid server is running and that you are connecting to the correct RPC port.", host, port));
    } else if (response.status == HTTP_UNAUTHORIZED) {
        if (failedToGetAuthCookie) {
            throw std::runtime_error(strprintf(
                "Could not locate RPC credentials. No authentication cookie could be found, and RPC password is not set.  See -rpcpassword and -stdinrpcpass.  Configuration file: (%s)",
                fs::PathToString(GetConfigFile(gArgs.GetArg("-conf", DEFI_CONF_FILENAME)))));
        } else {
            throw std::runtime_error("Authorization failed: Incorrect rpcuser or rpcpassword");
        }
    } else if (response.status == HTTP_SERVICE_UNAVAILABLE) {
        throw std::runtime_error(strprintf("Server response: %s", response.body));
    } else if (response.status >= 400 && response.status != HTTP_BAD_REQUEST && response.status != HTTP_NOT_FOUND && response.status != HTTP_INTERNAL_SERVER_ERROR)
        throw std::runtime_error(strprintf("server returned HTTP error %d", response.status));
    else if (response.body.empty())
        throw std::runtime_error("no response from server");

    // Parse reply
    UniValue valReply(UniValue::VSTR);
    if (!valReply.read(response.body))
        throw std::runtime_error("couldn't parse reply from server");
    const UniValue reply = rh->ProcessReply(valReply);
    if (reply.empty())
        throw std::runtime_error("expected reply to have result, error and id properties");

    return reply;
}

static int CommandLineRPC(int argc, char *argv[])
{
    std::string strPrint;
    int nRet = 0;
    try {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0])) {
            argc--;
            argv++;
        }
        std::string rpcPass;
        if (gArgs.GetBoolArg("-stdinrpcpass", false)) {
            if (!std::getline(std::cin, rpcPass)) {
                throw std::runtime_error("-stdinrpcpass specified but failed to read from standard input");
            }
            gArgs.ForceSetArg("-rpcpassword", rpcPass);
        }
        std::vector<std::string> args = std::vector<std::string>(&argv[1], &argv[argc]);
        if (gArgs.GetBoolArg("-stdin", false)) {
            // Read one arg per line from stdin and append
            std::string line;
            while (std::getline(std::cin, line)) {
                args.push_back(line);
            }
        }
        std::unique_ptr<BaseRequestHandler> rh;
        std::string method;
        if (gArgs.GetBoolArg("-addrinfo", false)) {
            rh.reset(new AddrinfoRequestHandler());
            method = "";
        } else if (gArgs.GetBoolArg("-getinfo", false)) {
            rh.reset(new GetinfoRequestHandler());
            method = "";
        } else {
            rh.reset(new DefaultRequestHandler());
            if (args.size() < 1) {
                throw std::runtime_error("too few parameters (need at least command)");
            }
            method = args[0];
            args.erase(args.begin()); // Remove trailing method name from arguments vector
        }

        // Execute and handle connection failures with -rpcwait
        const bool fWait = gArgs.GetBoolArg("-rpcwait", false);
        const int64_t timeout = gArgs.GetArg("-rpcwaittimeout", (int64_t)DEFAULT_WAIT_CLIENT_TIMEOUT);
        // Use a monotonic clock so that the deadline is not affected by -mocktime.
        const int64_t bounded_timeout{std::min<int64_t>(std::max<int64_t>(timeout, 0), MAX_WAIT_CLIENT_TIMEOUT)};
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds(bounded_timeout)};
        do {
            try {
                const UniValue reply = CallRPC(rh.get(), method, args);

                // Parse reply
                const UniValue& result = reply.find_value("result");
                const UniValue& error  = reply.find_value("error");

                if (!error.isNull()) {
                    // Error
                    int code = error["code"].getInt<int>();
                    if (fWait && code == RPC_IN_WARMUP)
                        throw CConnectionFailed("server in warmup");
                    strPrint = "error: " + error.write();
                    nRet = abs(code);
                    if (error.isObject())
                    {
                        UniValue errCode = error.find_value("code");
                        UniValue errMsg  = error.find_value("message");
                        strPrint = errCode.isNull() ? "" : "error code: "+errCode.getValStr()+"\n";

                        if (errMsg.isStr())
                            strPrint += "error message:\n"+errMsg.get_str();

                        if (errCode.isNum() && errCode.getInt<int>() == RPC_WALLET_NOT_SPECIFIED) {
                            strPrint += "\nTry adding \"-rpcwallet=<filename>\" option to defi-cli command line.";
                        }
                    }
                } else {
                    // Result
                    if (result.isNull())
                        strPrint = "";
                    else if (result.isStr())
                        strPrint = result.get_str();
                    else
                        strPrint = result.write(2);
                }
                // Connection succeeded, no need to retry.
                break;
            }
            catch (const CConnectionFailed& e) {
                if (fWait && (timeout <= 0 || std::chrono::steady_clock::now() < deadline)) {
                    UninterruptibleSleep(std::chrono::milliseconds{1000});
                } else if (fWait) {
                    throw CConnectionFailed(strprintf("timeout on transient error: %s", e.what()));
                } else {
                    throw;
                }
            }
        } while (fWait);
    }
    catch (const std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    }
    catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRPC()");
        throw;
    }

    if (strPrint != "") {
        tfm::format(nRet == 0 ? std::cout : std::cerr, "%s\n", strPrint.c_str());
    }
    return nRet;
}

int main(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif
    SetupEnvironment();
    if (!SetupNetworking()) {
        tfm::format(std::cerr, "Error: Initializing networking failed\n");
        return EXIT_FAILURE;
    }
    try {
        int ret = AppInitRPC(argc, argv);
        if (ret != CONTINUE_EXECUTION)
            return ret;
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInitRPC()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(nullptr, "AppInitRPC()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRPC(argc, argv);
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRPC()");
    } catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRPC()");
    }
    return ret;
}
