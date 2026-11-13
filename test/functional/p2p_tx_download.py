#!/usr/bin/env python3
# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""
Test transaction download behavior
"""

from test_framework.messages import (
    CInv,
    CTransaction,
    FromHex,
    MSG_TX,
    MSG_TYPE_MASK,
    MSG_WITNESS_FLAG,
    MSG_WTX,
    msg_getdata,
    msg_inv,
    msg_mempool,
    msg_notfound,
    msg_verack,
    msg_witness_tx,
    msg_wtxidrelay,
)
from test_framework.mininode import (
    P2PInterface,
    mininode_lock,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    wait_until,
)
from test_framework.address import ADDRESS_BCRT1_UNSPENDABLE

import copy
import os
import time


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.tx_getdata_count = 0

    def on_getdata(self, message):
        for i in message.inv:
            if i.type & MSG_TYPE_MASK == MSG_TX:
                self.tx_getdata_count += 1


class WtxidP2PConn(P2PInterface):
    def __init__(self, version=70040, send_wtxidrelay=True, auto_request_inv=True):
        super().__init__()
        self.peer_version = version
        self.send_wtxidrelay = send_wtxidrelay
        self.auto_request_inv = auto_request_inv
        self.received_wtxidrelay = False
        self.received_verack = False
        self.wtxidrelay_before_verack = False
        self.invs = []
        self.getdatas = []
        self.txs = []

    def peer_connect(self, *args, **kwargs):
        create_conn = super().peer_connect(*args, **kwargs)
        if self.on_connection_send_msg is not None:
            self.on_connection_send_msg.nVersion = self.peer_version
        return create_conn

    def on_wtxidrelay(self, message):
        self.received_wtxidrelay = True
        if not self.received_verack:
            self.wtxidrelay_before_verack = True

    def on_verack(self, message):
        self.received_verack = True

    def on_version(self, message):
        if self.send_wtxidrelay:
            self.send_message(msg_wtxidrelay())
        self.send_message(msg_verack())
        self.nServices = message.nServices

    def on_inv(self, message):
        self.invs.extend(message.inv)
        if self.auto_request_inv:
            want = msg_getdata()
            for i in message.inv:
                if i.type in (MSG_TX, MSG_WTX):
                    if i.type == MSG_TX:
                        want.inv.append(CInv(MSG_TX | MSG_WITNESS_FLAG, i.hash))
                    else:
                        want.inv.append(i)
            if want.inv:
                self.send_message(want)

    def on_getdata(self, message):
        self.getdatas.extend(message.inv)

    def on_tx(self, message):
        self.txs.append(message.tx)


# Constants from net_processing
GETDATA_TX_INTERVAL = 60  # seconds
MAX_GETDATA_RANDOM_DELAY = 2  # seconds
INBOUND_PEER_TX_DELAY = 2  # seconds
MAX_GETDATA_IN_FLIGHT = 100
TX_EXPIRY_INTERVAL = GETDATA_TX_INTERVAL * 10

# Python test constants
NUM_INBOUND = 10
MAX_GETDATA_INBOUND_WAIT = (
    GETDATA_TX_INTERVAL + MAX_GETDATA_RANDOM_DELAY + INBOUND_PEER_TX_DELAY
)


class TxDownloadTest(DefiTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 2
        self.extra_args = [["-peerbloomfilters=1"], []]

    def test_tx_requests(self):
        self.log.info(
            "Test that we request transactions from all our peers, eventually"
        )

        txid = 0xDEADBEEF

        self.log.info("Announce the txid from each incoming peer to node 0")
        msg = msg_inv([CInv(t=1, h=txid)])
        for p in self.nodes[0].p2ps:
            p.send_message(msg)
            p.sync_with_ping()

        outstanding_peer_index = [i for i in range(len(self.nodes[0].p2ps))]

        def getdata_found(peer_index):
            p = self.nodes[0].p2ps[peer_index]
            with mininode_lock:
                return (
                    p.last_message.get("getdata")
                    and p.last_message["getdata"].inv[-1].hash == txid
                )

        node_0_mocktime = int(time.time())
        while outstanding_peer_index:
            node_0_mocktime += MAX_GETDATA_INBOUND_WAIT
            self.nodes[0].setmocktime(node_0_mocktime)
            wait_until(lambda: any(getdata_found(i) for i in outstanding_peer_index))
            for i in outstanding_peer_index:
                if getdata_found(i):
                    outstanding_peer_index.remove(i)

        self.nodes[0].setmocktime(0)
        self.log.info("All outstanding peers received a getdata")

    def test_inv_block(self):
        self.log.info("Generate a transaction on node 0")
        tx = self.nodes[0].createrawtransaction(
            inputs=[
                {  # coinbase
                    "txid": self.nodes[0].getblock(self.nodes[0].getblockhash(1))["tx"][
                        0
                    ],
                    "vout": 0,
                }
            ],
            outputs={ADDRESS_BCRT1_UNSPENDABLE: 50 - 0.00025},
        )
        tx = self.nodes[0].signrawtransactionwithkey(
            hexstring=tx,
            privkeys=[self.nodes[0].get_genesis_keys().ownerPrivKey],
        )["hex"]
        ctx = FromHex(CTransaction(), tx)
        txid = int(ctx.rehash(), 16)

        self.log.info(
            "Announce the transaction to all nodes from all {} incoming peers, but never send it".format(
                NUM_INBOUND
            )
        )
        msg = msg_inv([CInv(t=1, h=txid)])
        for p in self.peers:
            p.send_message(msg)
            p.sync_with_ping()

        self.log.info("Put the tx in node 0's mempool")
        self.nodes[0].sendrawtransaction(tx)

        # Since node 1 is connected outbound to an honest peer (node 0), it
        # should get the tx within a timeout. (Assuming that node 0
        # announced the tx within the timeout)
        # The timeout is the sum of
        # * the worst case until the tx is first requested from an inbound
        #   peer, plus
        # * the first time it is re-requested from the outbound peer, plus
        # * 2 seconds to avoid races
        timeout = (
            2
            + (MAX_GETDATA_RANDOM_DELAY + INBOUND_PEER_TX_DELAY)
            + (GETDATA_TX_INTERVAL + MAX_GETDATA_RANDOM_DELAY)
        )
        self.log.info(
            "Tx should be received at node 1 after {} seconds".format(timeout)
        )
        self.sync_mempools(timeout=timeout)

    def test_in_flight_max(self):
        # need cause mocktime set in start_node() (if not None), and in setup_nodes()
        # TODO: refactor mocktime logic
        self.nodes[0].setmocktime(0)

        self.log.info(
            "Test that we don't request more than {} transactions from any peer, every {} minutes".format(
                MAX_GETDATA_IN_FLIGHT, TX_EXPIRY_INTERVAL / 60
            )
        )
        txids = [i for i in range(MAX_GETDATA_IN_FLIGHT + 2)]

        p = self.nodes[0].p2ps[0]

        with mininode_lock:
            p.tx_getdata_count = 0

        p.send_message(msg_inv([CInv(t=1, h=i) for i in txids]))
        wait_until(
            lambda: p.tx_getdata_count >= MAX_GETDATA_IN_FLIGHT, lock=mininode_lock
        )
        with mininode_lock:
            assert_equal(p.tx_getdata_count, MAX_GETDATA_IN_FLIGHT)

        self.log.info(
            "Now check that if we send a NOTFOUND for a transaction, we'll get one more request"
        )
        p.send_message(msg_notfound(vec=[CInv(t=1, h=txids[0])]))
        wait_until(
            lambda: p.tx_getdata_count >= MAX_GETDATA_IN_FLIGHT + 1,
            timeout=10,
            lock=mininode_lock,
        )
        with mininode_lock:
            assert_equal(p.tx_getdata_count, MAX_GETDATA_IN_FLIGHT + 1)

        WAIT_TIME = TX_EXPIRY_INTERVAL // 2 + TX_EXPIRY_INTERVAL
        self.log.info(
            "if we wait about {} minutes, we should eventually get more requests".format(
                WAIT_TIME / 60
            )
        )
        self.nodes[0].setmocktime(int(time.time() + WAIT_TIME))
        wait_until(lambda: p.tx_getdata_count == MAX_GETDATA_IN_FLIGHT + 2)
        self.nodes[0].setmocktime(0)

    def _create_segwit_tx(self, node):
        addr = node.getnewaddress("", "bech32")
        node.sendtoaddress(addr, 1.0)
        node.generate(1)
        utxo = next(
            u for u in node.listunspent() if u["address"] == addr and u["amount"] == 1
        )
        raw = node.createrawtransaction(
            inputs=[{"txid": utxo["txid"], "vout": utxo["vout"]}],
            outputs={ADDRESS_BCRT1_UNSPENDABLE: 0.999},
        )
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        ctx = FromHex(CTransaction(), signed["hex"])
        node.setmocktime(0)
        return signed["hex"], ctx

    def test_wtxidrelay_handshake(self):
        self.log.info("Test BIP 339 wtxidrelay handshake negotiation")
        node = self.nodes[0]

        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        legacy_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(legacy_peer.received_wtxidrelay, False)
            assert_equal(legacy_peer.message_count["wtxidrelay"], 0)

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(wtxid_peer.received_wtxidrelay, True)
            assert_equal(wtxid_peer.wtxidrelay_before_verack, True)

        self.log.info("Test disconnect when wtxidrelay is sent after verack")
        late_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=False)
        )
        late_peer.sync_with_ping()
        late_peer.send_message(msg_wtxidrelay())
        late_peer.wait_for_disconnect()

        node.disconnect_p2ps()

    def test_wtxid_relay_and_mempool(self):
        self.log.info("Test MSG_WTX relay, mempool lookup, MEMPOOL cmd, and NOTFOUND")
        node = self.nodes[0]
        raw_hex, ctx = self._create_segwit_tx(node)
        txid = int(ctx.rehash(), 16)
        wtxid = ctx.calc_sha256(True)
        assert txid != wtxid

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        wtxid_peer.sync_with_ping()
        legacy_peer.sync_with_ping()

        # Broadcast tx from node 0: wtxid_peer should get MSG_WTX, legacy_peer MSG_TX
        node.sendrawtransaction(raw_hex)
        wait_until(
            lambda: any(i.type == MSG_WTX and i.hash == wtxid for i in wtxid_peer.invs),
            lock=mininode_lock,
        )
        wait_until(
            lambda: any(i.type == MSG_TX and i.hash == txid for i in legacy_peer.invs),
            lock=mininode_lock,
        )
        wait_until(
            lambda: any(t.calc_sha256(True) == wtxid for t in wtxid_peer.txs),
            lock=mininode_lock,
        )
        wait_until(
            lambda: any(
                int(t.rehash(), 16) == txid for t in legacy_peer.txs
            ),
            lock=mininode_lock,
        )

        # Re-announcing MSG_WTX for an in-mempool tx must NOT trigger GETDATA
        with mininode_lock:
            wtxid_peer.getdatas.clear()
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, wtxid)]))
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(wtxid_peer.getdatas), 0)

        # BIP 35 MEMPOOL request to wtxid_peer should reply with MSG_WTX
        with mininode_lock:
            wtxid_peer.invs.clear()
        wtxid_peer.send_message(msg_mempool())
        wait_until(
            lambda: any(i.type == MSG_WTX and i.hash == wtxid for i in wtxid_peer.invs),
            lock=mininode_lock,
        )

        # NOTFOUND for MSG_WTX clears in-flight state so a later inv can be requested again
        fake_wtxid = 0xCAFEBABE
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, fake_wtxid)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == fake_wtxid for i in wtxid_peer.getdatas
            ),
            lock=mininode_lock,
        )
        wtxid_peer.send_message(msg_notfound([CInv(MSG_WTX, fake_wtxid)]))
        wtxid_peer.sync_with_ping()

        with mininode_lock:
            wtxid_peer.getdatas.clear()
        node_mocktime = int(time.time()) + GETDATA_TX_INTERVAL + MAX_GETDATA_RANDOM_DELAY + 5
        node.setmocktime(node_mocktime)
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, fake_wtxid)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == fake_wtxid for i in wtxid_peer.getdatas
            ),
            lock=mininode_lock,
        )
        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_witness_malleation_not_blackholed(self):
        self.log.info(
            "Test that a rejected witness-malleated tx does not blackhole the valid wtxid"
        )
        node = self.nodes[0]
        _, ctx_valid = self._create_segwit_tx(node)
        txid = int(ctx_valid.rehash(), 16)
        wtxid_valid = ctx_valid.calc_sha256(True)

        # Create an invalid witness-malleated variant with identical txid
        ctx_malleated = copy.deepcopy(ctx_valid)
        ctx_malleated.wit.vtxinwit[0].scriptWitness.stack[0] = b"\x00" * 71
        assert_equal(int(ctx_malleated.rehash(), 16), txid)
        wtxid_malleated = ctx_malleated.calc_sha256(True)
        assert wtxid_malleated != wtxid_valid

        attacker = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        honest = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        attacker.sync_with_ping()
        honest.sync_with_ping()

        # 1. Attacker announces and sends the witness-malleated invalid tx
        attacker.send_message(msg_inv([CInv(MSG_WTX, wtxid_malleated)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == wtxid_malleated
                for i in attacker.getdatas
            ),
            lock=mininode_lock,
        )
        attacker.send_message(msg_witness_tx(ctx_malleated))
        attacker.sync_with_ping()
        assert ctx_valid.rehash() not in node.getrawmempool()

        # 2. Re-announcing the same invalid wtxid must be suppressed by recentRejects
        with mininode_lock:
            attacker.getdatas.clear()
        attacker.send_message(msg_inv([CInv(MSG_WTX, wtxid_malleated)]))
        attacker.sync_with_ping()
        with mininode_lock:
            assert_equal(len(attacker.getdatas), 0)

        # 3. Honest peer announces and sends the valid wtxid (same txid!) -> must NOT be blackholed
        honest.send_message(msg_inv([CInv(MSG_WTX, wtxid_valid)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == wtxid_valid for i in honest.getdatas
            ),
            lock=mininode_lock,
        )
        honest.send_message(msg_witness_tx(ctx_valid))
        honest.sync_with_ping()
        assert ctx_valid.rehash() in node.getrawmempool()

        node.disconnect_p2ps()

    def test_maprelay_cross_relay_and_inv_filtering(self):
        self.log.info(
            "Test mapRelay dual-key serving (txid -> wtxid) and INV filtering against m_wtxid_relay"
        )
        node = self.nodes[0]
        raw_hex, ctx = self._create_segwit_tx(node)
        txid = int(ctx.rehash(), 16)
        wtxid = ctx.calc_sha256(True)
        assert txid != wtxid

        # 1. Relay tx while ONLY a legacy peer is connected (populates mapRelay)
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        legacy_peer.sync_with_ping()
        node.sendrawtransaction(raw_hex)
        wait_until(
            lambda: any(i.type == MSG_TX and i.hash == txid for i in legacy_peer.invs),
            lock=mininode_lock,
        )

        # 2. Connect a wtxid peer AFTER the relay (timeLastMempoolReq == 0) and request MSG_WTX
        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        wtxid_peer.sync_with_ping()
        wtxid_peer.send_message(msg_getdata([CInv(MSG_WTX, wtxid)]))
        wait_until(
            lambda: any(t.calc_sha256(True) == wtxid for t in wtxid_peer.txs),
            lock=mininode_lock,
        )

        # 3. INV type incompatible with peer's wtxidrelay negotiation must be ignored
        with mininode_lock:
            legacy_peer.getdatas.clear()
            wtxid_peer.getdatas.clear()
        legacy_peer.send_message(msg_inv([CInv(MSG_WTX, 0xDEADBEEF)]))
        wtxid_peer.send_message(msg_inv([CInv(MSG_TX, 0xDEADCAFE)]))
        legacy_peer.sync_with_ping()
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(legacy_peer.getdatas), 0)
            assert_equal(len(wtxid_peer.getdatas), 0)

        node.disconnect_p2ps()

    def test_legacy_peer_recent_rejects(self):
        self.log.info(
            "Test that a rejected SegWit tx sent by a legacy peer hits recentRejects (keyed by wtxid)"
        )
        node = self.nodes[0]
        _, ctx_valid = self._create_segwit_tx(node)
        ctx_malleated = copy.deepcopy(ctx_valid)
        ctx_malleated.wit.vtxinwit[0].scriptWitness.stack[0] = b"\x00" * 71
        wtxid_malleated = ctx_malleated.calc_sha256(True)

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        wtxid_peer.sync_with_ping()
        legacy_peer.sync_with_ping()

        # 1. Reject via wtxid_peer -> populates recentRejects with wtxid_malleated
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, wtxid_malleated)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == wtxid_malleated
                for i in wtxid_peer.getdatas
            ),
            lock=mininode_lock,
        )
        with node.assert_debug_log(expected_msgs=["mandatory-script-verify-flag"]):
            wtxid_peer.send_message(msg_witness_tx(ctx_malleated))
            wtxid_peer.sync_with_ping()

        # 2. Legacy peer sends the exact same rejected tx -> must hit recentRejects without re-validating
        debug_log = os.path.join(node.datadir, node.chain, "debug.log")
        with open(debug_log, encoding="utf-8") as dl:
            dl.seek(0, 2)
            prev_size = dl.tell()
        legacy_peer.send_message(msg_witness_tx(ctx_malleated))
        legacy_peer.sync_with_ping()
        with open(debug_log, encoding="utf-8") as dl:
            dl.seek(prev_size)
            new_log = dl.read()
        assert "mandatory-script-verify-flag" not in new_log

        node.disconnect_p2ps()

    def run_test(self):
        for node in self.nodes:
            node.setmocktime(0)

        # Setup the p2p connections
        self.peers = []
        for node in self.nodes:
            for i in range(NUM_INBOUND):
                self.peers.append(node.add_p2p_connection(TestP2PConn()))

        self.log.info(
            "Nodes are setup with {} incoming connections each".format(NUM_INBOUND)
        )

        # Test the in-flight max first, because we want no transactions in
        # flight ahead of this test.
        self.test_in_flight_max()

        self.test_inv_block()

        self.test_tx_requests()

        self.nodes[0].disconnect_p2ps()
        self.nodes[1].disconnect_p2ps()

        self.test_wtxidrelay_handshake()
        self.test_wtxid_relay_and_mempool()
        self.test_witness_malleation_not_blackholed()
        self.test_maprelay_cross_relay_and_inv_filtering()
        self.test_legacy_peer_recent_rejects()


if __name__ == "__main__":
    TxDownloadTest().main()
