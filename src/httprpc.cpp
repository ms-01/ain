// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <httprpc.h>

#include <chainparams.h>
#include <crypto/hmac_sha256.h>
#include <httpserver.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/stats.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <walletinitinterface.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <thread>

#include <boost/algorithm/string.hpp> // boost::trim

/** WWW-Authenticate to present with 401 Unauthorized response */
static const char* WWW_AUTH_HEADER_DATA = "Basic realm=\"jsonrpc\"";

/**
 * Timer service for the RPC mechanism.
 *
 * A single dedicated thread executes the scheduled callbacks, ordered by their
 * deadline. The service replaces the libevent based timer that was used
 * before; the RPCTimerInterface/RPCTimerBase contract is unchanged.
 */
class RPCTimerService
{
public:
    /** Handle of a scheduled callback; cancels the callback when destroyed. */
    struct TimerHandle
    {
        ~TimerHandle()
        {
            cancelled.store(true);
        }
        std::atomic<bool> cancelled{false};
    };

    ~RPCTimerService()
    {
        Stop();
    }

    /** Start the timer thread. */
    void Start()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_thread.joinable()) return;
        m_stopped = false;
        m_thread = std::thread(&RPCTimerService::Thread, this);
    }

    /**
     * Stop the timer thread and wait for it to exit.
     *
     * Pending callbacks are discarded, a callback that is currently executed is
     * finished before this function returns. This makes sure that no callback
     * can run concurrently with the teardown of the RPC server.
     */
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_thread.joinable()) return;
            m_stopped = true;
        }
        m_cond.notify_all();
        m_thread.join();
    }

    /**
     * Schedule a one-shot callback.
     *
     * @returns A handle that cancels the callback when it is destroyed.
     */
    std::shared_ptr<TimerHandle> Schedule(std::function<void()> func, int64_t millis)
    {
        auto handle = std::make_shared<TimerHandle>();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
            m_queue.emplace(deadline, Entry{handle, std::move(func)});
        }
        m_cond.notify_all();
        return handle;
    }

private:
    struct Entry {
        std::shared_ptr<TimerHandle> handle;
        std::function<void()> func;
    };

    void Thread()
    {
        util::ThreadRename("rpctimer");
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            if (m_stopped) return;
            if (m_queue.empty()) {
                m_cond.wait(lock);
                continue;
            }
            auto it = m_queue.begin();
            const auto now = std::chrono::steady_clock::now();
            if (it->first > now) {
                m_cond.wait_until(lock, it->first);
                continue;
            }
            Entry entry = std::move(it->second);
            m_queue.erase(it);
            if (entry.handle->cancelled.load()) continue;
            lock.unlock();
            try {
                entry.func();
            } catch (const std::exception& e) {
                LogPrintf("RPC timer callback failed: %s\n", e.what());
            } catch (...) {
                LogPrintf("RPC timer callback failed with an unknown exception\n");
            }
            lock.lock();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::multimap<std::chrono::steady_clock::time_point, Entry> m_queue;
    std::thread m_thread;
    bool m_stopped{false};
};

/** Simple one-shot callback timer to be used by the RPC mechanism to e.g.
 * re-lock the wallet.
 */
class HTTPRPCTimer : public RPCTimerBase
{
public:
    HTTPRPCTimer(RPCTimerService& service, std::function<void()>& func, int64_t millis) :
        m_handle(service.Schedule(func, millis))
    {
    }

private:
    std::shared_ptr<RPCTimerService::TimerHandle> m_handle;
};

class HTTPRPCTimerInterface : public RPCTimerInterface
{
public:
    explicit HTTPRPCTimerInterface(RPCTimerService& _service) : service(_service)
    {
    }
    const char* Name() override
    {
        return "HTTP";
    }
    RPCTimerBase* NewTimer(std::function<void()>& func, int64_t millis) override
    {
        return new HTTPRPCTimer(service, func, millis);
    }
private:
    RPCTimerService& service;
};


/* Pre-base64-encoded authentication token */
static std::string strRPCUserColonPass;
/* Pre-parsed multi-user credentials from -rpcauth (name, salt, hash) */
static std::vector<std::vector<std::string>> g_rpcauth;
/* Stored RPC timer interface (for unregistration) */
static std::unique_ptr<HTTPRPCTimerInterface> httpRPCTimerInterface;
/* The timer service that executes the scheduled RPC callbacks */
static RPCTimerService rpcTimerService;
/* The host to be used for CORS header */
static std::string corsOriginHost;

