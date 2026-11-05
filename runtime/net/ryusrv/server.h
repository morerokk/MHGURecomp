// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "common/common_types.h"

namespace Network::RyuLdn {

/**
 * A small RyuLdn master server inside the game: hosts lobbies for friends over a VPN such as
 * Radmin (or a LAN), so nobody needs the public server or a separate LdnServer. Exported-game
 * players and Ryujinx players (mode RyuLdn, server = this PC's address) both connect to it.
 *
 * Behaves like Ryujinx's LdnServer for everything a lobby needs (virtual 10.114.0.0/16 network,
 * scan filters, version checks, relay of the game traffic, idle pings). Left out: P2P proxy
 * hosting (a host asking for it is told PortUnreachable and relayed instead, as LdnServer does
 * when the port can't be reached), bans and statistics.
 */
class Server {
public:
    struct Status {
        bool running = false;
        u16 port = 0;
        std::size_t connections = 0; // clients connected to the server
        std::size_t lobbies = 0;
        std::size_t players = 0; // in lobbies
    };

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Listens on address:port (address "" = every network adapter). False + error when the
    /// port can't be opened (another server on it?).
    bool Start(const std::string& address, u16 port, std::string& error);
    /// Closes every connection and lobby.
    void Stop();
    Status GetStatus() const;

    static constexpr std::size_t MaxConnections = 64;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Network::RyuLdn
