// Copyright (c) 2024-present The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_I2P_H
#define DEFI_I2P_H

#include <compat.h>
#include <fs.h>
#include <netaddress.h>
#include <netbase.h>
#include <sync.h>
#include <threadinterrupt.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace i2p {

/**
 * Binary data.
 */
using Binary = std::vector<uint8_t>;

/**
 * Swap Standard Base64 <-> I2P Base64.
 *
 * Standard Base64 uses `+` and `/` as last two characters of its alphabet,
 * I2P Base64 uses `-` and `~` respectively.
 *
 * @param[in] from Input to convert.
 * @return converted `from`
 */
std::string SwapBase64(const std::string& from);

/**
 * Decode an I2P-style Base64 string.
 *
 * @param[in] i2p_b64 I2P-style Base64 string.
 * @return decoded `i2p_b64`
 * @throws std::runtime_error if decoding fails
 */
Binary DecodeI2PBase64(const std::string& i2p_b64);

/**
 * Derive the .b32.i2p address of an I2P destination (binary).
 *
 * @param[in] dest I2P destination.
 * @return the address that corresponds to `dest`
 * @throws std::runtime_error if conversion fails
 */
CNetAddr DestBinToAddr(const Binary& dest);

/**
 * Derive the .b32.i2p address of an I2P destination (I2P-style Base64).
 *
 * @param[in] dest I2P destination.
 * @return the address that corresponds to `dest`
 * @throws std::runtime_error if conversion fails
 */
CNetAddr DestB64ToAddr(const std::string& dest);

/**
 * Normalize the port of an I2P service.
 *
 * I2P destinations are portless (the SAM v3.1 protocol has no notion of a
 * destination port), so the port of an I2P CService is always 0. The
 * normalization has to happen before any CService comparison, so that
 * "<addr>" and "<addr>:1234" refer to the same peer.
 *
 * @param[in] service The service to normalize.
 * @return the normalized service (unchanged if it is not an I2P service)
 */
CService NormalizeI2PService(const CService& service);

/**
 * An established connection with another peer.
 */
struct Connection {
    /** Connected socket. */
    SOCKET sock{INVALID_SOCKET};

    /** Our I2P address. */
    CService me;

    /** The peer's I2P address. */
    CService peer;
};

namespace sam {

/**
 * A reply from the SAM proxy.
 */
struct Reply {
    /**
     * Full, unparsed reply.
     */
    std::string full;

    /**
     * Request, used for detailed error reporting.
     */
    std::string request;

    /**
     * A map of keywords from the parsed reply.
     * For example, if the reply is "A=X B C=YZ", then the map will be
     * keys["A"] == "X"
     * keys["B"] == (empty std::optional)
     * keys["C"] == "YZ"
     */
    std::unordered_map<std::string, std::optional<std::string>> keys;

    /**
     * Whether the given key is present in the reply.
     */
    bool Has(const std::string& key) const;

    /**
     * Get the value of a given key.
     * For example if the reply is "A=X B" then:
     * Get("A") -> "X"
     * Get("B") -> throws
     * Get("C") -> throws
     * @param[in] key Key whose value to retrieve
     * @returns the key's value
     * @throws std::runtime_error if the key is not present or if it has no value
     */
    std::string Get(const std::string& key) const;
};

/**
 * The maximum size of an incoming message from the I2P SAM proxy (in bytes).
 * Used to avoid a runaway proxy from sending us an "unlimited" amount of
 * data without a terminator. The longest known message is ~1400 bytes, so
 * this is high enough not to be triggered during normal operation, yet low
 * enough to avoid a malicious proxy from filling our memory.
 */
static constexpr size_t MAX_MSG_SIZE{65536};

/**
 * I2P destinations are portless: the SAM v3.1 protocol has no notion of a
 * destination port. The port of an I2P CService is therefore always
 * normalized to this value.
 */
static constexpr uint16_t I2P_SAM31_PORT{0};

/**
 * I2P SAM session.
 *
 * The session talks the SAM v3.1 protocol to a SAM bridge ("SAM proxy"):
 *
 *   HELLO VERSION MIN=3.1 MAX=3.1
 *   DEST GENERATE SIGNATURE_TYPE=7
 *   SESSION CREATE STYLE=STREAM ID=<id> DESTINATION=<b64>
 *   NAMING LOOKUP NAME=<b32.i2p>
 *   STREAM CONNECT ID=<id> DESTINATION=<b64> SILENT=false
 *   STREAM ACCEPT ID=<id> SILENT=false
 *
 * @see https://i2p.net/en/docs/api/samv3
 */
class Session
{
public:
    /**
     * Construct a session. This will not initiate any IO, the session is
     * lazily created later when it is first used.
     * @param[in] private_key_file Path to a private key file. If the file does
     * not exist then the private key is generated and saved into the file.
     * @param[in] control_host Location of the SAM bridge.
     * @param[in] interrupt If this is signaled then all operations are
     * canceled as soon as possible and executing methods throw an exception.
     */
    Session(const fs::path& private_key_file, const CService& control_host, CThreadInterrupt* interrupt);

