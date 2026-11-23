#!/usr/bin/env python3
# Copyright (c) 2019-2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Run fuzz test targets using the unified src/test/fuzz/fuzz binary."""

import argparse
import configparser
import logging
import os
import subprocess
import sys


def discover_targets(fuzz_bin):
    env = os.environ.copy()
    env["PRINT_ALL_FUZZ_TARGETS_AND_ABORT"] = "1"
    result = subprocess.run(
        [fuzz_bin],
        env=env,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def is_libfuzzer_build(fuzz_bin, sample_target):
    env = os.environ.copy()
    env["FUZZ"] = sample_target
    try:
        result = subprocess.run(
            [fuzz_bin, "-help=1"],
            env=env,
            timeout=2,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        return "libFuzzer" in result.stderr
    except subprocess.TimeoutExpired:
        return False


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "-l",
        "--loglevel",
        dest="loglevel",
        default="INFO",
        help="log events at this level and higher to the console.",
    )
    parser.add_argument(
        "--export_coverage",
        action="store_true",
        help="If true, export coverage information to files in the seed corpus",
    )
    parser.add_argument(
        "--max_total_time",
        type=int,
        default=0,
        help="If > 0 and built with libFuzzer, run each target for this many seconds.",
    )
    parser.add_argument(
        "--fuzz_bin",
        default=None,
        help="Path to the unified fuzz binary (defaults to BUILDDIR/src/test/fuzz/fuzz).",
    )
    parser.add_argument(
        "seed_dir",
        nargs="?",
        default=None,
        help="Optional seed corpus directory (subfolders per fuzz target).",
    )
    parser.add_argument(
        "target",
        nargs="*",
        help="The target(s) to run. Default is to run all targets.",
    )

    args = parser.parse_args()

    logging.basicConfig(
        format="%(message)s",
        level=int(args.loglevel) if args.loglevel.isdigit() else args.loglevel.upper(),
    )

    fuzz_bin = args.fuzz_bin
    if fuzz_bin is None:
        config = configparser.ConfigParser()
        configfile = os.path.abspath(os.path.dirname(__file__)) + "/../config.ini"
        config.read_file(open(configfile, encoding="utf8"))
        if not config["components"].getboolean("ENABLE_FUZZ"):
            logging.error("Must have fuzz targets built")
            sys.exit(1)
        fuzz_bin = os.path.join(
            config["environment"]["BUILDDIR"], "src", "test", "fuzz", "fuzz"
        )

    test_list_all = discover_targets(fuzz_bin)
    if not test_list_all:
        logging.error("No fuzz targets found")
        sys.exit(1)

    logging.info("Fuzz targets found (%d): %s", len(test_list_all), test_list_all)

    requested = args.target or test_list_all
    unknown = sorted(set(requested).difference(set(test_list_all)))
    if unknown:
        logging.error("Unknown fuzz targets selected: %s", unknown)
        sys.exit(1)

    test_list_selection = [t for t in requested if t in test_list_all]
    logging.info("Fuzz targets selected: %s", test_list_selection)

    libfuzzer = is_libfuzzer_build(fuzz_bin, test_list_selection[0])
    logging.info("Detected mode: %s", "libFuzzer" if libfuzzer else "standalone")

    for t in test_list_selection:
        env = os.environ.copy()
        env["FUZZ"] = t
        corpus_dir = os.path.join(args.seed_dir, t) if args.seed_dir else None

        if libfuzzer:
            cmd = [fuzz_bin]
            if args.max_total_time > 0:
                cmd.append(f"-max_total_time={args.max_total_time}")
            else:
                cmd.append("-runs=1")
            if corpus_dir and os.path.isdir(corpus_dir):
                cmd.append(corpus_dir)
            logging.debug("Run %s with cmd %s", t, cmd)
            result = subprocess.run(
                cmd, env=env, stderr=subprocess.PIPE, universal_newlines=True
            )
            logging.debug("Output: %s", result.stderr)
            result.check_returncode()
            if args.export_coverage and corpus_dir:
                for line in result.stderr.splitlines():
                    if "INITED" in line:
                        with open(
                            os.path.join(args.seed_dir, t + "_coverage"),
                            "w",
                            encoding="utf-8",
                        ) as cov_file:
                            cov_file.write(line)
                            break
        else:
            files = []
            if corpus_dir and os.path.isdir(corpus_dir):
                files = sorted(
                    os.path.join(corpus_dir, f)
                    for f in os.listdir(corpus_dir)
                    if os.path.isfile(os.path.join(corpus_dir, f))
                )
            cmd = [fuzz_bin] + files
            logging.debug("Run %s (standalone) with %d input files", t, len(files))
            result = subprocess.run(
                cmd,
                env=env,
                input=b"" if not files else None,
                stderr=subprocess.PIPE,
            )
            result.check_returncode()


if __name__ == "__main__":
    main()
