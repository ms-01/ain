// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_CRYPTO_CHACHA20_H
#define DEFI_CRYPTO_CHACHA20_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdint.h>
#include <stdlib.h>
#include <utility>

/** A class for ChaCha20 256-bit stream cipher developed by Daniel J. Bernstein
    https://cr.yp.to/chacha/chacha-20080128.pdf */
class ChaCha20
{
private:
    uint32_t input[16];

public:
    ChaCha20();
    ChaCha20(const unsigned char* key, size_t keylen);
    void SetKey(const unsigned char* key, size_t keylen); //!< set key with flexible keylength; 256bit recommended */
    void SetIV(uint64_t iv); // set the 64bit nonce
    void Seek(uint64_t pos); // set the 64bit block counter

    /** outputs the keystream of size <bytes> into <c> */
    void Keystream(unsigned char* c, size_t bytes);

    /** enciphers the message <input> of length <bytes> and write the enciphered representation into <output>
     *  Used for encryption and decryption (XOR)
     */
    void Crypt(const unsigned char* input, unsigned char* output, size_t bytes);
};

/** ChaCha20 cipher that only operates on multiples of 64 bytes (RFC8439 96-bit nonce + 32-bit counter). */
class ChaCha20Aligned
{
private:
    uint32_t input[12];

public:
    static constexpr unsigned KEYLEN{32};
    static constexpr unsigned BLOCKLEN{64};

    ChaCha20Aligned() noexcept = delete;
    ChaCha20Aligned(std::span<const std::byte> key) noexcept;
    ~ChaCha20Aligned();

    void SetKey(std::span<const std::byte> key) noexcept;

    using Nonce96 = std::pair<uint32_t, uint64_t>;
    void Seek(Nonce96 nonce, uint32_t block_counter) noexcept;
    void Keystream(std::span<std::byte> out) noexcept;
    void Crypt(std::span<const std::byte> input, std::span<std::byte> output) noexcept;
};

/** Unrestricted RFC8439 ChaCha20 cipher with std::byte span interface. */
class ChaCha20Stream
{
private:
    ChaCha20Aligned m_aligned;
    std::array<std::byte, ChaCha20Aligned::BLOCKLEN> m_buffer;
    unsigned m_bufleft{0};

public:
    static constexpr unsigned KEYLEN = ChaCha20Aligned::KEYLEN;

    ChaCha20Stream() noexcept = delete;
    ChaCha20Stream(std::span<const std::byte> key) noexcept : m_aligned(key) {}
    ~ChaCha20Stream();

    void SetKey(std::span<const std::byte> key) noexcept;
    using Nonce96 = ChaCha20Aligned::Nonce96;

    void Seek(Nonce96 nonce, uint32_t block_counter) noexcept
    {
        m_aligned.Seek(nonce, block_counter);
        m_bufleft = 0;
    }

    void Crypt(std::span<const std::byte> in_bytes, std::span<std::byte> out_bytes) noexcept;
    void Keystream(std::span<std::byte> out) noexcept;
};

/** Forward-secure ChaCha20 (BIP324). */
class FSChaCha20
{
private:
    ChaCha20Stream m_chacha20;
    const uint32_t m_rekey_interval;
    uint32_t m_chunk_counter{0};
    uint64_t m_rekey_counter{0};

public:
    static constexpr unsigned KEYLEN = 32;

    FSChaCha20(const FSChaCha20&) = delete;
    FSChaCha20(FSChaCha20&&) = delete;
    FSChaCha20& operator=(const FSChaCha20&) = delete;
    FSChaCha20& operator=(FSChaCha20&&) = delete;

    FSChaCha20(std::span<const std::byte> key, uint32_t rekey_interval) noexcept;
    void Crypt(std::span<const std::byte> input, std::span<std::byte> output) noexcept;
};

#endif // DEFI_CRYPTO_CHACHA20_H
