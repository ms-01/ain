// Copyright (c) 2009-2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/fuzz.h>

#include <pubkey.h>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct RegisteredTarget {
    TypeTestOneInput target;
    FuzzTargetOptions opts;
};

std::map<std::string_view, RegisteredTarget>& FuzzTargets()
{
    static std::map<std::string_view, RegisteredTarget> g_fuzz_targets;
    return g_fuzz_targets;
}

const RegisteredTarget* g_active_target{nullptr};

bool read_stdin(std::vector<uint8_t>& data)
{
    uint8_t buffer[1024];
    ssize_t length = 0;
    while ((length = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        data.insert(data.end(), buffer, buffer + length);
        if (data.size() > (1 << 20)) return false;
    }
    return length == 0;
}

bool read_file(const std::string& path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open input file: " << path << '\n';
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

void initialize()
{
    if (std::getenv("PRINT_ALL_FUZZ_TARGETS_AND_ABORT")) {
        for (const auto& [name, _] : FuzzTargets()) {
            std::cout << name << '\n';
        }
        std::exit(EXIT_SUCCESS);
    }

    const char* fuzz_env = std::getenv("FUZZ");
    if (!fuzz_env || fuzz_env[0] == '\0') {
        std::cerr << "Must select fuzz target with the FUZZ environment variable.\n"
                  << "Set PRINT_ALL_FUZZ_TARGETS_AND_ABORT=1 to list all available targets.\n";
        std::exit(EXIT_FAILURE);
    }

    const std::string_view target_name{fuzz_env};
    const auto it = FuzzTargets().find(target_name);
    if (it == FuzzTargets().end()) {
        std::cerr << "Unknown fuzz target: " << target_name << '\n';
        std::exit(EXIT_FAILURE);
    }

    static const auto verify_handle = std::make_unique<ECCVerifyHandle>();
    g_active_target = &it->second;
    g_active_target->opts.init();
}

} // namespace

void FuzzFrameworkRegisterTarget(std::string_view name, TypeTestOneInput target, FuzzTargetOptions opts)
{
    const auto [it, inserted] = FuzzTargets().try_emplace(name, RegisteredTarget{std::move(target), std::move(opts)});
    assert(inserted && "Duplicate fuzz target name");
}

// Used by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    assert(g_active_target != nullptr);
    g_active_target->target({data, size});
    return 0;
}

// Used by libFuzzer
extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    initialize();
    return 0;
}

// Disabled under WIN32 due to clash with Cygwin's WinMain.
#ifndef WIN32
// Declare main(...) "weak" to allow for libFuzzer linking. libFuzzer provides
// the main(...) function.
__attribute__((weak))
#endif
int main(int argc, char** argv)
{
    initialize();
#ifdef __AFL_INIT
    __AFL_INIT();
#endif

    std::vector<std::string> input_files;
    for (int i = 1; i < argc; ++i) {
        std::string arg{argv[i]};
        if (!arg.empty() && arg[0] == '-') {
            continue;
        }
        input_files.push_back(std::move(arg));
    }

    if (!input_files.empty()) {
        for (const auto& path : input_files) {
            std::vector<uint8_t> buffer;
            if (!read_file(path, buffer)) {
                return EXIT_FAILURE;
            }
            g_active_target->target(buffer);
        }
        return EXIT_SUCCESS;
    }

#ifdef __AFL_LOOP
    while (__AFL_LOOP(1000)) {
        std::vector<uint8_t> buffer;
        if (!read_stdin(buffer)) {
            continue;
        }
        g_active_target->target(buffer);
    }
#else
    std::vector<uint8_t> buffer;
    if (!read_stdin(buffer)) {
        return EXIT_SUCCESS;
    }
    g_active_target->target(buffer);
#endif
    return EXIT_SUCCESS;
}
