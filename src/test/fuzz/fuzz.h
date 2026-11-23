// Copyright (c) 2009-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_TEST_FUZZ_FUZZ_H
#define DEFI_TEST_FUZZ_FUZZ_H

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

/**
 * Can be used to limit a theoretically unbounded loop. This caps the runtime
 * of a single fuzz iteration to avoid timeouts on pathological inputs.
 */
#define LIMITED_WHILE(condition, limit) \
    for (unsigned _count{limit}; (condition) && _count; --_count)

using FuzzBufferType = std::span<const uint8_t>;
using TypeTestOneInput = std::function<void(FuzzBufferType)>;
using TypeInitialize = std::function<void()>;

struct FuzzTargetOptions {
    TypeInitialize init{[] {}};
};

void FuzzFrameworkRegisterTarget(std::string_view name, TypeTestOneInput target, FuzzTargetOptions opts);

#define FUZZ_TARGET(name, ...)                                                      \
    static void name##_fuzz_target(FuzzBufferType);                                 \
    struct name##_Before_Main {                                                     \
        name##_Before_Main()                                                        \
        {                                                                           \
            FuzzFrameworkRegisterTarget(#name, name##_fuzz_target, {__VA_ARGS__});  \
        }                                                                           \
    } const static g_##name##_before_main;                                          \
    static void name##_fuzz_target(FuzzBufferType buffer)

#endif // DEFI_TEST_FUZZ_FUZZ_H
