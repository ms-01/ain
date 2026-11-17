#!/usr/bin/env python3
# Copyright (c) 2015-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the setban rpc call."""

import json
import os

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    connect_nodes,
    p2p_port,
)


class SetBanTests(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[], []]

    def banlist_path(self, node):
        return os.path.join(node.datadir, node.chain, "banlist.json")

    def legacy_banlist_path(self, node):
        return os.path.join(node.datadir, node.chain, "banlist.dat")

    def run_test(self):
        # Node 0 connects to Node 1, check that the noban permission is not granted
        connect_nodes(self.nodes[0], 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert not "noban" in peerinfo["permissions"]

        # Node 0 get banned by Node 1
        self.nodes[1].setban("127.0.0.1", "add")

        # The banlist is persisted in the human-readable JSON file, the legacy
        # binary banlist.dat is not written anymore.
        assert os.path.isfile(self.banlist_path(self.nodes[1]))
        assert not os.path.isfile(self.legacy_banlist_path(self.nodes[1]))
        with open(self.banlist_path(self.nodes[1]), encoding="utf8") as f:
            banlist = json.load(f)
        assert "banned_nets" in banlist
        assert_equal(len(banlist["banned_nets"]), 1)
        assert_equal(banlist["banned_nets"][0]["address"], "127.0.0.1/32")
        assert_equal(banlist["banned_nets"][0]["version"], 1)
        assert_equal(banlist["banned_nets"][0]["ban_reason"], "manually added")

        # listbanned exposes the ban duration and the remaining time
        banned = self.nodes[1].listbanned()
        assert_equal(len(banned), 1)
        assert_equal(banned[0]["address"], "127.0.0.1/32")
        assert_equal(
            banned[0]["ban_duration"],
            banned[0]["banned_until"] - banned[0]["ban_created"],
        )
        assert_greater_than(banned[0]["time_remaining"], 0)
        assert_greater_than(banned[0]["ban_duration"], 0)

        # Node 0 should not be able to reconnect
        with self.nodes[1].assert_debug_log(
            expected_msgs=["dropped (banned)\n"], timeout=5
        ):
            self.restart_node(1, [])
            self.nodes[0].addnode("127.0.0.1:" + str(p2p_port(1)), "onetry")

        # The ban survived the restart, it was read back from banlist.json
        banned = self.nodes[1].listbanned()
        assert_equal(len(banned), 1)
        assert_equal(banned[0]["address"], "127.0.0.1/32")

        # However, node 0 should be able to reconnect if it has noban permission
        self.restart_node(1, ["-whitelist=127.0.0.1"])
        connect_nodes(self.nodes[0], 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert "noban" in peerinfo["permissions"]

        # If we remove the ban, Node 0 should be able to reconnect even without noban permission
        self.nodes[1].setban("127.0.0.1", "remove")
        self.restart_node(1, [])
        connect_nodes(self.nodes[0], 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert not "noban" in peerinfo["permissions"]

        # Onion addresses can be banned as well and survive a restart
        onion = "6hzph5hv6337r6p2.onion"
        self.nodes[1].setban(onion, "add")
        banned = self.nodes[1].listbanned()
        assert_equal(len(banned), 1)
        assert_equal(banned[0]["address"], onion + "/128")
        self.restart_node(1, [])
        banned = self.nodes[1].listbanned()
        assert_equal(len(banned), 1)
        assert_equal(banned[0]["address"], onion + "/128")
        self.nodes[1].setban(onion, "remove")
        assert_equal(len(self.nodes[1].listbanned()), 0)

        # ban_duration and time_remaining are deterministic with a mocked clock
        mocktime = 1600000000
        self.nodes[1].setmocktime(mocktime)
        self.nodes[1].setban("10.1.2.3", "add", 1000)
        banned = self.nodes[1].listbanned()
        assert_equal(len(banned), 1)
        assert_equal(banned[0]["address"], "10.1.2.3/32")
        assert_equal(banned[0]["ban_created"], mocktime)
        assert_equal(banned[0]["banned_until"], mocktime + 1000)
        assert_equal(banned[0]["ban_duration"], 1000)
        assert_equal(banned[0]["time_remaining"], 1000)
        self.nodes[1].setmocktime(mocktime + 500)
        banned = self.nodes[1].listbanned()
        assert_equal(banned[0]["time_remaining"], 500)
        self.nodes[1].setban("10.1.2.3", "remove")
        self.nodes[1].setmocktime(0)


if __name__ == "__main__":
    SetBanTests().main()
