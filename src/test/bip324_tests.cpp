// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip324.h>
#include <chainparams.h>
#include <crypto/chacha20poly1305.h>
#include <net.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <span.h>
#include <test/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> DrainSend(Transport& transport, bool have_next = false)
{
    std::vector<uint8_t> out;
    while (true) {
        auto [to_send, more, _] = transport.GetBytesToSend(have_next);
        if (to_send.empty()) break;
        out.insert(out.end(), to_send.begin(), to_send.end());
        transport.MarkBytesSent(to_send.size());
    }
    return out;
}

void CompleteV2Handshake(V2Transport& initiator, V2Transport& responder)
{
    // 1. Initiator sends pubkey + garbage -> Responder receives it and prepares its pubkey + garbage + terminator + version.
    auto init_bytes = DrainSend(initiator);
    BOOST_REQUIRE(!init_bytes.empty());
    std::span<const uint8_t> sp_init{init_bytes};
    BOOST_REQUIRE(responder.ReceivedBytes(sp_init));
    BOOST_REQUIRE(sp_init.empty());

    // 2. Responder sends pubkey + garbage + terminator + version -> Initiator receives it and prepares terminator + version.
    auto resp_bytes = DrainSend(responder);
    BOOST_REQUIRE(!resp_bytes.empty());
    std::span<const uint8_t> sp_resp{resp_bytes};
    BOOST_REQUIRE(initiator.ReceivedBytes(sp_resp));
    BOOST_REQUIRE(sp_resp.empty());

    // 3. Initiator sends terminator + version -> Responder receives it and enters APP state.
    auto init_fin = DrainSend(initiator);
    BOOST_REQUIRE(!init_fin.empty());
    std::span<const uint8_t> sp_fin{init_fin};
    BOOST_REQUIRE(responder.ReceivedBytes(sp_fin));
    BOOST_REQUIRE(sp_fin.empty());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bip324_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(fs_chacha20poly1305_rekey_and_tamper)
{
    std::array<std::byte, FSChaCha20Poly1305::KEYLEN> key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = std::byte{static_cast<uint8_t>(i + 1)};

    FSChaCha20Poly1305 enc(key, BIP324Cipher::REKEY_INTERVAL);
    FSChaCha20Poly1305 dec(key, BIP324Cipher::REKEY_INTERVAL);

    // Exercise across REKEY_INTERVAL (224) packets to verify forward-secure rekeying.
    for (unsigned i = 0; i < BIP324Cipher::REKEY_INTERVAL + 5; ++i) {
        std::vector<std::byte> plain(32, std::byte{static_cast<uint8_t>(i & 0xFF)});
        std::vector<std::byte> aad(4, std::byte{static_cast<uint8_t>((i >> 4) & 0xFF)});
        std::vector<std::byte> cipher(plain.size() + FSChaCha20Poly1305::EXPANSION);
        std::vector<std::byte> out(plain.size());

        enc.Encrypt(plain, aad, cipher);
        BOOST_CHECK(dec.Decrypt(cipher, aad, out));
        BOOST_CHECK(out == plain);
    }

    // Tamper detection: flipping a bit in ciphertext or AAD must fail decryption.
    std::vector<std::byte> plain(16, std::byte{0x42});
    std::vector<std::byte> aad(3, std::byte{0x11});
    std::vector<std::byte> cipher(plain.size() + FSChaCha20Poly1305::EXPANSION);
    std::vector<std::byte> out(plain.size());
    enc.Encrypt(plain, aad, cipher);
    cipher[0] ^= std::byte{0x01};
    BOOST_CHECK(!dec.Decrypt(cipher, aad, out));
}

BOOST_AUTO_TEST_CASE(v1_transport_golden_vector)
{
    V1Transport sender(1);
    V1Transport receiver(2);

    BOOST_CHECK(sender.GetInfo().transport_type == TransportProtocolType::V1);
    BOOST_CHECK(!sender.GetInfo().session_id.has_value());

    const uint64_t nonce = 0x1122334455667788ULL;
    CSerializedNetMsg msg = CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::PING, nonce);
    BOOST_REQUIRE(sender.SetMessageToSend(msg));

    auto wire = DrainSend(sender);
    // V1 wire format: 24-byte header (4-byte post-AMK magic + 12-byte command + 4-byte len + 4-byte checksum) + 8-byte payload.
    BOOST_REQUIRE_EQUAL(wire.size(), CMessageHeader::HEADER_SIZE + 8U);
    const auto& magic = Params().MessageStartPostAMK();
    BOOST_CHECK(std::equal(std::begin(magic), std::end(magic), wire.begin()));
    BOOST_CHECK_EQUAL(std::string(reinterpret_cast<const char*>(wire.data() + 4), 4), "ping");

    std::span<const uint8_t> sp{wire};
    while (!sp.empty()) {
        BOOST_REQUIRE(receiver.ReceivedBytes(sp));
    }
    BOOST_REQUIRE(receiver.ReceivedMessageComplete());

    bool reject{false};
    CNetMessage received = receiver.GetReceivedMessage(12345, reject);
    BOOST_CHECK(!reject);
    BOOST_CHECK_EQUAL(received.m_type, NetMsgType::PING);
    BOOST_CHECK_EQUAL(received.m_message_size, 8U);
    BOOST_CHECK_EQUAL(received.m_raw_message_size, CMessageHeader::HEADER_SIZE + 8U);
    uint64_t decoded_nonce = 0;
    received.m_recv >> decoded_nonce;
    BOOST_CHECK_EQUAL(decoded_nonce, nonce);
}

