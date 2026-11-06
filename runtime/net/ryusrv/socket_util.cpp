// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: MIT

#include <cerrno>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "socket_util.h"

namespace Network::RyuLdn {

namespace {
#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() {
        WSACleanup();
    }
};
#endif
} // namespace

void EnsureSockets() {
#ifdef _WIN32
    static WinsockInit winsock;
#endif
}

#ifdef _WIN32
void CloseSocket(std::uintptr_t s) {
    closesocket(static_cast<SOCKET>(s));
}

void ShutdownSocket(std::uintptr_t s) {
    shutdown(static_cast<SOCKET>(s), SD_BOTH);
}

int LastSocketError() {
    return WSAGetLastError();
}
#else
void CloseSocket(std::uintptr_t s) {
    close(static_cast<int>(s));
}

void ShutdownSocket(std::uintptr_t s) {
    shutdown(static_cast<int>(s), SHUT_RDWR);
}

int LastSocketError() {
    return errno;
}
#endif

/// TCP connect with a timeout; kInvalidSocket + message on failure.
std::uintptr_t ConnectTcp(const std::string& host, u16 port, int timeout_ms, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET; // the master servers are IPv4
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
        error = fmt::format("can't resolve {}", host);
        return kInvalidSocket;
    }

    std::uintptr_t found = kInvalidSocket;
    for (addrinfo* ai = result; ai && found == kInvalidSocket; ai = ai->ai_next) {
#ifdef _WIN32
        const SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        const int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = rc == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            ok = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(s, &writable);
        }
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
        if (!ok) {
            closesocket(s);
            continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                   sizeof(nodelay));
        found = static_cast<std::uintptr_t>(s);
#else
        const int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) {
            continue;
        }
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        const int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        bool ok = rc == 0;
        if (!ok && errno == EINPROGRESS) {
            pollfd pfd{s, POLLOUT, 0};
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            ok = poll(&pfd, 1, timeout_ms) > 0 &&
                 getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0;
        }
        fcntl(s, F_SETFL, flags);
        if (!ok) {
            close(s);
            continue;
        }
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        found = static_cast<std::uintptr_t>(s);
#endif
    }
    freeaddrinfo(result);
    if (found == kInvalidSocket) {
        error = fmt::format("can't connect to {}:{}", host, port);
    }
    return found;
}

bool SendAll(std::uintptr_t s, const std::vector<u8>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const int n = send(static_cast<SOCKET>(s), reinterpret_cast<const char*>(bytes.data()) + sent,
                           static_cast<int>(bytes.size() - sent), 0);
#else
        const ssize_t n =
            send(static_cast<int>(s), bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// Reads messages until the connection ends; returns why it ended.
std::string ReadMessages(std::uintptr_t s, const std::function<void(const Packet&)>& handle,
                         const std::atomic<bool>* stop) {
    Framer framer;
    std::vector<u8> buffer(64 * 1024);
    std::vector<Packet> packets;
    for (;;) {
        if (stop) { // wait for data a second at a time, so a dropped client's reader ends
#ifdef _WIN32
            fd_set r;
            FD_ZERO(&r);
            FD_SET(static_cast<SOCKET>(s), &r);
            timeval tv{1, 0};
            const int k = select(0, &r, nullptr, nullptr, &tv);
#else
            pollfd p{static_cast<int>(s), POLLIN, 0};
            const int k = poll(&p, 1, 1000);
#endif
            if (stop->load()) {
                return "dropped by the server";
            }
            if (k == 0 || (k < 0 && LastSocketError() == EINTR)) {
                continue;
            }
            if (k < 0) {
                return fmt::format("connection error {}", LastSocketError());
            }
        }
#ifdef _WIN32
        const int n = recv(static_cast<SOCKET>(s), reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0);
#else
        const ssize_t n = recv(static_cast<int>(s), buffer.data(), buffer.size(), 0);
#endif
        if (n <= 0) {
            return n == 0 ? "the other side closed the connection"
                          : fmt::format("connection error {}", LastSocketError());
        }
        packets.clear();
        if (!framer.Feed(std::span<const u8>(buffer.data(), static_cast<std::size_t>(n)),
                         packets)) {
            ShutdownSocket(s);
            return "bad data: " + framer.error();
        }
        for (const Packet& packet : packets) {
            handle(packet);
        }
    }
}

std::uintptr_t ListenTcp(const std::string& address, u16 port, std::string& error) {
    EnsureSockets();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (address.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        error = fmt::format("not an IPv4 address: {}", address);
        return kInvalidSocket;
    }
#ifdef _WIN32
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = fmt::format("socket error {}", WSAGetLastError());
        return kInvalidSocket;
    }
    // Another program on the port must make this fail, not share it.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, SOMAXCONN) != 0) {
        error = fmt::format("can't listen on port {} (error {}; in use?)", port, WSAGetLastError());
        closesocket(s);
        return kInvalidSocket;
    }
#else
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        error = fmt::format("socket error {}", errno);
        return kInvalidSocket;
    }
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, SOMAXCONN) != 0) {
        error = fmt::format("can't listen on port {} (error {}; in use?)", port, errno);
        close(s);
        return kInvalidSocket;
    }
#endif
    return static_cast<std::uintptr_t>(s);
}

std::uintptr_t AcceptTcp(std::uintptr_t listener, u32& remote_ip) {
    sockaddr_in addr{};
#ifdef _WIN32
    int len = sizeof(addr);
    const SOCKET s = accept(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
    if (s == INVALID_SOCKET) {
        return kInvalidSocket;
    }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               sizeof(nodelay));
    // (a client that stops reading fails the writer's send instead of holding it: Drop then ends the session)
    DWORD send_timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));
#else
    socklen_t len = sizeof(addr);
    const int s = accept(static_cast<int>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
    if (s < 0) {
        return kInvalidSocket;
    }
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    timeval send_timeout{10, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
#endif
    remote_ip = ntohl(addr.sin_addr.s_addr);
    return static_cast<std::uintptr_t>(s);
}

} // namespace Network::RyuLdn
