#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Chain specific helpers to independently compute the UTXO set hashes.

The node computes two hashes over its UTXO set:

* ``hash_serialized_2``: the legacy hash, a double SHA-256 over a
  serialization of the whole UTXO set (in coins database order).
* ``muhash``: a MuHash3072 digest over the raw (uncompressed) outputs, which
  is order independent and therefore verifiable with an independent
  implementation (see ``muhash.py``).

The helpers in this module re-implement both hashes in Python so that the
values reported by ``gettxoutsetinfo`` can be cross checked.
"""

import hashlib

from decimal import Decimal

from .muhash import MuHash3072


def write_varint(n):
    """Serialize an integer the way Bitcoin's VARINT does."""
    tmp = []
    while True:
        tmp.append((n & 0x7F) | (0x80 if tmp else 0x00))
        if n <= 0x7F:
            break
        n = (n >> 7) - 1
    return bytes(reversed(tmp))


def write_compact_size(n):
    """Serialize a length the way Bitcoin's compact size encoding does."""
    if n < 253:
        return bytes([n])
    if n <= 0xFFFF:
        return b'\xfd' + n.to_bytes(2, 'little')
    if n <= 0xFFFFFFFF:
        return b'\xfe' + n.to_bytes(4, 'little')
    return b'\xff' + n.to_bytes(8, 'little')


def amount_to_sat(amount):
    """Convert an amount as reported by the RPC into satoshis."""
    return int(Decimal(amount) * (10 ** 8))


def txid_to_bytes(txid):
    """Convert a txid into the byte order used by the node's uint256."""
    return bytes.fromhex(txid)[::-1]


def utxo_element(utxo):
    """Serialize a single UTXO the way the node's MuHash implementation does.

    The element consists of the outpoint (txid and output index), the height
    and coinbase flag of the containing transaction and the raw (uncompressed)
    output (value, script and token id).
    """
    element = txid_to_bytes(utxo["txid"])
    element += utxo["vout"].to_bytes(4, 'little')
    element += (utxo["height"] * 2 + (1 if utxo["coinbase"] else 0)).to_bytes(4, 'little')
    element += utxo["value"].to_bytes(8, 'little', signed=True)
    script = utxo["script"]
    element += write_compact_size(len(script)) + script
    element += write_varint(utxo["token_id"])
    return element


def utxo_set(node):
    """Collect the UTXO set of a node.

    The UTXO set is derived from the blocks of the chain: every output that is
    still known to the node (i.e. that ``gettxout`` reports) is part of it.
    """
    tip = node.getblockcount()
    utxos = []
    for height in range(0, tip + 1):
        block = node.getblock(node.getblockhash(height), 2)
        for tx in block["tx"]:
            for vout in tx["vout"]:
                coin = node.gettxout(tx["txid"], vout["n"], False)
                if coin is None:
                    # Not part of the UTXO set (spent or unspendable).
                    continue
                utxos.append({
                    "txid": tx["txid"],
                    "vout": vout["n"],
                    "height": tip - coin["confirmations"] + 1,
                    "coinbase": bool(coin["coinbase"]),
                    "value": amount_to_sat(coin["value"]),
                    "script": bytes.fromhex(coin["scriptPubKey"]["hex"]),
                    "token_id": vout.get("tokenId", 0) or 0,
                })
    return utxos


def muhash(utxos):
    """Compute the MuHash3072 digest over the given UTXO set."""
    muhash = MuHash3072()
    for utxo in utxos:
        muhash.insert(utxo_element(utxo))
    return muhash.hexdigest()


def hash_serialized_2(bestblock, utxos):
    """Compute the legacy (hash_serialized_2) hash over the given UTXO set.

    The legacy hash serializes the UTXO set in coins database order, i.e.
    ordered by the (byte reversed) txid and then by the output index.
    """
    ordered = sorted(utxos, key=lambda utxo: (txid_to_bytes(utxo["txid"]), utxo["vout"]))
    stream = hashlib.sha256()
    stream.update(txid_to_bytes(bestblock))
    index = 0
    while index < len(ordered):
        txid = txid_to_bytes(ordered[index]["txid"])
        end = index
        while end < len(ordered) and txid_to_bytes(ordered[end]["txid"]) == txid:
            end += 1
        stream.update(txid)
        stream.update(write_varint(1 if ordered[index]["height"] * 2 + (1 if ordered[index]["coinbase"] else 0) else 0))
        for utxo in ordered[index:end]:
            stream.update(write_varint(utxo["vout"] + 1))
            stream.update(write_compact_size(len(utxo["script"])))
            stream.update(utxo["script"])
            stream.update(write_varint(utxo["value"]))
        stream.update(write_varint(0))
        index = end
    return hashlib.sha256(stream.digest()).digest()[::-1].hex()
