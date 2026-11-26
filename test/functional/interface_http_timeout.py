#!/usr/bin/env python3
# Copyright (c) 2026 The DeFiChain developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test that the HTTP server idle timeout does not drop in-flight RPCs."""

import http.client
import time
import urllib.parse

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, str_to_b64str


class HTTPIdleTimeoutTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-rpcservertimeout=1"]]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        url = urllib.parse.urlparse(self.nodes[0].url)
        authpair = url.username + ":" + url.password
        headers = {
            "Authorization": "Basic " + str_to_b64str(authpair),
            "Connection": "keep-alive",
        }

        self.log.info("Testing in-flight RPC exceeding -rpcservertimeout...")
        # An RPC running longer than -rpcservertimeout (2.5s > 1s) must not be
        # disconnected by the idle timeout.
        conn = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
        conn.connect()
        conn.request(
            "POST",
            "/",
            '{"method": "waitfornewblock", "params": [2500]}',
            headers,
        )
        resp = conn.getresponse()
        body = resp.read()
        assert_equal(resp.status, http.client.OK)
        assert b'"error":null' in body
        assert conn.sock is not None

        # Follow-up request on the same keep-alive connection must still work.
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        resp = conn.getresponse()
        body = resp.read()
        assert_equal(resp.status, http.client.OK)
        assert b'"error":null' in body
        assert conn.sock is not None

        self.log.info("Testing idle keep-alive connection still times out...")
        # An idle keep-alive connection with no in-flight RPC must still time out.
        time.sleep(2.0)
        closed = False
        try:
            conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
            conn.getresponse().read()
        except (
            http.client.BadStatusLine,
            http.client.RemoteDisconnected,
            BrokenPipeError,
            ConnectionResetError,
        ):
            closed = True
        finally:
            conn.close()
        assert closed, "Expected idle keep-alive connection to be closed after timeout"


if __name__ == "__main__":
    HTTPIdleTimeoutTest().main()
