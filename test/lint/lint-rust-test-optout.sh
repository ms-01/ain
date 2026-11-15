#!/usr/bin/env bash
#
# Copyright (c) 2019-2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
#
# This script checks that no Rust test module is silently disabled.
#
# Test modules must fail the build when they do not compile. Opting a test
# module out with a crate level attribute (e.g. `#![cfg(test_off)]`) hides
# broken tests behind a green test report, so such patterns are rejected.

set -u

export LC_ALL=C

if [[ $# -ne 0 ]]; then
    echo "Usage: $0"
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB_DIR="${REPO_ROOT}/lib"

if [ ! -d "${LIB_DIR}" ]; then
    echo "Error: ${LIB_DIR} not found."
    exit 1
fi

# Patterns that switch off a whole test module without failing the build.
DISABLED_PATTERNS=(
    'cfg(test_off)'
    'cfg(feature = "test_off")'
    'cfg(all(test, feature = "test_off"))'
)

status=0
for pattern in "${DISABLED_PATTERNS[@]}"; do
    matches=$(grep -rn --include='*.rs' -- "${pattern}" "${LIB_DIR}" || true)
    if [ -n "${matches}" ]; then
        echo "Error: found disabled Rust test module(s) matching '${pattern}':"
        echo "${matches}"
        status=1
    fi
done

if [ ${status} -ne 0 ]; then
    echo "-> Disabled Rust test modules are not allowed: a broken test module must fail the build."
    exit ${status}
fi

echo "No disabled Rust test modules found in ${LIB_DIR}."
exit 0
