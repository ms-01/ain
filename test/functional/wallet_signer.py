#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the external signer address display.

Verify that a wallet address can be displayed on an external signer.

See also rpc_signer.py for the tests that do not require the wallet context.
"""
import json
import os
import sys

from test_framework.descriptors import descsum_create
from test_framework.key import ECKey
from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# The master key fingerprint of the mock device.
DEVICE_FINGERPRINT = "00000001"


class WalletSignerTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [],
            [f"-signer={self.mock_signer_path()}"],
        ]

    def mock_signer_path(self, name="signer.py"):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), "mocks", name)
        return sys.executable + " " + path

    def setup_network(self):
        # The mock signer writes its answers and the record of its calls into
        # the temporary directory of the test, never into the working directory
        # of the node.
        self.dump_dir = os.path.join(self.options.tmpdir, "signer")
        os.makedirs(self.dump_dir, exist_ok=True)
        os.environ["DFI_MOCK_SIGNER_DUMP_DIR"] = self.dump_dir
        self.setup_nodes()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_mock_signers(self, signers):
        with open(os.path.join(self.dump_dir, "mock_signers.json"), "w") as f:
            json.dump(signers, f)

    def clear_mock_signers(self):
        path = os.path.join(self.dump_dir, "mock_signers.json")
        if os.path.isfile(path):
            os.remove(path)

    def set_mock_display(self, addresses):
        with open(os.path.join(self.dump_dir, "mock_display.json"), "w") as f:
            json.dump(addresses, f)

    def clear_mock_display(self):
        path = os.path.join(self.dump_dir, "mock_display.json")
        if os.path.isfile(path):
            os.remove(path)

    def mock_calls(self):
        path = os.path.join(self.dump_dir, "mock_calls.json")
        if not os.path.isfile(path):
            return []
        with open(path, "r") as f:
            return json.load(f)

    def clear_mock_calls(self):
        path = os.path.join(self.dump_dir, "mock_calls.json")
        if os.path.isfile(path):
            os.remove(path)

    def import_descriptor(self, node, descriptor):
        result = node.importdescriptors([{"desc": descriptor, "timestamp": "now"}])
        assert_equal(result[0]["success"], True)

    def run_test(self):
        node = self.nodes[1]

        if not self.is_external_signer_compiled():
            # The RPC methods are registered in every build, a build without
            # external signer support reports the missing support. The check
            # comes before the check of the -signer option.
            self.log.info("The build has no external signer support")
            assert_raises_rpc_error(
                -1,
                "External signer support is not enabled in this build",
                node.walletdisplayaddress,
                "bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g",
            )
            assert_raises_rpc_error(
                -1,
                "External signer support is not enabled in this build",
                self.nodes[0].walletdisplayaddress,
                "bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g",
            )
            return

        # A node without -signer reports how to enable the support.
        assert_raises_rpc_error(
            -1,
            "Error: restart defid with -signer=<cmd>",
            self.nodes[0].walletdisplayaddress,
            "bcrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th68x4f8g",
        )

        # An invalid address is rejected.
        assert_raises_rpc_error(
            -5,
            "Invalid address",
            node.walletdisplayaddress,
            "not-an-address",
        )

        # ------------------------------------------------------------------
        # A descriptor with the origin information of the device.
        # ------------------------------------------------------------------
        self.log.info("Display an address of a descriptor with an origin")
        key = ECKey()
        key.generate()
        pubkey = key.get_pubkey().get_bytes().hex()
        descriptor = descsum_create(
            "wpkh([{}/84'/1'/0'/0/0]{})".format(DEVICE_FINGERPRINT, pubkey)
        )
        self.import_descriptor(node, descriptor)
        address = node.deriveaddresses(descriptor)[0]

        # The address belongs to the wallet (as a watch only address).
        assert_equal(node.getaddressinfo(address)["iswatchonly"], True)

        # Without the answer of the device the address cannot be displayed.
        assert_raises_rpc_error(
            -1,
            "Signer returned error",
            node.walletdisplayaddress,
            address,
        )

        # The device echoes the address of the public key.
        self.set_mock_display({pubkey: address})
        self.clear_mock_calls()
        assert_equal(node.walletdisplayaddress(address), {"address": address})

        # The argument contract of the signer: the enumeration and the display
        # of the address.
        calls = self.mock_calls()
        assert_equal(len(calls), 2)
        assert "enumerate" in calls[0]["argv"]
        argv = calls[1]["argv"]
        assert "--fingerprint" in argv
        assert DEVICE_FINGERPRINT in argv
        assert "--chain" in argv
        assert "regtest" in argv
        assert "displayaddress" in argv
        assert "--desc" in argv
        device_descriptor = argv[argv.index("--desc") + 1]
        assert device_descriptor.startswith(
            "wpkh([{}/84'/1'/0'/0/0]{}".format(DEVICE_FINGERPRINT, pubkey)
        ), device_descriptor

        # A device that echoes another address is rejected.
        self.set_mock_display({pubkey: "bcrt1ql7zg7ukh3dwr25ex2zn9jse926f27xy2jz58tm"})
        assert_raises_rpc_error(
            -1,
            "Signer echoed unexpected address",
            node.walletdisplayaddress,
            address,
        )
        self.set_mock_display({pubkey: address})

        # An address that does not belong to the wallet is rejected.
        other_address = self.nodes[0].getnewaddress()
        assert_raises_rpc_error(
            -4,
            "is not owned by this wallet",
            node.walletdisplayaddress,
            other_address,
        )

        # ------------------------------------------------------------------
        # The selection of the device.
        # ------------------------------------------------------------------
        self.log.info("The selection of the device")
        # A device with another fingerprint is not selected.
        self.set_mock_signers(
            [{"fingerprint": "00000002", "type": "trezor", "model": "trezor_one"}]
        )
        assert_raises_rpc_error(
            -1,
            "No external signer with fingerprint {} connected (connected: 00000002)".format(
                DEVICE_FINGERPRINT
            ),
            node.walletdisplayaddress,
            address,
        )
        self.clear_mock_signers()

        # More than one device is connected, the device of the origin
        # information is selected.
        self.set_mock_signers(
            [
                {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
                {"fingerprint": "00000002", "type": "trezor", "model": "trezor_one"},
            ]
        )
        assert_equal(node.walletdisplayaddress(address), {"address": address})
        self.clear_mock_signers()

        # No connected device at all.
        self.set_mock_signers([])
        assert_raises_rpc_error(
            -1,
            "No external signer connected",
            node.walletdisplayaddress,
            address,
        )
        self.clear_mock_signers()

        # ------------------------------------------------------------------
        # A descriptor without the origin information of the device.
        # ------------------------------------------------------------------
        self.log.info("Display an address of a descriptor without an origin")
        key_without_origin = ECKey()
        key_without_origin.generate()
        pubkey_without_origin = key_without_origin.get_pubkey().get_bytes().hex()
        descriptor_without_origin = descsum_create(
            "wpkh({})".format(pubkey_without_origin)
        )
        self.import_descriptor(node, descriptor_without_origin)
        address_without_origin = node.deriveaddresses(descriptor_without_origin)[0]

        self.set_mock_display({pubkey_without_origin: address_without_origin})
        assert_equal(
            node.walletdisplayaddress(address_without_origin),
            {"address": address_without_origin},
        )

        # The descriptor of a key without origin information has no origin, a
        # fabricated origin must never reach the device.
        calls = self.mock_calls()
        argv = calls[-1]["argv"]
        device_descriptor = argv[argv.index("--desc") + 1]
        assert device_descriptor.startswith(
            "wpkh({}".format(pubkey_without_origin)
        ), device_descriptor
        assert "[" not in device_descriptor, device_descriptor

        # Without the origin information more than one device is ambiguous.
        self.set_mock_signers(
            [
                {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
                {"fingerprint": "00000002", "type": "trezor", "model": "trezor_one"},
            ]
        )
        assert_raises_rpc_error(
            -1,
            "Multiple external signers connected (00000001, 00000002); connect only one device",
            node.walletdisplayaddress,
            address_without_origin,
        )
        self.clear_mock_signers()


if __name__ == "__main__":
    WalletSignerTest().main()
