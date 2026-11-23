// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_UTIL_SOCK_H
#define DEFI_UTIL_SOCK_H

#include <compat.h>

#ifndef WSAEAGAIN
#define WSAEAGAIN EAGAIN
#endif

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

class CThreadInterrupt;

/**
 * Maximum time to wait for I/O readiness.
 * It will take up until this time to break off in case of an interruption.
 */
inline constexpr auto MAX_WAIT_FOR_IO = std::chrono::steady_clock::duration{std::chrono::seconds{1}};

inline bool IOErrorIsPermanent(int err)
{
    return err != WSAEAGAIN && err != WSAEINTR && err != WSAEWOULDBLOCK && err != WSAEINPROGRESS;
}

/**
 * RAII helper class that manages a socket and closes it automatically when it goes out of scope.
 */
class Sock
{
public:
    Sock() = delete;

    /**
     * Take ownership of an existent socket.
     */
    explicit Sock(SOCKET s);

    /**
     * Copy constructor, disabled because closing the same socket twice is undesirable.
     */
    Sock(const Sock&) = delete;

    /**
     * Move constructor, grab the socket from another object and close ours (if set).
     */
    Sock(Sock&& other);

    /**
     * Destructor, close the socket or do nothing if empty.
     */
    virtual ~Sock();

    /**
     * Copy assignment operator, disabled because closing the same socket twice is undesirable.
     */
    Sock& operator=(const Sock&) = delete;

    /**
     * Move assignment operator, grab the socket from another object and close ours (if set).
     */
    virtual Sock& operator=(Sock&& other);

    /**
     * send(2) wrapper. Equivalent to `send(m_socket, data, len, flags);`.
     */
    [[nodiscard]] virtual ssize_t Send(const void* data, size_t len, int flags) const;

    /**
     * recv(2) wrapper. Equivalent to `recv(m_socket, buf, len, flags);`.
     */
    [[nodiscard]] virtual ssize_t Recv(void* buf, size_t len, int flags) const;

    /**
     * connect(2) wrapper. Equivalent to `connect(m_socket, addr, addrlen)`.
     */
    [[nodiscard]] virtual int Connect(const sockaddr* addr, socklen_t addr_len) const;

    /**
     * bind(2) wrapper. Equivalent to `bind(m_socket, addr, addrlen)`.
     */
    [[nodiscard]] virtual int Bind(const sockaddr* addr, socklen_t addr_len) const;

    /**
     * listen(2) wrapper. Equivalent to `listen(m_socket, backlog)`.
     */
    [[nodiscard]] virtual int Listen(int backlog) const;

    /**
     * accept(2) wrapper. Equivalent to `std::make_unique<Sock>(accept(m_socket, addr, addr_len))`.
     * The returned unique_ptr is empty if `accept()` failed in which case errno will be set.
     */
    [[nodiscard]] virtual std::unique_ptr<Sock> Accept(sockaddr* addr, socklen_t* addr_len) const;

    /**
     * getsockopt(2) wrapper.
     */
    [[nodiscard]] virtual int GetSockOpt(int level,
                                         int opt_name,
                                         void* opt_val,
                                         socklen_t* opt_len) const;

    /**
     * setsockopt(2) wrapper.
     */
    [[nodiscard]] virtual int SetSockOpt(int level,
                                         int opt_name,
                                         const void* opt_val,
                                         socklen_t opt_len) const;

    /**
     * getsockname(2) wrapper.
     */
    [[nodiscard]] virtual int GetSockName(sockaddr* name, socklen_t* name_len) const;

    /**
     * Set the non-blocking option on the socket.
     * @return true if set successfully
     */
    [[nodiscard]] virtual bool SetNonBlocking() const;

    /**
     * Check if the underlying socket can be used for `select(2)` (or the `Wait()` method).
     * @return true if selectable
     */
    [[nodiscard]] virtual bool IsSelectable() const;

    using Event = uint8_t;

    /**
     * If passed to `Wait()`, then it will wait for readiness to read from the socket.
     */
    static constexpr Event RecvEvent = 0b001;

