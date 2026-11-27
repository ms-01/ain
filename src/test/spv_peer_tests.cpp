// Copyright (c) 2026 The DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <test/setup_common.h>
#include <spv/bitcoin/BRPeer.h>

#include <boost/test/unit_test.hpp>

#ifndef WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#endif

BOOST_FIXTURE_TEST_SUITE(spv_peer_tests, BasicTestingSetup)

#ifndef WIN32
namespace {

struct FdGuard {
    std::vector<int> fds;

    void Add(int fd)
    {
        if (fd >= 0) fds.push_back(fd);
    }

    ~FdGuard()
    {
        for (int fd : fds) {
            close(fd);
        }
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(spv_peer_wait_socket_writable_high_fd)
{
    struct rlimit rl {};
    BOOST_REQUIRE_EQUAL(getrlimit(RLIMIT_NOFILE, &rl), 0);
    if (rl.rlim_cur <= 1040) {
        rlim_t target = 2048;
        if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max) {
            target = rl.rlim_max;
        }
        rl.rlim_cur = target;
        BOOST_REQUIRE_EQUAL(setrlimit(RLIMIT_NOFILE, &rl), 0);
    }
    BOOST_REQUIRE_GT(rl.rlim_cur, static_cast<rlim_t>(1024));

    FdGuard guard;

    // Create a loopback listening socket to accept the non-blocking connect.
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE_GE(listen_fd, 0);
    guard.Add(listen_fd);

    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in bind_addr {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    BOOST_REQUIRE_EQUAL(bind(listen_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    BOOST_REQUIRE_EQUAL(listen(listen_fd, 4), 0);

    socklen_t addr_len = sizeof(bind_addr);
    BOOST_REQUIRE_EQUAL(getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), &addr_len), 0);

    // Exhaust file descriptors below 1024 so the client socket receives fd >= 1024.
    int devnull = open("/dev/null", O_RDONLY);
    BOOST_REQUIRE_GE(devnull, 0);
    guard.Add(devnull);

    int highest_fd = devnull;
    while (highest_fd < 1024) {
        int dup_fd = dup(devnull);
        BOOST_REQUIRE_GE(dup_fd, 0);
        guard.Add(dup_fd);
        highest_fd = dup_fd;
    }

    int client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE_GE(client_fd, 1024);
    guard.Add(client_fd);

    int flags = fcntl(client_fd, F_GETFL, 0);
    BOOST_REQUIRE_GE(flags, 0);
    BOOST_REQUIRE_EQUAL(fcntl(client_fd, F_SETFL, flags | O_NONBLOCK), 0);

    int conn_res = connect(client_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr));
    BOOST_REQUIRE(conn_res == 0 || errno == EINPROGRESS);

    // Must not trigger __fdelt_chk / SIGABRT for fd >= 1024.
    int wait_res = BRPeerWaitSocketWritable(client_fd, 2.0);
    BOOST_CHECK_GT(wait_res, 0);

    int so_err = -1;
    socklen_t opt_len = sizeof(so_err);
    BOOST_REQUIRE_EQUAL(getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &so_err, &opt_len), 0);
    BOOST_CHECK_EQUAL(so_err, 0);
}

BOOST_AUTO_TEST_CASE(spv_peer_wait_socket_writable_timeout)
{
    int sv[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    FdGuard guard;
    guard.Add(sv[0]);
    guard.Add(sv[1]);

    int flags = fcntl(sv[0], F_GETFL, 0);
    BOOST_REQUIRE_GE(flags, 0);
    BOOST_REQUIRE_EQUAL(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);

    // Fill the send buffer so sv[0] is not writable.
    std::vector<char> buf(4096, 'x');
    for (;;) {
        ssize_t n = send(sv[0], buf.data(), buf.size(), MSG_DONTWAIT);
        if (n < 0) {
            BOOST_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
    }

    BOOST_CHECK_EQUAL(BRPeerWaitSocketWritable(sv[0], 0.05), 0);
    BOOST_CHECK_EQUAL(BRPeerWaitSocketWritable(sv[0], 0.0), 0);
    BOOST_CHECK_EQUAL(BRPeerWaitSocketWritable(sv[0], -1.0), 0);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
