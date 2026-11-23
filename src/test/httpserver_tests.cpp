// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>
#include <netaddress.h>
#include <util/url.h>

#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_FIXTURE_TEST_SUITE(httpserver_tests, BasicTestingSetup)

static HTTPRequestData Parse(const std::string& request, HTTPParseStatus& status)
{
    HTTPRequestData data;
    status = ParseHTTPRequest(request, data);
    return data;
}

BOOST_AUTO_TEST_CASE(parse_simple_get)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::COMPLETE);
    BOOST_CHECK_EQUAL(data.method, HTTPRequest::GET);
    BOOST_CHECK_EQUAL(data.uri, "/");
    BOOST_CHECK(data.keep_alive);
    BOOST_CHECK_EQUAL(data.body, "");
    BOOST_CHECK_EQUAL(data.consumed, std::string("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").size());
    const std::string* host{nullptr};
    for (const auto& header : data.headers) {
        if (header.first == "Host") host = &header.second;
    }
    BOOST_REQUIRE(host != nullptr);
    BOOST_CHECK_EQUAL(*host, "127.0.0.1");
}

BOOST_AUTO_TEST_CASE(parse_query_string_is_part_of_the_uri)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("POST /?query=1 HTTP/1.1\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::COMPLETE);
    BOOST_CHECK_EQUAL(data.method, HTTPRequest::POST);
    BOOST_CHECK_EQUAL(data.uri, "/?query=1");
}

BOOST_AUTO_TEST_CASE(parse_post_with_body)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::COMPLETE);
    BOOST_CHECK_EQUAL(data.method, HTTPRequest::POST);
    BOOST_CHECK_EQUAL(data.body, "hello");
    BOOST_CHECK_EQUAL(data.consumed, std::string("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello").size());
}

BOOST_AUTO_TEST_CASE(parse_incomplete_request)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhe", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::INCOMPLETE);
    BOOST_CHECK_EQUAL(data.consumed, 0U);

    const HTTPRequestData headers_only = Parse("GET / HTTP/1.1\r\nHost: x\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::INCOMPLETE);
}

BOOST_AUTO_TEST_CASE(parse_malformed_request)
{
    HTTPParseStatus status;
    Parse("GET\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::ERROR);

    Parse("GET / HTTP/2.0\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::ERROR);

    Parse("GET / HTTP/1.1\r\nbroken-header\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::ERROR);

    Parse("GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::ERROR);
}

BOOST_AUTO_TEST_CASE(parse_headers_too_large)
{
    HTTPParseStatus status;
    const std::string request = "GET /" + std::string(10000, 'x') + " HTTP/1.1\r\n\r\n";
    Parse(request, status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::ERROR);
}

BOOST_AUTO_TEST_CASE(parse_keep_alive)
{
    HTTPParseStatus status;
    // HTTP/1.1 keeps the connection open by default
    BOOST_CHECK(Parse("GET / HTTP/1.1\r\n\r\n", status).keep_alive);
    // ... unless the client asks for the opposite
    BOOST_CHECK(!Parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", status).keep_alive);
    // HTTP/1.0 closes the connection by default
    BOOST_CHECK(!Parse("GET / HTTP/1.0\r\n\r\n", status).keep_alive);
    // ... unless the client asks for the opposite
    BOOST_CHECK(Parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", status).keep_alive);
}

BOOST_AUTO_TEST_CASE(parse_unknown_method)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("DELETE / HTTP/1.1\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::COMPLETE);
    BOOST_CHECK_EQUAL(data.method, HTTPRequest::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(reply_is_built_correctly)
{
    std::string response;
    {
        HTTPRequest request{CService(), HTTPRequest::POST, "/", {}, "{}", true,
                            [&response](const std::string& reply) { response = reply; }};
        request.WriteHeader("Content-Type", "application/json");
        request.WriteReply(200, "{\"result\":42}");
    }
    BOOST_CHECK(response.find("HTTP/1.1 200 OK\r\n") == 0);
    BOOST_CHECK(response.find("Content-Type: application/json\r\n") != std::string::npos);
    BOOST_CHECK(response.find("Content-Length: 13\r\n") != std::string::npos);
    BOOST_CHECK(response.find("Connection: keep-alive\r\n") != std::string::npos);
    BOOST_CHECK(response.find("\r\n\r\n{\"result\":42}") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(unhandled_request_gets_an_internal_server_error)
{
    std::string response;
    {
        HTTPRequest request{CService(), HTTPRequest::GET, "/", {}, "", false,
                            [&response](const std::string& reply) { response = reply; }};
        // The request is destroyed without a reply being written
    }
    BOOST_CHECK(response.find("HTTP/1.1 500 Internal Server Error\r\n") == 0);
    BOOST_CHECK(response.find("Connection: close\r\n") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(headers_are_case_insensitive)
{
    HTTPParseStatus status;
    const HTTPRequestData data = Parse("GET / HTTP/1.1\r\nauthorization: Basic abc\r\n\r\n", status);
    BOOST_CHECK_EQUAL(status, HTTPParseStatus::COMPLETE);

    HTTPRequest request{CService(), data.method, data.uri, data.headers, data.body, data.keep_alive,
                        [](const std::string&) {}};
    const auto header = request.GetHeader("Authorization");
    BOOST_CHECK(header.first);
    BOOST_CHECK_EQUAL(header.second, "Basic abc");
    const auto missing = request.GetHeader("Content-Type");
    BOOST_CHECK(!missing.first);
}

BOOST_AUTO_TEST_CASE(body_can_only_be_read_once)
{
    std::string response;
    HTTPRequest request{CService(), HTTPRequest::POST, "/", {}, "{\"a\":1}", true,
                        [&response](const std::string& reply) { response = reply; }};
    BOOST_CHECK_EQUAL(request.ReadBody(), "{\"a\":1}");
    BOOST_CHECK_EQUAL(request.ReadBody(), "");
}

BOOST_AUTO_TEST_CASE(url_decoding)
{
    BOOST_CHECK_EQUAL(urlDecode("hello%20world"), "hello world");
    BOOST_CHECK_EQUAL(urlDecode("hello+world"), "hello+world");
    BOOST_CHECK_EQUAL(urlDecode("%2F"), "/");
    // Invalid escapes are kept as they are
    BOOST_CHECK_EQUAL(urlDecode("%zz"), "%zz");
    BOOST_CHECK_EQUAL(urlDecode("100%"), "100%");
}

BOOST_AUTO_TEST_CASE(url_encoding)
{
    BOOST_CHECK_EQUAL(urlEncode("hello"), "hello");
    BOOST_CHECK_EQUAL(urlEncode("hello world"), "hello%20world");
    BOOST_CHECK_EQUAL(urlEncode("a/b"), "a%2Fb");
    BOOST_CHECK_EQUAL(urlEncode("a-b_c.d~e"), "a-b_c.d~e");
}

BOOST_AUTO_TEST_SUITE_END()
