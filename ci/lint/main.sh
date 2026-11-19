#!/bin/bash

export LC_ALL=C.UTF-8
set -Eeuo pipefail

main() {
    _setup_dir_env
    trap _cleanup 0 1 2 3 6 15 ERR
    cd "$_SCRIPT_DIR/../../"

    _check_secp256k1_macros
    ./make.sh check-lints
    ./make.sh check_sh
    ./make.sh check_py
    ./make.sh check_cpp
}

# Guard against a subtree import silently dropping the secp256k1 macro definitions:
# `SECP_TRY_APPEND_CFLAGS` is defined in `bitcoin_secp.m4` and used by `configure.ac`, without
# the file the regenerated `configure` script keeps the raw m4 call and fails to parse.
_check_secp256k1_macros() {
    local macro_file="src/secp256k1/autotools-aux/m4/bitcoin_secp.m4"
    if [[ ! -f "${macro_file}" ]]; then
        echo "missing tracked macro file: ${macro_file}" >&2
        return 1
    fi
}

_setup_dir_env() {
    _WORKING_DIR="$(pwd)"
    local dir
    dir="$(dirname "${BASH_SOURCE[0]}")"
    _SCRIPT_DIR="$(cd "${dir}/" && pwd)"
}

_cleanup() {
    cd "$_WORKING_DIR"
}

main "$@"