    /**
     * Destroy the session, closing the internally used sockets. The sockets
     * that have been returned by `Accept()` or `Connect()` are not closed,
     * but they are closed by the SAM bridge because the session is destroyed.
     * So they will return an error next time we try to read or write to them.
     */
    ~Session();

    /**
     * Start listening for an incoming connection.
     * @param[out] conn Upon successful completion the `sock` and `me` members
     * are set to the listening socket and address.
     * @return true on success
     */
    bool Listen(Connection& conn);

    /**
     * Wait for and accept a new incoming connection.
     * @param[in,out] conn The `sock` member is used for waiting and accepting.
     * Upon successful completion the `peer` member will be set to the address
     * of the incoming peer.
     * @return true on success
     */
    bool Accept(Connection& conn);

    /**
     * Connect to an I2P peer.
     * @param[in] to Peer to connect to. The port is ignored (I2P destinations
     * are portless), the destination is normalized to port 0.
     * @param[out] conn Established connection. Only set if `true` is returned.
     * @param[out] proxy_error If an error occurs due to a proxy or general
     * network failure, then this is set to `true`. If an error occurs due to
     * an unreachable peer (likely the peer is down), then it is set to
     * `false`. Only set if `false` is returned.
     * @return true on success
     */
    bool Connect(const CService& to, Connection& conn, bool& proxy_error);

private:
    /**
     * Send request and get a reply from the SAM proxy.
     * @param[in] sock A socket that is connected to the SAM proxy.
     * @param[in] request Raw request to send, a newline terminator is appended to it.
     * @param[in] check_result_ok If true then after receiving the reply a check
     * is made whether it contains "RESULT=OK" and an exception is thrown if it
     * does not.
     * @throws std::runtime_error if an error occurs
     */
    Reply SendRequestAndGetReply(SOCKET sock, const std::string& request, bool check_result_ok = true) const;

    /**
     * Open a new connection to the SAM proxy.
     * @return a connected socket
     * @throws std::runtime_error if an error occurs
     */
    SOCKET Hello() const;

    /**
     * Check the control socket for errors and possibly disconnect.
     */
    void CheckControlSock();

    /**
     * Generate a new destination with the SAM proxy and set `m_private_key` to it.
     * @param[in] sock Socket to use for talking to the SAM proxy.
     * @throws std::runtime_error if an error occurs
     */
    void DestGenerate(SOCKET sock);

    /**
     * Generate a new destination with the SAM proxy, set `m_private_key` to it
     * and save it on disk to `m_private_key_file`.
     * @param[in] sock Socket to use for talking to the SAM proxy.
     * @throws std::runtime_error if an error occurs
     */
    void GenerateAndSavePrivateKey(SOCKET sock);

    /**
     * Derive own destination from `m_private_key`.
     * @see https://i2p.net/en/docs/specs/common-structures/#destination
     * @return an I2P destination
     */
    Binary MyDestination() const;

    /**
     * Create the session if not already created. Reads the private key file and
     * connects to the SAM proxy.
     * @throws std::runtime_error if an error occurs
     */
    void CreateIfNotCreatedAlready();

    /**
     * Open a new connection to the SAM proxy and issue a "STREAM ACCEPT"
     * request using the existing session id.
     * @return the idle socket that is waiting for a peer to connect to us
     * @throws std::runtime_error if an error occurs
     */
    SOCKET StreamAccept();

    /**
     * Destroy the session, closing the internally used sockets.
     */
    void Disconnect();

    /**
     * The name of the file where this peer's private key is stored (in binary).
     */
    const fs::path m_private_key_file;

    /**
     * The SAM control service proxy.
     */
    const CService m_control_host;

    /**
     * Cease network activity when this is signaled.
     */
    CThreadInterrupt* const m_interrupt;

    /**
     * Mutex protecting the members that can be concurrently accessed.
     */
    mutable Mutex m_mutex;

    /**
     * The private key of this peer.
     * @see The reply to the "DEST GENERATE" command in
     * https://i2p.net/en/docs/api/samv3
     */
    Binary m_private_key;

    /**
     * SAM control socket.
     * Used to connect to the I2P SAM service and create a session
     * ("SESSION CREATE"). With the established session id we later open
     * other connections to the SAM service to accept incoming I2P
     * connections and make outgoing ones.
     * If not connected then this will be INVALID_SOCKET.
     * See https://i2p.net/en/docs/api/samv3
     */
    SOCKET m_control_sock{INVALID_SOCKET};

    /**
     * Our .b32.i2p address.
     * Derived from `m_private_key`.
     */
    CService m_my_addr;

    /**
     * SAM session id.
     */
    std::string m_session_id;
};

} // namespace sam
} // namespace i2p

#endif // DEFI_I2P_H
