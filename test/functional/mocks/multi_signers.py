#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mock external signer with more than one connected device.

The mock answers the enumerate command with two signers.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import signer  # noqa: E402

MULTI_SIGNERS = [
    {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
    {"fingerprint": "00000002", "type": "trezor", "model": "trezor_one"},
]


def enumerate_signers(args):
    sys.stdout.write(json.dumps(MULTI_SIGNERS))


if __name__ == "__main__":
    signer.main(enumerate_signers)
