// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_CRYPTO_CHACHA20POLY1305_H
#define DEFI_CRYPTO_CHACHA20POLY1305_H

#include <cstddef>
#include <cstdint>
#include <span>

#include <crypto/chacha20.h>
#include <crypto/poly1305.h>

/** The AEAD_CHACHA20_POLY1305 authenticated encryption algorithm from RFC8439 section 2.8. */
class AEADChaCha20Poly1305
{
    ChaCha20Stream m_chacha20;

public:
    static constexpr unsigned KEYLEN = 32;
    static constexpr unsigned EXPANSION = Poly1305::TAGLEN;

    AEADChaCha20Poly1305(std::span<const std::byte> key) noexcept;
    void SetKey(std::span<const std::byte> key) noexcept;

    using Nonce96 = ChaCha20Stream::Nonce96;

    void Encrypt(std::span<const std::byte> plain, std::span<const std::byte> aad, Nonce96 nonce, std::span<std::byte> cipher) noexcept
    {
        Encrypt(plain, {}, aad, nonce, cipher);
    }

    void Encrypt(std::span<const std::byte> plain1, std::span<const std::byte> plain2, std::span<const std::byte> aad, Nonce96 nonce, std::span<std::byte> cipher) noexcept;

    bool Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, Nonce96 nonce, std::span<std::byte> plain) noexcept
    {
        return Decrypt(cipher, aad, nonce, plain, {});
    }

    bool Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, Nonce96 nonce, std::span<std::byte> plain1, std::span<std::byte> plain2) noexcept;

    void Keystream(Nonce96 nonce, std::span<std::byte> keystream) noexcept;
};

/** Forward-secure wrapper around AEADChaCha20Poly1305 (BIP324). */
class FSChaCha20Poly1305
{
private:
    AEADChaCha20Poly1305 m_aead;
    const uint32_t m_rekey_interval;
    uint32_t m_packet_counter{0};
    uint64_t m_rekey_counter{0};

    void NextPacket() noexcept;

public:
    static constexpr auto KEYLEN = AEADChaCha20Poly1305::KEYLEN;
    static constexpr auto EXPANSION = AEADChaCha20Poly1305::EXPANSION;

    FSChaCha20Poly1305(const FSChaCha20Poly1305&) = delete;
    FSChaCha20Poly1305(FSChaCha20Poly1305&&) = delete;
    FSChaCha20Poly1305& operator=(const FSChaCha20Poly1305&) = delete;
    FSChaCha20Poly1305& operator=(FSChaCha20Poly1305&&) = delete;

    FSChaCha20Poly1305(std::span<const std::byte> key, uint32_t rekey_interval) noexcept :
        m_aead(key), m_rekey_interval(rekey_interval) {}

    void Encrypt(std::span<const std::byte> plain, std::span<const std::byte> aad, std::span<std::byte> cipher) noexcept
    {
        Encrypt(plain, {}, aad, cipher);
    }

    void Encrypt(std::span<const std::byte> plain1, std::span<const std::byte> plain2, std::span<const std::byte> aad, std::span<std::byte> cipher) noexcept;

    bool Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, std::span<std::byte> plain) noexcept
    {
        return Decrypt(cipher, aad, plain, {});
    }

    bool Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, std::span<std::byte> plain1, std::span<std::byte> plain2) noexcept;
};

#endif // DEFI_CRYPTO_CHACHA20POLY1305_H
