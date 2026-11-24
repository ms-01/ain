// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_CRYPTO_POLY1305_H
#define DEFI_CRYPTO_POLY1305_H

#include <span.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdint.h>
#include <stdlib.h>

#define POLY1305_KEYLEN 32
#define POLY1305_TAGLEN 16
#define POLY1305_BLOCK_SIZE 16

void poly1305_auth(unsigned char out[POLY1305_TAGLEN], const unsigned char *m, size_t inlen,
    const unsigned char key[POLY1305_KEYLEN]);

namespace poly1305_donna {

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    unsigned char buffer[POLY1305_BLOCK_SIZE];
    unsigned char final;
} poly1305_context;

void poly1305_init(poly1305_context *st, const unsigned char key[32]) noexcept;
void poly1305_update(poly1305_context *st, const unsigned char *m, size_t bytes) noexcept;
void poly1305_finish(poly1305_context *st, unsigned char mac[16]) noexcept;

} // namespace poly1305_donna

/** C++ wrapper with std::byte span interface around poly1305_donna code. */
class Poly1305
{
    poly1305_donna::poly1305_context m_ctx;

public:
    static constexpr unsigned TAGLEN{16};
    static constexpr unsigned KEYLEN{32};

    Poly1305(std::span<const std::byte> key) noexcept
    {
        assert(key.size() == KEYLEN);
        poly1305_donna::poly1305_init(&m_ctx, UCharCast(key.data()));
    }

    Poly1305& Update(std::span<const std::byte> msg) noexcept
    {
        poly1305_donna::poly1305_update(&m_ctx, UCharCast(msg.data()), msg.size());
        return *this;
    }

    void Finalize(std::span<std::byte> out) noexcept
    {
        assert(out.size() == TAGLEN);
        poly1305_donna::poly1305_finish(&m_ctx, UCharCast(out.data()));
    }
};

#endif // DEFI_CRYPTO_POLY1305_H
