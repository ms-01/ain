#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the peer anchoring (block-relay anchor peers) feature.

Peer anchoring is a connectivity/priority concept only: anchor peers are
prioritized for the limited block-relay-only outbound connections. Blocks and
headers received from an anchor peer are validated exactly like blocks and
headers received from any other peer.

The test covers:

  * an anchored peer is connected as a block-relay-only connection and the
    connection is flagged as an anchor peer,
  * blocks delivered by the anchor peer are accepted by the anchoring node,
  * the non-anchored path still validates normally,
  * the feature is inert without block-relay-only outbound slots,
  * the anchor peer set is managed via RPC, is capped and is persisted
    (write-through) in the anchors file,
  * a corrupted anchors file is ignored and the node keeps running,
  * an anchor peer that is not reachable at startup is retried: it stays in
    the configured set and the anchor connection is established as soon as the
    anchor peer becomes reachable,
  * a banned anchor peer is skipped: the node stays responsive, does not spin
    and shuts down cleanly,
  * an anchor peer that is already connected (as a manual peer) is skipped:
    the node stays responsive and the behaviour is deterministic.
"""

import os
import time

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    p2p_port,
    wait_until,
)


class AnchorPeersTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 8
        self.setup_clean_chain = True

    def setup_network(self):
        # The nodes are started individually: the anchoring node must not be
        # connected to the block source by any other means, otherwise the
        # anchor connection could not be established.
        self.add_nodes(8)
        self.start_node(0)
        self.start_node(
            1,
            extra_args=[
                "-peeranchoring=1",
                f"-anchorpeer=127.0.0.1:{p2p_port(0)}",
                "-blockrelayconnections=1",
            ],
        )
        self.start_node(2)
        # Peer anchoring without block-relay-only slots is inert.
        self.start_node(3, extra_args=["-peeranchoring=1", "-blockrelayconnections=0"])
        # Import the deterministic coinbase privkeys for the nodes that are
        # already running (the nodes 4 and 5 are started later in the test).
        for node in self.nodes[:4]:
            node.importprivkey(
                privkey=node.get_genesis_keys().ownerPrivKey,
                label="coinbase",
                rescan=True,
            )
            node.importprivkey(
                privkey=node.get_genesis_keys().operatorPrivKey,
                label="coinbase",
                rescan=True,
            )

    def anchors_path(self, node):
        return os.path.join(node.datadir, self.chain, "net", "anchors.dat")

    def anchor_peers(self, node):
        return [entry["address"] for entry in node.listanchorpeers()]

    def read_log(self, node):
        with open(
            os.path.join(node.datadir, self.chain, "debug.log"), encoding="utf8"
        ) as log:
            return log.read()

    def cpu_time(self, node):
        """Return the CPU time (in seconds) used by the node process."""
        try:
            with open(f"/proc/{node.process.pid}/stat", encoding="utf8") as f:
                fields = f.read().split()
            ticks = os.sysconf("SC_CLK_TCK")
            return (int(fields[13]) + int(fields[14])) / ticks
        except (OSError, IndexError, ValueError):
            return None

    def assert_no_cpu_spin(self, node):
        """Assert that the node does not burn a full core."""
        before = self.cpu_time(node)
        if before is None:
            return
        time.sleep(5)
        after = self.cpu_time(node)
        if after is None:
            return
        assert after - before < 1.0, "the node is burning CPU"

    def run_test(self):
        node0, node1, node2, node3 = self.nodes[:4]

        # The anchoring node connects to the block source as a block-relay-only
        # connection and flags the peer as an anchor peer.
        def anchor_connected():
            for peer in node1.getpeerinfo():
                if (
                    peer["connection_type"] == "block-relay-only"
                    and peer["is_anchor_peer"]
                    and peer["addr"] == f"127.0.0.1:{p2p_port(0)}"
                ):
                    return True
            return False

        wait_until(anchor_connected, timeout=30)
        self.log.info("Anchor connection established")

        # The configured anchor peer is reported by the RPC interface.
        assert_equal(self.anchor_peers(node1), [f"127.0.0.1:{p2p_port(0)}"])

        # A block delivered by the anchor peer is accepted by the anchoring
        # node (normal validation path).
        block_hash = node0.generate(1)[0]
        wait_until(lambda: node1.getbestblockhash() == block_hash, timeout=30)
        assert_equal(node1.getbestblockhash(), block_hash)

        # The non-anchored path still validates normally: node2 connects to the
        # block source as a regular (manual) peer.
        node2.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        wait_until(lambda: len(node2.getpeerinfo()) > 0, timeout=30)
        block_hash = node0.generate(1)[0]
        wait_until(lambda: node2.getbestblockhash() == block_hash, timeout=30)
        assert_equal(node2.getbestblockhash(), block_hash)

        # Without the anchoring flags no anchor connection is opened and no
        # peer is flagged as an anchor peer.
        for peer in node2.getpeerinfo():
            assert not peer["is_anchor_peer"]
            assert peer["connection_type"] != "block-relay-only"

        # Peer anchoring is inert without block-relay-only outbound slots.
        assert_equal(node3.getpeerinfo(), [])
        assert "Peer anchoring is enabled" in self.read_log(node3)

        # RPC round trip: list/add/removeanchorpeer.
        assert_raises_rpc_error(
            -8,
            "Invalid anchor peer address",
            node1.addanchorpeer,
            "not-an-address",
        )
        node1.addanchorpeer("127.0.0.3:12345")
        assert_equal(
            sorted(self.anchor_peers(node1)),
            sorted([f"127.0.0.1:{p2p_port(0)}", "127.0.0.3:12345"]),
        )
        # The number of anchor peers is capped.
        assert_raises_rpc_error(
            -1,
            "the maximum of 2 anchor peers is reached",
            node1.addanchorpeer,
            "127.0.0.4:12346",
        )
        assert_equal(len(self.anchor_peers(node1)), 2)
        # Removing an anchor peer works, removing an unknown one does not.
        assert_equal(node1.removeanchorpeer("127.0.0.3:12345"), True)
        assert_equal(self.anchor_peers(node1), [f"127.0.0.1:{p2p_port(0)}"])
        assert_equal(node1.removeanchorpeer("127.0.0.3:12345"), False)

        # The anchor peer set is written through immediately.
        node1.addanchorpeer("127.0.0.3:12345")
        anchors_file = self.anchors_path(node1)
        assert os.path.exists(anchors_file)
        with open(anchors_file, encoding="utf8") as f:
            content = f.read()
        assert f"127.0.0.1:{p2p_port(0)}" in content
        assert "127.0.0.3:12345" in content

        # The anchor peer set survives a restart of the node.
        self.restart_node(
            1,
            extra_args=[
                "-peeranchoring=1",
                f"-anchorpeer=127.0.0.1:{p2p_port(0)}",
                "-blockrelayconnections=1",
            ],
        )
        assert_equal(
            sorted(self.anchor_peers(node1)),
            sorted([f"127.0.0.1:{p2p_port(0)}", "127.0.0.3:12345"]),
        )
        wait_until(anchor_connected, timeout=30)

        # A corrupted anchors file is ignored: the node starts normally and no
        # anchor peer is configured.
        self.stop_node(1)
        with open(anchors_file, "wb") as f:
            f.write(b"\x00\x01\x02garbage\xff")
        self.start_node(1)
        assert_equal(self.anchor_peers(node1), [])
        assert_equal(node1.getblockcount(), node0.getblockcount())

        # A configured anchor peer that is not reachable at startup is not
        # dropped: it stays in the configured set and is retried, so that the
        # anchor connection is established as soon as the anchor peer becomes
        # reachable.
        late_anchor = f"127.0.0.1:{p2p_port(5)}"
        self.start_node(
            4,
            extra_args=[
                "-peeranchoring=1",
                f"-anchorpeer={late_anchor}",
                "-blockrelayconnections=1",
            ],
        )
        node4 = self.nodes[4]

        # The anchor peer is not running yet: the connection attempt fails, but
        # the anchor peer stays configured and the failure is observable.
        wait_until(lambda: "is not reachable" in self.read_log(node4), timeout=60)
        assert_equal(self.anchor_peers(node4), [late_anchor])
        assert not any(peer["is_anchor_peer"] for peer in node4.getpeerinfo())

        # The anchor peer becomes reachable: the anchor connection is
        # established after the retry backoff.
        self.start_node(5)

        def late_anchor_connected():
            for peer in node4.getpeerinfo():
                if (
                    peer["connection_type"] == "block-relay-only"
                    and peer["is_anchor_peer"]
                    and peer["addr"] == late_anchor
                ):
                    return True
            return False

        wait_until(late_anchor_connected, timeout=180)
        assert_equal(self.anchor_peers(node4), [late_anchor])

        # A banned anchor peer is skipped: the node stays responsive, does not
        # spin and the anchor peer stays configured.
        banned_node = self.nodes[6]
        self.start_node(
            6,
            extra_args=[
                "-peeranchoring=1",
                "-blockrelayconnections=1",
            ],
        )
        # 127.0.0.2 is not used by any other node of the test, banning it does
        # not affect the other connections of the node.
        banned_node.setban("127.0.0.2", "add")
        banned_node.addanchorpeer("127.0.0.2:12345")
        wait_until(
            lambda: "is currently not used: banned" in self.read_log(banned_node),
            timeout=60,
        )
        assert_equal(self.anchor_peers(banned_node), ["127.0.0.2:12345"])
        # The node is still responsive and does not burn a core.
        assert banned_node.getpeerinfo() is not None
        self.assert_no_cpu_spin(banned_node)
        # The node still opens connections to other peers.
        banned_node.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        wait_until(lambda: len(banned_node.getpeerinfo()) > 0, timeout=60)

        # An anchor peer that is already connected as a manual peer is skipped:
        # the node does not hang and the behaviour is deterministic.
        connected_node = self.nodes[7]
        self.start_node(
            7,
            extra_args=[
                "-peeranchoring=1",
                "-blockrelayconnections=1",
            ],
        )
        connected_node.addnode(f"127.0.0.1:{p2p_port(0)}", "add")
        wait_until(lambda: len(connected_node.getpeerinfo()) > 0, timeout=60)
        connected_node.addanchorpeer(f"127.0.0.1:{p2p_port(0)}")
        wait_until(
            lambda: "is currently not used: already connected"
            in self.read_log(connected_node),
            timeout=60,
        )
        # The manual connection is not turned into an anchor connection.
        for peer in connected_node.getpeerinfo():
            assert not peer["is_anchor_peer"]
        assert_equal(self.anchor_peers(connected_node), [f"127.0.0.1:{p2p_port(0)}"])
        # The node is still responsive and does not burn a core.
        assert connected_node.getpeerinfo() is not None
        self.assert_no_cpu_spin(connected_node)


if __name__ == "__main__":
    AnchorPeersTest().main()
