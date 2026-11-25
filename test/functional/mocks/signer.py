#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mock external signer.

The mock implements the command line interface of an external signer:

    <cmd> enumerate
    <cmd> --fingerprint <fpr> --chain <chain> displayaddress --desc <desc>

The answers are configured with files in the directory that is named by the
environment variable DFI_MOCK_SIGNER_DUMP_DIR:

    mock_signers.json   the answer of the enumerate command
    mock_display.json   the answer of the displayaddress command, a mapping
                        of the public key of the descriptor to the address
                        that the device echoes
    mock_exit_code      if the file exists, the mock exits with its content

Every call is recorded in mock_calls.json of the same directory, so that the
test can assert the argument contract of the signer.
"""
import argparse
import json
import os
import re
import sys

DUMP_DIR_ENV = "DFI_MOCK_SIGNER_DUMP_DIR"
SIGNERS_FILE = "mock_signers.json"
DISPLAY_FILE = "mock_display.json"
EXIT_CODE_FILE = "mock_exit_code"
CALLS_FILE = "mock_calls.json"

DEFAULT_SIGNERS = [
    {"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"},
]


def dump_dir():
    return os.environ.get(DUMP_DIR_ENV)


def dump_path(name):
    directory = dump_dir()
    if not directory:
        return None
    return os.path.join(directory, name)


def read_json(name, default):
    path = dump_path(name)
    if not path or not os.path.isfile(path):
        return default
    with open(path, "r") as f:
        return json.load(f)


def record_call(argv, stdin_data):
    """Record the arguments of the call for the assertions of the test."""
    path = dump_path(CALLS_FILE)
    if not path:
        return
    calls = []
    if os.path.isfile(path):
        try:
            with open(path, "r") as f:
                calls = json.load(f)
        except (OSError, ValueError):
            calls = []
    calls.append({"argv": argv, "stdin": stdin_data})
    with open(path, "w") as f:
        json.dump(calls, f)


def pubkey_from_descriptor(descriptor):
    """Extract the public key of a single key descriptor."""
    match = re.search(r"[0-9a-fA-F]{66}", descriptor)
    return match.group(0) if match else None


def enumerate_signers(args):
    sys.stdout.write(json.dumps(read_json(SIGNERS_FILE, DEFAULT_SIGNERS)))


def display_address(args):
    pubkey = pubkey_from_descriptor(args.desc)
    addresses = read_json(DISPLAY_FILE, {})
    address = addresses.get(pubkey) if pubkey else None
    if address is None:
        sys.stdout.write(
            json.dumps({"error": "Unexpected descriptor", "desc": args.desc})
        )
        return
    sys.stdout.write(json.dumps({"address": address}))


def main(enumerate_func=None):
    parser = argparse.ArgumentParser(
        prog="signer.py", description="External signer mock"
    )
    parser.add_argument("--fingerprint")
    parser.add_argument("--chain", default="main")
    parser.add_argument("--stdin", action="store_true")

    subparsers = parser.add_subparsers(description="Commands", dest="command")
    subparsers.required = True

    parser_enumerate = subparsers.add_parser("enumerate", help="list available signers")
    parser_enumerate.set_defaults(func=enumerate_func or enumerate_signers)

    parser_display = subparsers.add_parser(
        "displayaddress", help="display address on signer"
    )
    parser_display.add_argument("--desc", metavar="desc")
    parser_display.set_defaults(func=display_address)

    args = parser.parse_args()

    stdin_data = ""
    if not sys.stdin.isatty():
        stdin_data = sys.stdin.read()

    record_call(sys.argv, stdin_data)

    exit_code_path = dump_path(EXIT_CODE_FILE)
    if exit_code_path and os.path.isfile(exit_code_path):
        with open(exit_code_path, "r") as f:
            sys.exit(int(f.read().strip() or 1))

    args.func(args)


if __name__ == "__main__":
    main()
