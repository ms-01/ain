// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_HTTPSERVER_H
#define DEFI_HTTPSERVER_H

#include <netaddress.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

static const int DEFAULT_HTTP_THREADS=4;
static const int DEFAULT_HTTP_WORKQUEUE=16;
static const int DEFAULT_HTTP_SERVER_TIMEOUT=30;

class CService;
class HTTPRequest;

/** Initialize HTTP server.
 * Call this before RegisterHTTPHandler().
 */
bool InitHTTPServer();
/** Start HTTP server.
 * This is separate from InitHTTPServer to give users race-condition-free time
 * to register their handlers between InitHTTPServer and StartHTTPServer.
 */
void StartHTTPServer();
/** Interrupt HTTP server threads */
void InterruptHTTPServer();
/** Stop HTTP server */
void StopHTTPServer();

/** Handler for requests to a certain HTTP path */
typedef std::function<bool(HTTPRequest* req, const std::string &)> HTTPRequestHandler;
/** Register handler for prefix.
 * If multiple handlers match a prefix, the first-registered one will
 * be invoked.
 */
void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler);
/** Unregister handler for prefix */
void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch);

/** In-flight HTTP request.
 *
 * The request is handed over to the HTTP handler by a worker thread of the
 * HTTP server. The complete response is sent back to the client through the
 * reply sender callback that is provided by the server.
 */
class HTTPRequest
{
public:
    enum RequestMethod {
        UNKNOWN,
        GET,
        POST,
        HEAD,
        PUT,
        OPTIONS,
    };

    /** Callback used by the server to send the complete response to the client. */
    using ReplySender = std::function<void(const std::string&)>;

    HTTPRequest(CService peer,
                RequestMethod method,
                std::string uri,
                std::vector<std::pair<std::string, std::string>> headers,
                std::string body,
                bool keep_alive,
                ReplySender reply_sender);

    ~HTTPRequest();

    HTTPRequest(const HTTPRequest&) = delete;
    HTTPRequest& operator=(const HTTPRequest&) = delete;

    /** Get requested URI.
     */
    std::string GetURI() const { return m_uri; }

    /** Get CService (address:ip) for the origin of the http request.
     */
    CService GetPeer() const { return m_peer; }

    /** Get request method.
     */
    RequestMethod GetRequestMethod() const { return m_method; }

    /**
     * Get the request header specified by hdr, or an empty string.
     * Return a pair (isPresent,string).
     */
    std::pair<bool, std::string> GetHeader(const std::string& hdr) const;

    /**
     * Read request body.
     *
     * @note As this consumes the underlying buffer, call this only once.
     * Repeated calls will return an empty string.
     */
    std::string ReadBody();

    /**
     * Write output header.
     *
     * @note call this before calling WriteReply.
     */
    void WriteHeader(const std::string& hdr, const std::string& value);

    /**
     * Write HTTP reply.
     * nStatus is the HTTP status code to send.
     * strReply is the body of the reply. Keep it empty to send a standard message.
     *
     * @note Can be called only once.
     */
    void WriteReply(int nStatus, const std::string& strReply = "");

private:
    CService m_peer;
    RequestMethod m_method;
    std::string m_uri;
    std::vector<std::pair<std::string, std::string>> m_request_headers;
    std::vector<std::pair<std::string, std::string>> m_response_headers;
    std::string m_body;
    bool m_body_consumed{false};
    bool m_reply_sent{false};
    bool m_keep_alive{true};
    ReplySender m_reply_sender;
};

/** Status of an attempt to parse an HTTP request. */
enum class HTTPParseStatus {
    /** More data is needed to complete the request. */
    INCOMPLETE,
    /** A complete request was parsed. */
    COMPLETE,
    /** The data does not contain a valid HTTP request. */
    ERROR,
};

/** A HTTP request that was parsed from a byte stream. */
struct HTTPRequestData {
    HTTPRequest::RequestMethod method{HTTPRequest::UNKNOWN};
    /** The request target (path and optional query string) as sent by the client. */
    std::string uri;
    /** The request headers, in the order they were received. */
    std::vector<std::pair<std::string, std::string>> headers;
    /** The request body. */
    std::string body;
    /** Whether the connection should be kept alive after the reply was sent. */
    bool keep_alive{true};
    /** Number of bytes of the buffer that belong to this request. */
    size_t consumed{0};
    /** Description of the parse error (only set if the status is ERROR). */
    std::string error;
};

/**
 * Parse a HTTP request from the beginning of the given buffer.
 *
 * @param[in] buffer The received data.
 * @param[out] out The parsed request; only fully populated on COMPLETE.
 * @returns The parse status.
 */
HTTPParseStatus ParseHTTPRequest(std::string_view buffer, HTTPRequestData& out);

#endif // DEFI_HTTPSERVER_H
