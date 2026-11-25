#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the importdescriptors and listdescriptors RPCs.

The test covers the additive descriptor support of the DeFi Blockchain wallet:

- importing a descriptor (ranged and non ranged, public and private),
- the per item error semantics of `importdescriptors`,
- the persistence of the descriptor records over a node restart,
- the export and the re-import of the descriptors of a wallet,
- the active descriptor handling (address derivation, next index),
- the encryption of the private descriptors.
"""
import hashlib
import time

from test_framework.descriptors import descsum_create
from test_framework.key import ECKey
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes,
)

B58_CHARS = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

# The base58 prefixes of the regtest chain.
REGTEST_SECRET_KEY = bytes([0xEF])
REGTEST_EXT_PUBLIC_KEY = bytes([0x04, 0x35, 0x87, 0xCF])

# The BIP32 test vector 1 (mainnet encoding), the version bytes are replaced
# with the regtest ones so that the node accepts the keys.
BIP32_XPUB = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8"


def b58encode(data):
    number = int.from_bytes(data, "big")
    result = ""
    while number > 0:
        number, remainder = divmod(number, 58)
        result = B58_CHARS[remainder] + result
    pad = 0
    for byte in data:
        if byte == 0:
            pad += 1
        else:
            break
    return "1" * pad + result


def b58decode(text):
    number = 0
    for char in text:
        number = number * 58 + B58_CHARS.index(char)
    data = number.to_bytes((number.bit_length() + 7) // 8, "big")
    pad = 0
    for char in text:
        if char == "1":
            pad += 1
        else:
            break
    return b"\x00" * pad + data


def b58check_encode(payload):
    checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return b58encode(payload + checksum)


def b58check_decode(text):
    data = b58decode(text)
    payload, checksum = data[:-4], data[-4:]
    assert hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4] == checksum
    return payload


def key_to_wif(key):
    """Encode a private key in the wallet import format of the regtest chain."""
    payload = REGTEST_SECRET_KEY + key.get_bytes()
    if key.is_compressed:
        payload += b"\x01"
    return b58check_encode(payload)


def convert_ext_key(key, version):
    """Replace the version bytes of an extended key (mainnet -> regtest)."""
    payload = b58check_decode(key)
    return b58check_encode(version + payload[4:])


def descriptor(body):
    """Return a descriptor string with a valid checksum."""
    return descsum_create(body)


class ImportDescriptorsTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-addresstype=legacy"], ["-addresstype=legacy"]]

    def check_multisig_descriptor_limits(self, node):
        """The multi() descriptor supports up to 20 keys, the P2SH wrapper is
        limited by the 520 byte redeem script size."""
        self.log.info("Check the number of keys of a multi() descriptor")
        pubkeys = []
        for _ in range(21):
            key = ECKey()
            key.generate()
            pubkeys.append(key.get_pubkey().get_bytes().hex())

        def multi_desc(count, wrapper="wsh"):
            body = "multi(2,{})".format(",".join(pubkeys[:count]))
            if wrapper:
                body = "{}({})".format(wrapper, body)
            return descsum_create(body)

        # A witness script with 20 keys is accepted.
        info = node.getdescriptorinfo(multi_desc(20))
        assert info["descriptor"]
        assert info["issolvable"]

        # More than 20 keys are rejected.
        assert_raises_rpc_error(
            -5,
            "Cannot have 21 keys in multisig; must have between 1 and 20 keys, inclusive",
            node.getdescriptorinfo,
            multi_desc(21),
        )

        # A bare multisig descriptor supports at most 3 keys.
        assert_raises_rpc_error(
            -5,
            "Cannot have 4 pubkeys in bare multisig; only at most 3 pubkeys",
            node.getdescriptorinfo,
            descsum_create("multi(2,{})".format(",".join(pubkeys[:4]))),
        )

        # The P2SH wrapper is limited by the 520 byte redeem script size, which
        # allows at most 15 compressed keys.
        assert_raises_rpc_error(
            -5,
            "P2SH script is too large",
            node.getdescriptorinfo,
            multi_desc(16, "sh"),
        )
        info = node.getdescriptorinfo(multi_desc(15, "sh"))
        assert info["descriptor"]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        connect_nodes(self.nodes[0], 1)

    def run_test(self):
        node = self.nodes[0]
        self.log.info("Mining blocks...")
        node.generate(101)

        self.check_multisig_descriptor_limits(node)

        # A wallet without descriptor records behaves exactly as before.
        self.log.info("A legacy wallet has no descriptors")
        assert_equal(node.listdescriptors()["descriptors"], [])
        assert_equal(node.getwalletinfo()["descriptors"], False)
        legacy_address = node.getnewaddress()
        assert legacy_address

        # ------------------------------------------------------------------
        # Import a non ranged descriptor with a private key.
        # ------------------------------------------------------------------
        self.log.info("Import a non ranged descriptor")
        key = ECKey()
        key.generate()
        wif = key_to_wif(key)
        private_desc = descriptor("pkh({})".format(wif))
        public_desc = node.getdescriptorinfo(private_desc)["descriptor"]

        result = node.importdescriptors([{"desc": private_desc, "timestamp": "now"}])
        assert_equal(result[0]["success"], True)

        listed = node.listdescriptors()
        assert_equal(len(listed["descriptors"]), 1)
        entry = listed["descriptors"][0]
        assert_equal(entry["desc"], public_desc)
        assert_equal(entry["active"], False)
        assert "internal" not in entry
        assert "range" not in entry
        assert "next" not in entry
        assert "next_index" not in entry
        assert entry["timestamp"] > 0
        assert_equal(node.getwalletinfo()["descriptors"], True)

        # The imported address belongs to the wallet.
        address = node.deriveaddresses(public_desc)[0]
        info = node.getaddressinfo(address)
        assert_equal(info["ismine"], True)
        assert_equal(info["iswatchonly"], False)

        # ------------------------------------------------------------------
        # Per item error semantics.
        # ------------------------------------------------------------------
        self.log.info("Per item errors of importdescriptors")
        cases = [
            # A missing descriptor.
            ({"timestamp": "now"}, -8, "Descriptor not found."),
            # A missing timestamp.
            ({"desc": private_desc}, -3, "Missing required timestamp field for key"),
            # An invalid checksum.
            ({"desc": private_desc[:-8] + "00000000", "timestamp": "now"}, -5, None),
            # A range for a non ranged descriptor.
            (
                {"desc": private_desc, "timestamp": "now", "range": [0, 1]},
                -8,
                "Range should not be specified for an un-ranged descriptor",
            ),
            # A label for an internal descriptor.
            (
                {
                    "desc": private_desc,
                    "timestamp": "now",
                    "internal": True,
                    "label": "x",
                },
                -8,
                "Internal addresses should not have a label",
            ),
            # An active non ranged descriptor.
            (
                {"desc": private_desc, "timestamp": "now", "active": True},
                -8,
                "Active descriptors must be ranged",
            ),
            # next_index without an active ranged descriptor.
            (
                {"desc": private_desc, "timestamp": "now", "next_index": 0},
                -8,
                "next_index is only allowed for active ranged descriptors",
            ),
            # An unsupported descriptor function.
            (
                {
                    "desc": descriptor(
                        "tr(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)"
                    ),
                    "timestamp": "now",
                },
                -5,
                None,
            ),
        ]
        for request, error_code, error_message in cases:
            result = node.importdescriptors([request])
            assert_equal(result[0]["success"], False)
            assert_equal(result[0]["error"]["code"], error_code)
            if error_message is not None:
                assert_equal(result[0]["error"]["message"], error_message)

        # A failing item does not abort the batch.
        self.log.info("A failing item does not abort the batch")
        other_key = ECKey()
        other_key.generate()
        other_desc = descriptor("pkh({})".format(key_to_wif(other_key)))
        result = node.importdescriptors(
            [
                {"desc": "not a descriptor", "timestamp": "now"},
                {"desc": other_desc, "timestamp": "now"},
            ]
        )
        assert_equal(result[0]["success"], False)
        assert_equal(result[1]["success"], True)
        assert_equal(len(node.listdescriptors()["descriptors"]), 2)

        # ------------------------------------------------------------------
        # Import a ranged descriptor.
        # ------------------------------------------------------------------
        self.log.info("Import a ranged descriptor")
        xpub = convert_ext_key(BIP32_XPUB, REGTEST_EXT_PUBLIC_KEY)
        ranged_desc = descriptor("wpkh({}/0/*)".format(xpub))
        result = node.importdescriptors(
            [{"desc": ranged_desc, "timestamp": "now", "range": [0, 10]}]
        )
        assert_equal(result[0]["success"], True)

        listed = node.listdescriptors()
        ranged_entry = None
        for entry in listed["descriptors"]:
            if entry["desc"].startswith("wpkh("):
                ranged_entry = entry
                break
        assert ranged_entry is not None
        assert_equal(ranged_entry["range"], [0, 10])
        assert_equal(ranged_entry["next"], 0)
        assert_equal(ranged_entry["next_index"], 0)
        assert_equal(ranged_entry["active"], False)

        # A ranged descriptor must not have a label.
        result = node.importdescriptors(
            [{"desc": ranged_desc, "timestamp": "now", "range": [0, 10], "label": "x"}]
        )
        assert_equal(result[0]["success"], False)
        assert_equal(
            result[0]["error"]["message"], "Ranged descriptors should not have a label"
        )

        # ------------------------------------------------------------------
        # An active descriptor derives the addresses of the wallet.
        # ------------------------------------------------------------------
        self.log.info("An active descriptor derives the new addresses")
        result = node.importdescriptors(
            [
                {
                    "desc": ranged_desc,
                    "timestamp": "now",
                    "range": [0, 10],
                    "active": True,
                }
            ]
        )
        assert_equal(result[0]["success"], True)

        listed = node.listdescriptors()
        active_entry = None
        for entry in listed["descriptors"]:
            if entry["active"]:
                active_entry = entry
                break
        assert active_entry is not None
        assert_equal(active_entry["internal"], False)
        assert_equal(active_entry["next_index"], 0)

        expected_address = node.deriveaddresses(ranged_desc, [0, 0])[0]
        assert_equal(node.getnewaddress("", "bech32"), expected_address)
        listed = node.listdescriptors()
        for entry in listed["descriptors"]:
            if entry["active"]:
                assert_equal(entry["next_index"], 1)
                assert_equal(entry["next"], 1)

        # The second address is derived from the next index of the descriptor.
        expected_address_2 = node.deriveaddresses(ranged_desc, [1, 1])[0]
        assert_equal(node.getnewaddress("", "bech32"), expected_address_2)

        # Only one active descriptor per address type and externality.
        self.log.info(
            "A second active descriptor for the same address type is rejected"
        )
        second_desc = descriptor("wpkh({}/1/*)".format(xpub))
        result = node.importdescriptors(
            [
                {
                    "desc": second_desc,
                    "timestamp": "now",
                    "range": [0, 10],
                    "active": True,
                }
            ]
        )
        assert_equal(result[0]["success"], False)
        assert_equal(
            result[0]["error"]["message"],
            "There is already an active descriptor for this address type",
        )

        # An active descriptor and a HD seed are two sources of truth.
        self.log.info("sethdseed is rejected for a wallet with an active descriptor")
        assert_raises_rpc_error(
            -4,
            "Cannot set a HD seed on a wallet with an active descriptor",
            node.sethdseed,
        )

        # The descriptor list of the wallet is used to restore the wallet later on.
        before = node.listdescriptors()

        # ------------------------------------------------------------------
        # The descriptor records survive a restart of the node.
        # ------------------------------------------------------------------
        self.log.info("The descriptor records survive a restart")
        self.restart_node(0)
        node = self.nodes[0]
        after = node.listdescriptors()
        assert_equal(after, before)
        assert_equal(node.getwalletinfo()["descriptors"], True)
        # The node does not reconnect to its peers after a restart.
        connect_nodes(self.nodes[0], 1)

        # ------------------------------------------------------------------
        # A wallet can be restored from its descriptors.
        # ------------------------------------------------------------------
        self.log.info("A wallet can be restored from its descriptors")
        restore_node = self.nodes[1]
        requests = []
        for entry in before["descriptors"]:
            request = {"desc": entry["desc"], "timestamp": entry["timestamp"]}
            if "range" in entry:
                request["range"] = entry["range"]
            requests.append(request)
        result = restore_node.importdescriptors(requests)
        for item in result:
            assert_equal(item["success"], True)
        restored = restore_node.listdescriptors()
        assert_equal(len(restored["descriptors"]), len(before["descriptors"]))
        assert_equal(
            [entry["desc"] for entry in restored["descriptors"]],
            [entry["desc"] for entry in before["descriptors"]],
        )

        # ------------------------------------------------------------------
        # A rescan finds the transactions of an imported descriptor.
        # ------------------------------------------------------------------
        self.log.info("A rescan finds the transactions of an imported descriptor")
        rescan_node = self.nodes[1]
        watch_key = ECKey()
        watch_key.generate()
        watch_desc = descriptor("pkh({})".format(key_to_wif(watch_key)))
        watch_public = rescan_node.getdescriptorinfo(watch_desc)["descriptor"]
        watch_address = rescan_node.deriveaddresses(watch_public)[0]

        self.log.info("Send coins to the descriptor address")
        # The transaction is mined into a block that is older than the tip of
        # the chain (the timestamps are more than two hours apart), so that the
        # transaction is only found by a rescan of the whole chain.
        for mock_node in self.nodes:
            mock_node.set_mocktime(int(time.time()) + 4 * 60 * 60)
            mock_node.setmocktime(int(time.time()) + 4 * 60 * 60)
        self.nodes[0].sendtoaddress(watch_address, 1)
        self.nodes[0].generate(1)
        for mock_node in self.nodes:
            mock_node.set_mocktime(int(time.time()) + 8 * 60 * 60)
            mock_node.setmocktime(int(time.time()) + 8 * 60 * 60)
        # The median time past of the tip has to move as well, therefore more
        # than eleven blocks are generated with the new timestamp.
        self.nodes[0].generate(11)
        self.sync_blocks()

        self.log.info("Import the descriptor into a fresh wallet")
        result = rescan_node.importdescriptors(
            [{"desc": watch_public, "timestamp": "now"}]
        )
        assert_equal(result[0]["success"], True)
        # The transaction is not known to the wallet yet.
        assert_equal(len(rescan_node.listunspent()), 0)

        # Re-importing the descriptor with timestamp 0 triggers a rescan that
        # finds the transaction.
        result = rescan_node.importdescriptors([{"desc": watch_public, "timestamp": 0}])
        assert_equal(result[0]["success"], True)
        assert_equal(len(rescan_node.listunspent()), 1)
        assert_equal(rescan_node.listunspent()[0]["address"], watch_address)

        # ------------------------------------------------------------------
        # The private descriptors are encrypted with the wallet master key.
        # ------------------------------------------------------------------
        self.log.info("The private descriptors are encrypted")
        node.encryptwallet("descriptor-passphrase")
        assert_raises_rpc_error(
            -13,
            "Error: Please enter the wallet passphrase with walletpassphrase first.",
            node.listdescriptors,
            True,
        )
        node.walletpassphrase("descriptor-passphrase", 60)
        private_descriptors = [
            entry["desc"] for entry in node.listdescriptors(True)["descriptors"]
        ]
        assert any(wif in desc for desc in private_descriptors)
        # The public descriptor list is not affected by the private argument.
        public_listed = node.listdescriptors()
        assert public_desc in [entry["desc"] for entry in public_listed["descriptors"]]

        # The passphrase can be changed without losing the private descriptor.
        node.walletpassphrasechange("descriptor-passphrase", "descriptor-passphrase-2")
        node.walletpassphrase("descriptor-passphrase-2", 60)
        private_descriptors = [
            entry["desc"] for entry in node.listdescriptors(True)["descriptors"]
        ]
        assert any(wif in desc for desc in private_descriptors)


if __name__ == "__main__":
    ImportDescriptorsTest().main()
