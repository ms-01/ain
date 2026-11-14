#!/usr/bin/env python3
# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""
Test transaction download behavior
"""

from test_framework.messages import (
    CInv,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
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
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
)
from test_framework.mininode import (
    P2PInterface,
    mininode_lock,
)
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    disconnect_nodes,
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
TXID_RELAY_DELAY = 2  # seconds
MAX_GETDATA_IN_FLIGHT = 100
TX_EXPIRY_INTERVAL = GETDATA_TX_INTERVAL * 10

# Python test constants
NUM_INBOUND = 10
MAX_GETDATA_INBOUND_WAIT = (
    GETDATA_TX_INTERVAL
    + MAX_GETDATA_RANDOM_DELAY
    + INBOUND_PEER_TX_DELAY
    + TXID_RELAY_DELAY
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

    def test_recent_confirmed_wtxid_suppressed(self):
        self.log.info(
            "Test that MSG_WTX INVs for recently confirmed transactions are suppressed"
        )
        node = self.nodes[0]
        raw_hex, ctx = self._create_segwit_tx(node)
        wtxid = ctx.calc_sha256(True)
        node.sendrawtransaction(raw_hex)
        node.generate(1)

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            wtxid_peer.getdatas.clear()
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, wtxid)]))
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(wtxid_peer.getdatas), 0)

        node.disconnect_p2ps()

    def test_inputs_not_standard_rejects_txid(self):
        self.log.info(
            "Test that TX_INPUTS_NOT_STANDARD inserts both wtxid and txid into recentRejects"
        )
        node = self.nodes[0]
        redeem_script = CScript([OP_CHECKSIG] * 16)
        p2sh_addr = node.decodescript(redeem_script.hex())["p2sh"]
        p2sh_txid = node.sendtoaddress(p2sh_addr, 1.0)
        p2sh_raw = node.getrawtransaction(p2sh_txid, True)
        node.generate(1)
        p2sh_vout = next(
            v["n"]
            for v in p2sh_raw["vout"]
            if p2sh_addr in v["scriptPubKey"].get("addresses", [])
        )

        _, ctx = self._create_segwit_tx(node)
        ctx.vin.append(
            CTxIn(COutPoint(int(p2sh_txid, 16), p2sh_vout), CScript([redeem_script]))
        )
        ctx.wit.vtxinwit.append(CTxInWitness())
        ctx.rehash()
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

        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, wtxid)]))
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == wtxid for i in wtxid_peer.getdatas
            ),
            lock=mininode_lock,
        )
        with node.assert_debug_log(expected_msgs=["bad-txns-nonstandard-inputs"]):
            wtxid_peer.send_message(msg_witness_tx(ctx))
            wtxid_peer.sync_with_ping()

        # Legacy peer announcing by txid must now be suppressed by recentRejects
        with mininode_lock:
            legacy_peer.getdatas.clear()
        legacy_peer.send_message(msg_inv([CInv(MSG_TX, txid)]))
        legacy_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(legacy_peer.getdatas), 0)

        node.disconnect_p2ps()

    def test_orphan_wtxid_already_have(self):
        self.log.info(
            "Test that AlreadyHave(MSG_WTX) hits g_orphans_by_wtxid and cleans up on EraseOrphansFor"
        )
        node = self.nodes[0]

        ctx = CTransaction()
        ctx.vin.append(CTxIn(COutPoint(0x123456789ABCDEF, 0), b""))
        ctx.vout.append(CTxOut(1000, CScript([0, b"\x00" * 20])))
        ctx.wit.vtxinwit.append(CTxInWitness())
        ctx.wit.vtxinwit[0].scriptWitness.stack = [b"dummy-witness"]
        ctx.rehash()
        txid = int(ctx.rehash(), 16)
        wtxid = ctx.calc_sha256(True)
        assert txid != wtxid

        peer1 = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        peer2 = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        peer1.sync_with_ping()
        peer2.sync_with_ping()
        legacy_peer.sync_with_ping()

        with node.assert_debug_log(expected_msgs=["stored orphan tx"]):
            peer1.send_message(msg_witness_tx(ctx))
            peer1.sync_with_ping()

        # Second wtxid peer announcing MSG_WTX for the stored orphan must be suppressed
        with mininode_lock:
            peer2.getdatas.clear()
        peer2.send_message(msg_inv([CInv(MSG_WTX, wtxid)]))
        peer2.sync_with_ping()
        with mininode_lock:
            assert_equal(len(peer2.getdatas), 0)

        # Legacy peer announcing MSG_TX for the same orphan must still be suppressed
        with mininode_lock:
            legacy_peer.getdatas.clear()
        legacy_peer.send_message(msg_inv([CInv(MSG_TX, txid)]))
        legacy_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(legacy_peer.getdatas), 0)

        # Disconnecting peer1 erases its orphans from both maps
        peers_before = len(node.getpeerinfo())
        with node.assert_debug_log(expected_msgs=["Erased 1 orphan tx"]):
            peer1.peer_disconnect()
            wait_until(lambda: len(node.getpeerinfo()) == peers_before - 1)

        with mininode_lock:
            peer2.getdatas.clear()
        peer2.send_message(msg_inv([CInv(MSG_WTX, wtxid)]))
        wait_until(
            lambda: any(i.type == MSG_WTX and i.hash == wtxid for i in peer2.getdatas),
            lock=mininode_lock,
        )

        node.disconnect_p2ps()

    def test_orphan_parent_fetch_wtxid_vs_legacy(self):
        self.log.info(
            "Test that orphan parent txid fetches are skipped for m_wtxid_relay peers but sent to legacy peers"
        )
        node = self.nodes[0]

        parent_txid_1 = 0x1111111111111111111111111111111111111111111111111111111111111111
        ctx_wtxid = CTransaction()
        ctx_wtxid.vin.append(CTxIn(COutPoint(parent_txid_1, 0), b""))
        ctx_wtxid.vout.append(CTxOut(1000, CScript([0, b"\x00" * 20])))
        ctx_wtxid.wit.vtxinwit.append(CTxInWitness())
        ctx_wtxid.wit.vtxinwit[0].scriptWitness.stack = [b"wtxid-orphan"]
        ctx_wtxid.rehash()

        parent_txid_2 = 0x2222222222222222222222222222222222222222222222222222222222222222
        ctx_legacy = CTransaction()
        ctx_legacy.vin.append(CTxIn(COutPoint(parent_txid_2, 0), b""))
        ctx_legacy.vout.append(CTxOut(1000, CScript([0, b"\x01" * 20])))
        ctx_legacy.wit.vtxinwit.append(CTxInWitness())
        ctx_legacy.wit.vtxinwit[0].scriptWitness.stack = [b"legacy-orphan"]
        ctx_legacy.rehash()

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        wtxid_peer.sync_with_ping()
        legacy_peer.sync_with_ping()

        with mininode_lock:
            wtxid_peer.getdatas.clear()
            legacy_peer.getdatas.clear()

        with node.assert_debug_log(expected_msgs=["stored orphan tx"]):
            wtxid_peer.send_message(msg_witness_tx(ctx_wtxid))
            wtxid_peer.sync_with_ping()

        with mininode_lock:
            assert_equal(len(wtxid_peer.getdatas), 0)

        with node.assert_debug_log(expected_msgs=["stored orphan tx"]):
            legacy_peer.send_message(msg_witness_tx(ctx_legacy))
            legacy_peer.sync_with_ping()

        wait_until(
            lambda: any(
                (i.type & MSG_TYPE_MASK) == MSG_TX and i.hash == parent_txid_2
                for i in legacy_peer.getdatas
            ),
            lock=mininode_lock,
        )

        node.disconnect_p2ps()

    def test_witness_mutated_vs_stripped_recent_rejects(self):
        self.log.info(
            "Test that TX_WITNESS_MUTATED caches wtxid in recentRejects while TX_WITNESS_STRIPPED does not"
        )
        node = self.nodes[0]

        # Part A: bad-witness-nonstandard (TX_WITNESS_MUTATED) caches wtxid (not txid)
        witness_script = CScript([1])
        p2wsh_addr = node.decodescript(witness_script.hex())["segwit"]["addresses"][0]
        p2wsh_txid = node.sendtoaddress(p2wsh_addr, 1.0)
        p2wsh_raw = node.getrawtransaction(p2wsh_txid, True)
        node.generate(1)
        node.setmocktime(0)
        p2wsh_vout = next(
            v["n"]
            for v in p2wsh_raw["vout"]
            if p2wsh_addr in v["scriptPubKey"].get("addresses", [])
        )

        ctx_nonstd = CTransaction()
        ctx_nonstd.vin.append(CTxIn(COutPoint(int(p2wsh_txid, 16), p2wsh_vout), b""))
        ctx_nonstd.vout.append(CTxOut(90000000, CScript([0, b"\x02" * 20])))
        ctx_nonstd.wit.vtxinwit.append(CTxInWitness())
        # Exceeds MAX_STANDARD_P2WSH_STACK_ITEM_SIZE (80 bytes) -> bad-witness-nonstandard
        ctx_nonstd.wit.vtxinwit[0].scriptWitness.stack = [b"\x00" * 81, witness_script]
        ctx_nonstd.rehash()
        txid_nonstd = int(ctx_nonstd.rehash(), 16)
        wtxid_nonstd = ctx_nonstd.calc_sha256(True)
        assert txid_nonstd != wtxid_nonstd

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        wtxid_peer.sync_with_ping()
        legacy_peer.sync_with_ping()

        with node.assert_debug_log(expected_msgs=["bad-witness-nonstandard"]):
            wtxid_peer.send_message(msg_witness_tx(ctx_nonstd))
            wtxid_peer.sync_with_ping()

        # Re-announcing the same nonstandard wtxid must be suppressed by recentRejects
        with mininode_lock:
            wtxid_peer.getdatas.clear()
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, wtxid_nonstd)]))
        wtxid_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(wtxid_peer.getdatas), 0)

        # txid must NOT be blackholed by TX_WITNESS_MUTATED
        with mininode_lock:
            legacy_peer.getdatas.clear()
        legacy_peer.send_message(msg_inv([CInv(MSG_TX, txid_nonstd)]))
        wait_until(
            lambda: any(
                (i.type & MSG_TYPE_MASK) == MSG_TX and i.hash == txid_nonstd
                for i in legacy_peer.getdatas
            ),
            lock=mininode_lock,
        )

        # Part B: witness-stripped (TX_WITNESS_STRIPPED) must NOT be cached in recentRejects
        _, ctx_valid = self._create_segwit_tx(node)
        ctx_stripped = copy.deepcopy(ctx_valid)
        ctx_stripped.wit.vtxinwit = []
        txid_valid = int(ctx_stripped.rehash(), 16)
        assert_equal(txid_valid, int(ctx_valid.rehash(), 16))

        legacy_peer.send_message(msg_witness_tx(ctx_stripped))
        legacy_peer.sync_with_ping()

        with mininode_lock:
            legacy_peer.getdatas.clear()
        legacy_peer.send_message(msg_inv([CInv(MSG_TX, txid_valid)]))
        wait_until(
            lambda: any(
                (i.type & MSG_TYPE_MASK) == MSG_TX and i.hash == txid_valid
                for i in legacy_peer.getdatas
            ),
            lock=mininode_lock,
        )

        node.disconnect_p2ps()

    def test_txid_relay_delay(self):
        self.log.info(
            "Test that txid GETDATA is delayed by TXID_RELAY_DELAY (2s) when wtxidrelay peers are connected"
        )
        node = self.nodes[0]
        mocktime = int(time.time())
        node.setmocktime(mocktime)

        wtxid_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        legacy_peer = node.add_p2p_connection(
            WtxidP2PConn(version=70039, send_wtxidrelay=False)
        )
        wtxid_peer.sync_with_ping()
        legacy_peer.sync_with_ping()

        with mininode_lock:
            wtxid_peer.getdatas.clear()
            legacy_peer.getdatas.clear()

        fake_txid = 0xCAFEBABE1111
        fake_wtxid = 0xCAFEBABE2222
        legacy_peer.send_message(msg_inv([CInv(MSG_TX, fake_txid)]))
        wtxid_peer.send_message(msg_inv([CInv(MSG_WTX, fake_wtxid)]))
        legacy_peer.sync_with_ping()
        wtxid_peer.sync_with_ping()

        # After INBOUND_PEER_TX_DELAY (2s), MSG_WTX is requested, while MSG_TX still waits for TXID_RELAY_DELAY (2s).
        node.setmocktime(mocktime + 2)
        wait_until(
            lambda: any(
                i.type == MSG_WTX and i.hash == fake_wtxid for i in wtxid_peer.getdatas
            ),
            lock=mininode_lock,
        )
        legacy_peer.sync_with_ping()
        with mininode_lock:
            assert_equal(len(legacy_peer.getdatas), 0)

        # After INBOUND_PEER_TX_DELAY + TXID_RELAY_DELAY (4s), MSG_TX is requested.
        node.setmocktime(mocktime + 4)
        wait_until(
            lambda: any(
                (i.type & MSG_TYPE_MASK) == MSG_TX and i.hash == fake_txid
                for i in legacy_peer.getdatas
            ),
            lock=mininode_lock,
        )

        node.setmocktime(0)
        node.disconnect_p2ps()

    def test_orphan_reeval_on_block_connected(self):
        self.log.info(
            "Test that an orphan from a wtxidrelay peer is resolved when its parent is confirmed in a block"
        )
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # Create a confirmed funding UTXO on node0 and sync to node1
        addr_parent = node0.getnewaddress("", "bech32")
        node0.sendtoaddress(addr_parent, 1.0)
        node0.generate(1)
        self.sync_blocks()

        utxo = next(
            u for u in node0.listunspent() if u["address"] == addr_parent and u["amount"] == 1
        )
        addr_child = node0.getnewaddress("", "bech32")
        parent_raw = node0.createrawtransaction(
            inputs=[{"txid": utxo["txid"], "vout": utxo["vout"]}],
            outputs={addr_child: 0.999},
        )
        parent_signed = node0.signrawtransactionwithwallet(parent_raw)
        assert parent_signed["complete"]
        ctx_parent = FromHex(CTransaction(), parent_signed["hex"])
        parent_txid = ctx_parent.rehash()
        script_pubkey = node0.getaddressinfo(addr_child)["scriptPubKey"]

        child_raw = node0.createrawtransaction(
            inputs=[{"txid": parent_txid, "vout": 0}],
            outputs={ADDRESS_BCRT1_UNSPENDABLE: 0.998},
        )
        child_signed = node0.signrawtransactionwithwallet(
            child_raw,
            [
                {
                    "txid": parent_txid,
                    "vout": 0,
                    "scriptPubKey": script_pubkey,
                    "amount": 0.999,
                }
            ],
        )
        assert child_signed["complete"]
        ctx_child = FromHex(CTransaction(), child_signed["hex"])
        child_txid = ctx_child.rehash()

        # Disconnect node1 so parent_tx can be mined there without entering node0's mempool
        disconnect_nodes(node0, 1)

        wtxid_peer = node0.add_p2p_connection(
            WtxidP2PConn(version=70040, send_wtxidrelay=True)
        )
        wtxid_peer.sync_with_ping()

        with node0.assert_debug_log(expected_msgs=["stored orphan tx"]):
            wtxid_peer.send_message(msg_witness_tx(ctx_child))
            wtxid_peer.sync_with_ping()
        assert child_txid not in node0.getrawmempool()

        # Mine parent_tx on node1 and reconnect so node0 learns about parent via BlockConnected
        node1.sendrawtransaction(parent_signed["hex"])
        node1.generate(1)
        connect_nodes(node0, 1)
        self.sync_blocks()

        wait_until(lambda: child_txid in node0.getrawmempool())

        node0.disconnect_p2ps()

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
        self.test_recent_confirmed_wtxid_suppressed()
        self.test_inputs_not_standard_rejects_txid()
        self.test_orphan_wtxid_already_have()
        self.test_orphan_parent_fetch_wtxid_vs_legacy()
        self.test_witness_mutated_vs_stripped_recent_rejects()
        self.test_txid_relay_delay()
        self.test_orphan_reeval_on_block_connected()


if __name__ == "__main__":
    TxDownloadTest().main()