static void JSONErrorReply(HTTPRequest* req, const UniValue& objError, const UniValue& id)
{
    // Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = objError.find_value("code").getInt<int>();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    std::string strReply = JSONRPCReply(NullUniValue, objError, id);

    req->WriteHeader("Content-Type", "application/json");
    req->WriteReply(nStatus, strReply);
}

//This function checks username and password against -rpcauth
//entries from config file.
static bool multiUserAuthorized(std::string strUserPass)
{
    if (strUserPass.find(':') == std::string::npos) {
        return false;
    }
    std::string strUser = strUserPass.substr(0, strUserPass.find(':'));
    std::string strPass = strUserPass.substr(strUserPass.find(':') + 1);

    for (const std::vector<std::string>& vFields : g_rpcauth) {
        std::string strName = vFields[0];
        if (!TimingResistantEqual(strName, strUser)) {
            continue;
        }

        std::string strSalt = vFields[1];
        std::string strHash = vFields[2];

        static const unsigned int KEY_SIZE = 32;
        unsigned char out[KEY_SIZE];

        CHMAC_SHA256(reinterpret_cast<const unsigned char*>(strSalt.c_str()), strSalt.size()).Write(reinterpret_cast<const unsigned char*>(strPass.c_str()), strPass.size()).Finalize(out);
        std::vector<unsigned char> hexvec(out, out+KEY_SIZE);
        std::string strHashFromPass = HexStr(hexvec);

        if (TimingResistantEqual(strHashFromPass, strHash)) {
            return true;
        }
    }
    return false;
}

static bool RPCAuthorized(const std::string& strAuth, std::string& strAuthUsernameOut)
{
    if (strRPCUserColonPass.empty()) // Belt-and-suspenders measure if InitRPCAuthentication was not called
        return false;
    if (strAuth.substr(0, 6) != "Basic ")
        return false;
    std::string strUserPass64 = strAuth.substr(6);
    boost::trim(strUserPass64);
    std::string strUserPass = DecodeBase64(strUserPass64);

    if (strUserPass.find(':') != std::string::npos)
        strAuthUsernameOut = strUserPass.substr(0, strUserPass.find(':'));

    //Check if authorized under single-user field
    if (TimingResistantEqual(strUserPass, strRPCUserColonPass)) {
        return true;
    }
    return multiUserAuthorized(strUserPass);
}

static bool CorsHandler(HTTPRequest *req) {
    auto host = corsOriginHost;
    // If if it's empty assume cors is disallowed. Do nothing and proceed,
    // with request as usual.
    if (host.empty())
        return false;

    req->WriteHeader("Access-Control-Allow-Origin", host);
    if (host != "*")
        req->WriteHeader("Access-Control-Allow-Credentials", "true");
    req->WriteHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    req->WriteHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    // If it's a Cors preflight request, short-circuit and return. We have
    // set what's needed already.
    if (req->GetRequestMethod() == HTTPRequest::OPTIONS) {
        req->WriteReply(HTTP_NO_CONTENT);
        return true;
    }

    // Indicate to continue with the request as usual
    return false;
}

