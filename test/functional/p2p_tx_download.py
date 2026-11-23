#!/usr/bin/env python3
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test transaction download behavior with TxRequestTracker."""

import time

from test_framework.messages import (
    CInv,
    MSG_TX,
    MSG_TYPE_MASK,
    MSG_WTX,
    msg_inv,
    msg_notfound,
    msg_tx,
    msg_wtxidrelay,
    FromHex,
    CTransaction,
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

# Constants from net_processing.cpp
GETDATA_TX_INTERVAL = 60  # seconds
INBOUND_PEER_TX_DELAY = 2  # seconds
TXID_RELAY_DELAY = 2  # seconds
MAX_GETDATA_IN_FLIGHT = 100

NUM_INBOUND = 5
MAX_GETDATA_INBOUND_WAIT = GETDATA_TX_INTERVAL + INBOUND_PEER_TX_DELAY + TXID_RELAY_DELAY + 2


class TestP2PConn(P2PInterface):
    def __init__(self, wtxidrelay=True):
        super().__init__()
        self.send_wtxidrelay = wtxidrelay
        self.tx_getdata_count = 0
        self.getdata_hashes = []

    def peer_connect(self, *args, **kwargs):
        create_conn = super().peer_connect(*args, **kwargs)
        if self.send_wtxidrelay and self.on_connection_send_msg is not None:
            self.on_connection_send_msg.nVersion = 70040
        return create_conn

    def on_version(self, message):
        if self.send_wtxidrelay:
            self.send_message(msg_wtxidrelay())
        super().on_version(message)

    def on_getdata(self, message):
        for i in message.inv:
            if (i.type & MSG_TYPE_MASK) in (MSG_TX, MSG_WTX):
                self.tx_getdata_count += 1
                self.getdata_hashes.append(i.hash)


class TxDownloadTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def add_peer(self, node, wtxidrelay=True):
        return node.add_p2p_connection(TestP2PConn(wtxidrelay=wtxidrelay))

    def make_real_tx(self, node):
        """Create and sign a valid transaction spending a mature coinbase output."""
        utxos = node.listunspent()
        assert len(utxos) > 0, "Need at least one mature UTXO"
        utxo = utxos[0]
        addr = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: float(utxo["amount"]) - 0.001},
        )
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        tx = FromHex(CTransaction(), signed["hex"])
        tx.calc_sha256()
        return tx

    def current_mocktime(self, node):
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        return max(int(time.time()), tip_time) + 10

    def test_tx_requests_and_expiry_fallback(self):
        self.log.info("Test that all announcing peers receive a GETDATA on expiry, at most once per announcement")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        peers = [self.add_peer(node) for _ in range(NUM_INBOUND)]

        txhash = 0xdeadbeef01
        msg = msg_inv([CInv(t=MSG_WTX, h=txhash)])
        for p in peers:
            p.send_and_ping(msg)

        outstanding = list(range(len(peers)))
        while outstanding:
            mocktime += MAX_GETDATA_INBOUND_WAIT
            node.setmocktime(mocktime)
            wait_until(
                lambda: any(txhash in peers[i].getdata_hashes for i in outstanding),
                lock=mininode_lock,
            )
            newly_requested = [i for i in outstanding if txhash in peers[i].getdata_hashes]
            # At most one peer should be requested per expiry interval.
            assert_equal(len(newly_requested), 1)
            outstanding.remove(newly_requested[0])

        # Verify each peer received exactly one GETDATA for this txhash.
        for p in peers:
            assert_equal(p.getdata_hashes.count(txhash), 1)

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_notfound_fallback(self):
        self.log.info("Test that NOTFOUND triggers immediate fallback to another peer without 60s stall")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        p1 = self.add_peer(node)
        p2 = self.add_peer(node)

        txhash = 0xdeadbeef02
        inv = CInv(t=MSG_WTX, h=txhash)
        p1.send_and_ping(msg_inv([inv]))
        p2.send_and_ping(msg_inv([inv]))

        mocktime += INBOUND_PEER_TX_DELAY + 2
        node.setmocktime(mocktime)

        wait_until(
            lambda: txhash in p1.getdata_hashes or txhash in p2.getdata_hashes,
            lock=mininode_lock,
        )
        first, second = (p1, p2) if txhash in p1.getdata_hashes else (p2, p1)
        assert txhash not in second.getdata_hashes

        # Reply NOTFOUND from the first peer; the second peer must be asked immediately (without advancing mocktime by 60s).
        first.send_and_ping(msg_notfound([inv]))
        wait_until(lambda: txhash in second.getdata_hashes, lock=mininode_lock)

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_disconnect_fallback(self):
        self.log.info("Test that disconnecting the requested peer triggers immediate fallback")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        p1 = self.add_peer(node)
        p2 = self.add_peer(node)

        txhash = 0xdeadbeef03
        inv = CInv(t=MSG_WTX, h=txhash)
        p1.send_and_ping(msg_inv([inv]))
        p2.send_and_ping(msg_inv([inv]))

        mocktime += INBOUND_PEER_TX_DELAY + 2
        node.setmocktime(mocktime)

        wait_until(
            lambda: txhash in p1.getdata_hashes or txhash in p2.getdata_hashes,
            lock=mininode_lock,
        )
        first, second = (p1, p2) if txhash in p1.getdata_hashes else (p2, p1)
        assert txhash not in second.getdata_hashes

        # Disconnect the first peer; the second peer must be asked immediately.
        first.peer_disconnect()
        wait_until(lambda: txhash in second.getdata_hashes, lock=mininode_lock)

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_in_flight_max(self):
        self.log.info("Test that per-peer in-flight requests never exceed MAX_GETDATA_IN_FLIGHT (100)")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        peer = self.add_peer(node)

        invs = [CInv(t=MSG_WTX, h=0x10000 + i) for i in range(MAX_GETDATA_IN_FLIGHT + 20)]
        peer.send_and_ping(msg_inv(invs))

        mocktime += INBOUND_PEER_TX_DELAY + 2
        node.setmocktime(mocktime)

        wait_until(lambda: peer.tx_getdata_count >= MAX_GETDATA_IN_FLIGHT, lock=mininode_lock)
        peer.sync_with_ping()
        assert_equal(peer.tx_getdata_count, MAX_GETDATA_IN_FLIGHT)

        # Reply NOTFOUND for the 100 in-flight requests; the remaining 20 must now be requested.
        with mininode_lock:
            requested_hashes = list(peer.getdata_hashes)
        peer.send_and_ping(msg_notfound([CInv(t=MSG_WTX, h=h) for h in requested_hashes]))
        wait_until(lambda: peer.tx_getdata_count == MAX_GETDATA_IN_FLIGHT + 20, lock=mininode_lock)

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_single_download_across_peers(self):
        self.log.info("Test that a valid tx announced by multiple peers is downloaded exactly once")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        tx = self.make_real_tx(node)
        wtxid = tx.calc_sha256(with_witness=True)

        peers = [self.add_peer(node) for _ in range(3)]
        inv = CInv(t=MSG_WTX, h=wtxid)
        for p in peers:
            p.send_and_ping(msg_inv([inv]))

        mocktime += INBOUND_PEER_TX_DELAY + 2
        node.setmocktime(mocktime)

        wait_until(
            lambda: any(wtxid in p.getdata_hashes for p in peers),
            lock=mininode_lock,
        )
        requesters = [p for p in peers if wtxid in p.getdata_hashes]
        assert_equal(len(requesters), 1)

        # Serve the transaction from the selected peer.
        requesters[0].send_and_ping(msg_tx(tx))
        wait_until(lambda: tx.hash in node.getrawmempool())

        # Advance mocktime well past GETDATA_TX_INTERVAL; no other peer should be asked.
        node.setmocktime(mocktime + MAX_GETDATA_INBOUND_WAIT)
        for p in peers:
            p.sync_with_ping()
        total_getdatas = sum(p.getdata_hashes.count(wtxid) for p in peers)
        assert_equal(total_getdatas, 1)

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_wtxid_preferred_over_txid(self):
        self.log.info("Test that wtxid announcement is requested before txid announcement due to TXID_RELAY_DELAY")
        node = self.nodes[0]
        mocktime = self.current_mocktime(node)
        node.setmocktime(mocktime)
        tx = self.make_real_tx(node)
        txid = tx.sha256
        wtxid = tx.calc_sha256(with_witness=True)

        p_wtx = self.add_peer(node, wtxidrelay=True)
        p_tx = self.add_peer(node, wtxidrelay=False)

        p_tx.send_and_ping(msg_inv([CInv(t=MSG_TX, h=txid)]))
        p_wtx.send_and_ping(msg_inv([CInv(t=MSG_WTX, h=wtxid)]))

        # Advance by INBOUND_PEER_TX_DELAY (2s) only: wtxid is ready, txid still delayed by TXID_RELAY_DELAY (2s).
        mocktime += INBOUND_PEER_TX_DELAY + 1
        node.setmocktime(mocktime)

        wait_until(lambda: wtxid in p_wtx.getdata_hashes, lock=mininode_lock)
        p_tx.sync_with_ping()
        assert txid not in p_tx.getdata_hashes

        # Serve tx from p_wtx -> accepted into mempool -> txid announcement on p_tx is forgotten.
        p_wtx.send_and_ping(msg_tx(tx))
        wait_until(lambda: tx.hash in node.getrawmempool())
        node.setmocktime(mocktime + MAX_GETDATA_INBOUND_WAIT)
        p_tx.sync_with_ping()
        assert txid not in p_tx.getdata_hashes

        node.setmocktime(0)
        node.disconnect_p2ps()

    def run_test(self):
        node = self.nodes[0]
        # Exit IBD and mature coinbase outputs for real-transaction tests.
        node.generate(105)

        self.test_tx_requests_and_expiry_fallback()
        self.test_notfound_fallback()
        self.test_disconnect_fallback()
        self.test_in_flight_max()
        self.test_single_download_across_peers()
        self.test_wtxid_preferred_over_txid()


if __name__ == '__main__':
    TxDownloadTest().main()