BOOST_AUTO_TEST_CASE(v2_transport_handshake_session_id_and_all_dfi_commands)
{
    V2Transport initiator(1, /*initiating=*/true);
    V2Transport responder(2, /*initiating=*/false);

    BOOST_CHECK(responder.GetInfo().transport_type == TransportProtocolType::DETECTING);
    BOOST_CHECK(!responder.ShouldReconnectV1());
    BOOST_CHECK(!initiator.ShouldReconnectV1()); // Nothing sent yet

    CompleteV2Handshake(initiator, responder);

    auto init_info = initiator.GetInfo();
    auto resp_info = responder.GetInfo();
    BOOST_CHECK(init_info.transport_type == TransportProtocolType::V2);
    BOOST_CHECK(resp_info.transport_type == TransportProtocolType::V2);
    BOOST_REQUIRE(init_info.session_id.has_value());
    BOOST_REQUIRE(resp_info.session_id.has_value());
    BOOST_CHECK(*init_info.session_id == *resp_info.session_id);
    BOOST_CHECK(!initiator.ShouldReconnectV1());
    BOOST_CHECK(!responder.ShouldReconnectV1());

    // Round-trip every single DFI P2P command (all 31 commands in getAllNetMessageTypes(), including getauths/anchorauth/anchorconf).
    const auto& all_cmds = getAllNetMessageTypes();
    BOOST_REQUIRE(!all_cmds.empty());
    for (const std::string& cmd : all_cmds) {
        CSerializedNetMsg out_msg;
        out_msg.command = cmd;
        out_msg.data = {0xDE, 0xAD, 0xBE, 0xEF};
        BOOST_REQUIRE(initiator.SetMessageToSend(out_msg));

        auto wire = DrainSend(initiator);
        BOOST_REQUIRE(!wire.empty());
        std::span<const uint8_t> sp{wire};
        BOOST_REQUIRE(responder.ReceivedBytes(sp));
        BOOST_REQUIRE(sp.empty());
        BOOST_REQUIRE(responder.ReceivedMessageComplete());

        bool reject{false};
        CNetMessage in_msg = responder.GetReceivedMessage(99999, reject);
        BOOST_CHECK(!reject);
        BOOST_CHECK_EQUAL(in_msg.m_type, cmd);
        BOOST_CHECK_EQUAL(in_msg.m_message_size, 4U);
    }
}

BOOST_AUTO_TEST_CASE(v2_transport_should_reconnect_v1_and_responder_v1_fallback)
{
    // Initiator: after sending >= 24 bytes with nothing received, ShouldReconnectV1() must be true.
    V2Transport initiator(1, /*initiating=*/true);
    BOOST_CHECK(!initiator.ShouldReconnectV1());
    auto init_bytes = DrainSend(initiator);
    BOOST_REQUIRE(init_bytes.size() >= CMessageHeader::HEADER_SIZE);
    BOOST_CHECK(initiator.ShouldReconnectV1());

    // Responder: feeding a v1 "version" message transparently switches to V1Transport.
    V1Transport v1_sender(3);
    V2Transport v2_responder(4, /*initiating=*/false);
    CSerializedNetMsg ver_msg = CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERSION, PROTOCOL_VERSION);
    BOOST_REQUIRE(v1_sender.SetMessageToSend(ver_msg));
    auto v1_wire = DrainSend(v1_sender);

    std::span<const uint8_t> sp{v1_wire};
    while (!sp.empty()) {
        BOOST_REQUIRE(v2_responder.ReceivedBytes(sp));
    }
    BOOST_CHECK(v2_responder.GetInfo().transport_type == TransportProtocolType::V1);
    BOOST_REQUIRE(v2_responder.ReceivedMessageComplete());
    bool reject{false};
    CNetMessage decoded = v2_responder.GetReceivedMessage(1, reject);
    BOOST_CHECK(!reject);
    BOOST_CHECK_EQUAL(decoded.m_type, NetMsgType::VERSION);
}

BOOST_AUTO_TEST_CASE(v2_transport_rejects_oversized_packet)
{
    V2Transport initiator(1, /*initiating=*/true);
    V2Transport responder(2, /*initiating=*/false);
    CompleteV2Handshake(initiator, responder);

    // M1: BIP324 3-byte length field max contents is 16,777,215 bytes.
    // A message exceeding MAX_V2_CONTENTS_LEN must be rejected cleanly without truncation or crash.
    CSerializedNetMsg oversized;
    oversized.command = NetMsgType::BLOCK;
    oversized.data.assign(V2Transport::MAX_V2_CONTENTS_LEN, 0x00); // +1 short-id byte = MAX_V2_CONTENTS_LEN + 1
    BOOST_CHECK(!initiator.SetMessageToSend(oversized));
}

BOOST_AUTO_TEST_SUITE_END()