    /**
     * If passed to `Wait()`, then it will wait for readiness to send to the socket.
     */
    static constexpr Event SendEvent = 0b010;

    /**
     * Ignored if passed to `Wait()`, but could be set in the occurred events if an
     * exceptional condition has occurred on the socket or if it has been disconnected.
     */
    static constexpr Event ErrorEvent = 0b100;

    /**
     * Maximum timeout supported by `Wait()` and `WaitMany()` (about 24.8 days).
     */
    static constexpr std::chrono::seconds MAX_WAIT_TIMEOUT{std::numeric_limits<int>::max() / 1000};

    /**
     * Wait for readiness for input (recv) or output (send).
     * @param[in] timeout Wait this much for at least one of the requested events to occur.
     * @param[in] requested Wait for those events, bitwise-or of `RecvEvent` and `SendEvent`.
     * @param[out] occurred If not nullptr and the function returns `true`, then this
     * indicates which of the requested events occurred.
     * @return true on success (or timeout, if `occurred` of 0 is returned), false otherwise
     */
    [[nodiscard]] virtual bool Wait(std::chrono::milliseconds timeout,
                                    Event requested,
                                    Event* occurred = nullptr) const;

    /**
     * Auxiliary requested/occurred events to wait for in `WaitMany()`.
     */
    struct Events {
        explicit Events(Event req) : requested{req} {}
        Event requested;
        Event occurred{0};
    };

    struct HashSharedPtrSock {
        size_t operator()(const std::shared_ptr<const Sock>& s) const
        {
            return s ? s->m_socket : std::numeric_limits<SOCKET>::max();
        }
    };

    struct EqualSharedPtrSock {
        bool operator()(const std::shared_ptr<const Sock>& lhs,
                        const std::shared_ptr<const Sock>& rhs) const
        {
            if (lhs && rhs) {
                return lhs->m_socket == rhs->m_socket;
            }
            if (!lhs && !rhs) {
                return true;
            }
            return false;
        }
    };

    /**
     * On which socket to wait for what events in `WaitMany()`.
     */
    using EventsPerSock = std::unordered_map<std::shared_ptr<const Sock>, Events, HashSharedPtrSock, EqualSharedPtrSock>;

    /**
     * Same as `Wait()`, but wait on many sockets within the same timeout.
     * @return true on success (or timeout, if all `occurred` are returned as 0),
     * false otherwise
     */
    [[nodiscard]] virtual bool WaitMany(std::chrono::milliseconds timeout,
                                        EventsPerSock& events_per_sock) const;

    /* Higher level, convenience, methods. These may throw. */

    /**
     * Send the given data, retrying on transient errors.
     * @throws std::runtime_error if the operation cannot be completed.
     */
    virtual void SendComplete(const std::string& data,
                              std::chrono::milliseconds timeout,
                              CThreadInterrupt& interrupt) const;

    /**
     * Read from socket until a terminator character is encountered. Will never consume bytes past
     * the terminator from the socket.
     * @return The data that has been read, without the terminating character.
     * @throws std::runtime_error if the operation cannot be completed.
     */
    [[nodiscard]] virtual std::string RecvUntilTerminator(uint8_t terminator,
                                                          std::chrono::milliseconds timeout,
                                                          CThreadInterrupt& interrupt,
                                                          size_t max_data) const;

    /**
     * Check if still connected.
     * @param[out] errmsg The error string, if the socket has been disconnected.
     * @return true if connected
     */
    [[nodiscard]] virtual bool IsConnected(std::string& errmsg) const;

    /**
     * Check if the internal socket is equal to `s`. Use only in tests.
     */
    bool operator==(SOCKET s) const;

    /**
     * Get the raw socket handle.
     */
    SOCKET GetSocket() const { return m_socket; }

protected:
    /**
     * Contained socket. `INVALID_SOCKET` designates the object is empty.
     */
    SOCKET m_socket;

private:
    /**
     * Close `m_socket` if it is not `INVALID_SOCKET`.
     */
    void Close();
};

#endif // DEFI_UTIL_SOCK_H
