#!/bin/bash
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
#
# Startup regression check for sanitizer-instrumented fuzz binaries.
#
# The sanitizer runtimes (ASan/MSan/TSan and libFuzzer) reserve a large, fixed
# address range for their primary allocator with mmap(MAP_FIXED). When the
# kernel loads the randomized address of a PIE image into such a reservation,
# the reservation clobbers the image mapping and the process dies with SIGSEGV
# before main() is reached.
#
# This script therefore asserts that
#   1. a sanitizer-instrumented fuzz binary is linked non-PIE (ET_EXEC), and
#   2. the binary starts successfully N times in a row.
#
# Usage: ci/fuzz/main.sh <fuzz-binary> [iterations]

export LC_ALL=C
set -Eeuo pipefail

readonly DEFAULT_ITERATIONS=100
readonly DEFAULT_FUZZ_TARGET="transaction"
readonly STARTUP_TIMEOUT=120

# Symbol patterns of sanitizer runtimes that reserve a fixed address range.
readonly SANITIZER_SYMBOL_PATTERN='__asan_init|__msan_init|__tsan_init|__sanitizer_cov_trace_pc_guard|LLVMFuzzerRunDriver'

usage() {
    echo "Usage: $(basename "$0") <fuzz-binary> [iterations]" >&2
}

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

# Print the numeric ELF e_type of the given binary. The value is read with
# od(1) to stay independent of the (possibly localized) readelf output, and the
# byte order is taken from the ELF header itself.
elf_type() {
    local binary="$1"
    local raw encoding byte

    raw=$(od -An -tx1 -j16 -N2 "$binary" | tr -d ' \n')
    encoding=$(od -An -tu1 -j5 -N1 "$binary" | tr -d ' \n')

    if [[ ${#raw} -ne 4 ]]; then
        fail "could not read the ELF header of $binary"
    fi

    if [[ $encoding == "1" ]]; then
        byte=${raw:0:2}
    else
        byte=${raw:2:2}
    fi

    printf '%d\n' "$((16#$byte))"
}

elf_type_name() {
    case "$1" in
        0) echo "ET_NONE" ;;
        1) echo "ET_REL" ;;
        2) echo "ET_EXEC" ;;
        3) echo "ET_DYN" ;;
        4) echo "ET_CORE" ;;
        *) echo "unknown" ;;
    esac
}

# Succeed when the binary is linked against a sanitizer runtime.
is_sanitizer_instrumented() {
    local binary="$1"
    local symbols

    symbols=$(nm -D --defined-only "$binary" 2>/dev/null || true)
    if [[ $symbols =~ $SANITIZER_SYMBOL_PATTERN ]]; then
        return 0
    fi

    symbols=$(nm --defined-only "$binary" 2>/dev/null || true)
    if [[ $symbols =~ $SANITIZER_SYMBOL_PATTERN ]]; then
        return 0
    fi

    return 1
}

main() {
    if [[ $# -lt 1 || $# -gt 2 ]]; then
        usage
        exit 1
    fi

    local binary="$1"
    local iterations="${2:-$DEFAULT_ITERATIONS}"

    if [[ ! -x $binary ]]; then
        fail "fuzz binary not found or not executable: $binary"
    fi
    if [[ ! $iterations =~ ^[0-9]+$ ]] || [[ $iterations -lt 1 ]]; then
        fail "invalid number of iterations: $iterations"
    fi

    local type type_name
    type=$(elf_type "$binary")
    type_name=$(elf_type_name "$type")

    echo "fuzz binary : $binary"
    echo "elf type    : $type ($type_name)"

    if is_sanitizer_instrumented "$binary"; then
        echo "sanitizer   : detected"
        if [[ $type != "2" ]]; then
            fail "sanitizer-instrumented binary must be linked non-PIE (ET_EXEC), got $type_name"
        fi
        echo "pie check   : ok (non-PIE)"
    else
        echo "sanitizer   : not detected, skipping the non-PIE assertion"
    fi

    # The startup crash happens in the sanitizer constructor / loader phase,
    # before any argument is parsed, so a trivial invocation is enough to
    # exercise it. A non-zero exit code (in particular a signal death) is a
    # startup failure. Standard input is redirected from /dev/null so that a
    # fuzz binary without libFuzzer (which reads its input from stdin) does not
    # block the check.
    local i rc
    local -a cmd
    for ((i = 1; i <= iterations; i++)); do
        cmd=(env "FUZZ=${FUZZ_TARGET:-$DEFAULT_FUZZ_TARGET}")
        if command -v timeout >/dev/null 2>&1; then
            cmd=(timeout "$STARTUP_TIMEOUT" "${cmd[@]}")
        fi
        cmd+=("$binary" -help=1)
        rc=0
        "${cmd[@]}" </dev/null >/dev/null 2>&1 || rc=$?
        if [[ $rc -ne 0 ]]; then
            fail "startup #$i of $iterations failed with exit code $rc"
        fi
    done

    echo "startup     : $iterations/$iterations successful"
}

main "$@"
