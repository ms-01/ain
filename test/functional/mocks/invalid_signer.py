#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mock external signer with an invalid answer.

The mock answers the enumerate command with output that is not valid JSON.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import signer  # noqa: E402


def enumerate_signers(args):
    sys.stdout.write("{invalid json}")


if __name__ == "__main__":
    signer.main(enumerate_signers)