static bool HTTPReq_JSONRPC(HTTPRequest* req, const std::string &)
{
    int64_t time = GetTimeMillis();
    // Handle CORS
    if (CorsHandler(req))
        return true;

    // JSONRPC handles only POST
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return false;
    }
    // Check authorization
    std::pair<bool, std::string> authHeader = req->GetHeader("authorization");
    if (!authHeader.first) {
        req->WriteHeader("WWW-Authenticate", WWW_AUTH_HEADER_DATA);
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    JSONRPCRequest jreq;
    jreq.peerAddr = req->GetPeer().ToString();
    if (!RPCAuthorized(authHeader.second, jreq.authUser)) {
        LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", jreq.peerAddr);

        /* Deter brute-forcing
           If this results in a DoS the user really
           shouldn't have their RPC port exposed. */
        UninterruptibleSleep(std::chrono::milliseconds{250});

        req->WriteHeader("WWW-Authenticate", WWW_AUTH_HEADER_DATA);
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    try {
        // Parse request
        UniValue valRequest;
        if (!valRequest.read(req->ReadBody()))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

        // Set the URI
        jreq.URI = req->GetURI();
        // Parse the metadata from request
        auto func = [&](const std::string& h) { return req->GetHeader(h); };
        RPCMetadata::FromHTTPHeader(jreq.metadata, func);

        std::string strReply;
        // singleton request
        if (valRequest.isObject()) {
            jreq.parse(valRequest);

            UniValue result = tableRPC.execute(jreq);

            // Send reply
            strReply = JSONRPCReply(result, NullUniValue, jreq.id);

        // array of requests
        } else if (valRequest.isArray())
            strReply = JSONRPCExecBatch(jreq, valRequest.get_array());
        else
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");

        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strReply);

        if (statsRPC.isActive()) statsRPC.add(jreq.strMethod, GetTimeMillis() - time, strReply.length());
    } catch (const UniValue& objError) {
        JSONErrorReply(req, objError, jreq.id);
        return false;
    } catch (const std::exception& e) {
        JSONErrorReply(req, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
        return false;
    }
    return true;
}

static bool InitRPCAuthentication()
{
    if (gArgs.GetArg("-rpcpassword", "") == "")
    {
        LogPrintf("No rpcpassword set - using random cookie authentication.\n");
        if (!GenerateAuthCookie(&strRPCUserColonPass)) {
            uiInterface.ThreadSafeMessageBox(
                _("Error: A fatal internal error occurred, see debug.log for details").translated, // Same message as AbortNode
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
    } else {
        LogPrintf("Config options rpcuser and rpcpassword will soon be deprecated. Locally-run instances may remove rpcuser to use cookie-based auth, or may be replaced with rpcauth. Please see share/rpcauth for rpcauth auth generation.\n");
        strRPCUserColonPass = gArgs.GetArg("-rpcuser", "") + ":" + gArgs.GetArg("-rpcpassword", "");
    }
    // Parse the -rpcauth entries once at startup, so that a malformed entry is
    // reported as a configuration error instead of silently failing every
    // multi-user login later on. The accepted grammar is the very same one the
    // runtime authentication path uses (name, salt and hash separated by ':' or
    // '$'), therefore no working configuration is rejected here.
    for (const std::string& strRPCAuth : gArgs.GetArgs("-rpcauth")) {
        if (strRPCAuth.empty()) {
            // An empty value is ignored, just like it is at runtime.
            continue;
        }
        std::vector<std::string> vFields;
        boost::split(vFields, strRPCAuth, boost::is_any_of(":$"));
        if (vFields.size() != 3) {
            // Do not log the value itself, it may contain credentials.
            LogPrintf("Invalid -rpcauth argument.\n");
            uiInterface.ThreadSafeMessageBox(
                _("Invalid -rpcauth argument. Expected the format <user>:<salt>$<hash>.").translated,
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
        g_rpcauth.push_back(std::move(vFields));
    }
    if (!g_rpcauth.empty())
    {
        LogPrintf("Using rpcauth authentication.\n");
    }
    return true;
}

bool StartHTTPRPC()
{
    LogPrint(BCLog::RPC, "Starting HTTP RPC server\n");
    if (!InitRPCAuthentication())
        return false;

    // Setup Cors origin host name from arg.
    corsOriginHost = gArgs.GetArg("-rpcallowcors", "");

    RegisterHTTPHandler("/", true, HTTPReq_JSONRPC);
    if (g_wallet_init_interface.HasWalletSupport()) {
        RegisterHTTPHandler("/wallet/", false, HTTPReq_JSONRPC);
    }
    rpcTimerService.Start();
    httpRPCTimerInterface = std::make_unique<HTTPRPCTimerInterface>(rpcTimerService);
    RPCSetTimerInterface(httpRPCTimerInterface.get());
    if (statsRPC.isActive()) statsRPC.load();
    return true;
}

void InterruptHTTPRPC()
{
    LogPrint(BCLog::RPC, "Interrupting HTTP RPC server\n");
}

void StopHTTPRPC()
{
    LogPrint(BCLog::RPC, "Stopping HTTP RPC server\n");
    UnregisterHTTPHandler("/", true);
    if (g_wallet_init_interface.HasWalletSupport()) {
        UnregisterHTTPHandler("/wallet/", false);
    }
    if (httpRPCTimerInterface) {
        RPCUnsetTimerInterface(httpRPCTimerInterface.get());
        httpRPCTimerInterface.reset();
    }
    // Stop and join the timer thread, so that no callback can run anymore
    // once the RPC server is torn down.
    rpcTimerService.Stop();

    if (statsRPC.isActive()) statsRPC.save();
}

CRPCStats statsRPC;
