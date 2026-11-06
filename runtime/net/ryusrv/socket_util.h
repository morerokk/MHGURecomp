// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "protocol.h"

/// Plain blocking TCP for the RyuLdn client and server (sockets as std::uintptr_t).
namespace Network::RyuLdn {

#ifdef _WIN32
constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0}; // INVALID_SOCKET
#else
constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(-1);
#endif

/// Winsock start-up (Windows); call before the first socket.
void EnsureSockets();

void CloseSocket(std::uintptr_t s);
void ShutdownSocket(std::uintptr_t s);
int LastSocketError();

/// TCP connect with a timeout; kInvalidSocket + message on failure.
std::uintptr_t ConnectTcp(const std::string& host, u16 port, int timeout_ms, std::string& error);

bool SendAll(std::uintptr_t s, const std::vector<u8>& bytes);

/// Reads messages until the connection ends; returns why it ended.
std::string ReadMessages(std::uintptr_t s, const std::function<void(const Packet&)>& handle);

/// IPv4 TCP listener on address:port (address "" = all); kInvalidSocket + message on failure.
std::uintptr_t ListenTcp(const std::string& address, u16 port, std::string& error);

/// Next connection (blocking); kInvalidSocket once the listener is closed. remote_ip is the
/// peer's IPv4 address, host order.
std::uintptr_t AcceptTcp(std::uintptr_t listener, u32& remote_ip);

} // namespace Network::RyuLdn
