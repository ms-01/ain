#!/usr/bin/env python3
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the RPC HTTP basics."""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal, str_to_b64str

import http.client
import subprocess
import urllib.parse
from threading import Thread


def test_work_queue_getblock(node, got_exceeded_error):
    """Issue a blocking RPC until the server rejects a request with a 503."""
    while not got_exceeded_error:
        try:
            # The timeout of waitfornewblock is given in milliseconds, keep the
            # request busy long enough for the work queue to overflow.
            node.cli("waitfornewblock", "5000").send_cli()
        except subprocess.CalledProcessError as e:
            assert "Server response: Work queue depth exceeded" in e.output
            got_exceeded_error.append(True)


class HTTPBasicsTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 3

    def setup_network(self):
        self.setup_nodes()

    def test_work_queue_exceeded(self):
        self.log.info("Testing work queue exceeded...")
        self.restart_node(0, ["-rpcworkqueue=1", "-rpcthreads=1"])
        got_exceeded_error = []
        threads = []
        for _ in range(3):
            t = Thread(
                target=test_work_queue_getblock,
                args=(self.nodes[0], got_exceeded_error),
            )
            t.start()
            threads.append(t)
        for t in threads:
            t.join()
        assert got_exceeded_error

    def run_test(self):
        #################################################
        # lowlevel check for http persistent connection #
        #################################################
        url = urllib.parse.urlparse(self.nodes[0].url)
        authpair = url.username + ":" + url.password
        headers = {"Authorization": "Basic " + str_to_b64str(authpair)}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        out1 = conn.getresponse().read()
        assert b'"error":null' in out1
        assert (
            conn.sock is not None
        )  # according to http/1.1 connection must still be open!

        # send 2nd request without closing connection
        conn.request("POST", "/", '{"method": "getchaintips"}', headers)
        out1 = conn.getresponse().read()
        assert (
            b'"error":null' in out1
        )  # must also response with a correct json-rpc message
        assert (
            conn.sock is not None
        )  # according to http/1.1 connection must still be open!
        conn.close()

        # same should be if we add keep-alive because this should be the std. behaviour
        headers = {
            "Authorization": "Basic " + str_to_b64str(authpair),
            "Connection": "keep-alive",
        }

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        out1 = conn.getresponse().read()
        assert b'"error":null' in out1
        assert (
            conn.sock is not None
        )  # according to http/1.1 connection must still be open!

        # send 2nd request without closing connection
        conn.request("POST", "/", '{"method": "getchaintips"}', headers)
        out1 = conn.getresponse().read()
        assert (
            b'"error":null' in out1
        )  # must also response with a correct json-rpc message
        assert (
            conn.sock is not None
        )  # according to http/1.1 connection must still be open!
        conn.close()

        # now do the same with "Connection: close"
        headers = {
            "Authorization": "Basic " + str_to_b64str(authpair),
            "Connection": "close",
        }

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        out1 = conn.getresponse().read()
        assert b'"error":null' in out1
        assert conn.sock is None  # now the connection must be closed after the response

        # node1 (2nd node) is running with disabled keep-alive option
        urlNode1 = urllib.parse.urlparse(self.nodes[1].url)
        authpair = urlNode1.username + ":" + urlNode1.password
        headers = {"Authorization": "Basic " + str_to_b64str(authpair)}

        conn = http.client.HTTPConnection(urlNode1.hostname, urlNode1.port)
        conn.connect()
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        out1 = conn.getresponse().read()
        assert b'"error":null' in out1

        # node2 (third node) is running with standard keep-alive parameters which means keep-alive is on
        urlNode2 = urllib.parse.urlparse(self.nodes[2].url)
        authpair = urlNode2.username + ":" + urlNode2.password
        headers = {"Authorization": "Basic " + str_to_b64str(authpair)}

        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request("POST", "/", '{"method": "getbestblockhash"}', headers)
        out1 = conn.getresponse().read()
        assert b'"error":null' in out1
        assert (
            conn.sock is not None
        )  # connection must be closed because defid should use keep-alive by default

        # Check excessive request size
        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request("GET", "/" + ("x" * 1000), "", headers)
        out1 = conn.getresponse()
        assert_equal(out1.status, http.client.NOT_FOUND)

        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request("GET", "/" + ("x" * 10000), "", headers)
        out1 = conn.getresponse()
        assert_equal(out1.status, http.client.BAD_REQUEST)

        # The RPC work queue overflow must be reported as "503 Service Unavailable".
        self.test_work_queue_exceeded()


if __name__ == "__main__":
    HTTPBasicsTest().main()
