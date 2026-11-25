#!/usr/bin/env python3
# Copyright (c) 2026 DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the external signer enumeration.

Verify that a node can use an external signer command.

See also wallet_signer.py for the tests that require the wallet context.
"""
import json
import os
import platform
import sys

from test_framework.test_framework import DefiTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class RPCSignerTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            [],
            [f"-signer={self.mock_signer_path()}"],
            [f"-signer={self.mock_invalid_signer_path()}"],
            ["-signer=defi-missing-signer-binary"],
        ]

    def mock_signer_path(self, name="signer.py"):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), "mocks", name)
        return sys.executable + " " + path

    def mock_invalid_signer_path(self):
        return self.mock_signer_path("invalid_signer.py")

    def setup_network(self):
        # The mock signer writes its answers and the record of its calls into
        # the temporary directory of the test, never into the working directory
        # of the node.
        self.dump_dir = os.path.join(self.options.tmpdir, "signer")
        os.makedirs(self.dump_dir, exist_ok=True)
        os.environ["DFI_MOCK_SIGNER_DUMP_DIR"] = self.dump_dir
        self.setup_nodes()

    def skip_test_if_missing_module(self):
        pass

    def set_mock_signers(self, signers):
        with open(os.path.join(self.dump_dir, "mock_signers.json"), "w") as f:
            json.dump(signers, f)

    def clear_mock_signers(self):
        path = os.path.join(self.dump_dir, "mock_signers.json")
        if os.path.isfile(path):
            os.remove(path)

    def set_mock_exit_code(self, code):
        with open(os.path.join(self.dump_dir, "mock_exit_code"), "w") as f:
            f.write(str(code))

    def clear_mock_exit_code(self):
        path = os.path.join(self.dump_dir, "mock_exit_code")
        if os.path.isfile(path):
            os.remove(path)

    def run_test(self):
        if not self.is_external_signer_compiled():
            # The RPC methods are registered in every build, a build without
            # external signer support reports the missing support. The check
            # comes before the check of the -signer option.
            self.log.info("The build has no external signer support")
            assert_raises_rpc_error(
                -1,
                "External signer support is not enabled in this build",
                self.nodes[0].enumeratesigners,
            )
            assert_raises_rpc_error(
                -1,
                "External signer support is not enabled in this build",
                self.nodes[1].enumeratesigners,
            )
            return

        # A node without -signer reports how to enable the support.
        assert_raises_rpc_error(
            -1,
            "Error: restart defid with -signer=<cmd>",
            self.nodes[0].enumeratesigners,
        )

        # A signer that cannot be started.
        assert_raises_rpc_error(
            -1,
            (
                "CreateProcess failed"
                if platform.system() == "Windows"
                else "execve failed: No such file or directory"
            ),
            self.nodes[3].enumeratesigners,
        )

        # A signer that does not answer with valid JSON.
        assert_raises_rpc_error(
            -1,
            "Unable to parse JSON",
            self.nodes[2].enumeratesigners,
        )

        # The default answer of the mock signer.
        assert_equal(
            self.nodes[1].enumeratesigners(),
            {"signers": [{"fingerprint": "00000001", "name": "trezor_t"}]},
        )

        # A signer that exits with an error.
        self.set_mock_exit_code(2)
        assert_raises_rpc_error(
            -1,
            "RunCommandParseJSON error",
            self.nodes[1].enumeratesigners,
        )
        self.clear_mock_exit_code()

        # An error of the signer is reported.
        self.set_mock_signers(
            [{"type": "trezor", "model": "trezor_t", "error": "fingerprint not found"}]
        )
        assert_raises_rpc_error(
            -1,
            "error: fingerprint not found",
            self.nodes[1].enumeratesigners,
        )
        self.clear_mock_signers()

        # A signer without a fingerprint is rejected.
        self.set_mock_signers([{"type": "trezor", "model": "trezor_t"}])
        assert_raises_rpc_error(
            -1,
            "missing signer fingerprint",
            self.nodes[1].enumeratesigners,
        )
        self.clear_mock_signers()

        # A signer with an invalid fingerprint is rejected.
        for fingerprint in ["", "0000001", "000000001", "0000000g", "zzzzzzzz"]:
            self.set_mock_signers(
                [{"type": "trezor", "model": "trezor_t", "fingerprint": fingerprint}]
            )
            assert_raises_rpc_error(
                -1,
                "invalid fingerprint",
                self.nodes[1].enumeratesigners,
            )
            self.clear_mock_signers()

        # An answer that is not an array is rejected.
        self.set_mock_signers({"fingerprint": "00000001"})
        assert_raises_rpc_error(
            -1,
            "expected array of signers",
            self.nodes[1].enumeratesigners,
        )
        self.clear_mock_signers()

        # Signers with a duplicate fingerprint are skipped.
        self.set_mock_signers(
            [
                {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
                {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
                {"fingerprint": "00000002", "type": "trezor", "model": "trezor_one"},
            ]
        )
        assert_equal(
            self.nodes[1].enumeratesigners(),
            {
                "signers": [
                    {"fingerprint": "00000001", "name": "trezor_t"},
                    {"fingerprint": "00000002", "name": "trezor_one"},
                ]
            },
        )
        self.clear_mock_signers()


if __name__ == "__main__":
    RPCSignerTest().main()
