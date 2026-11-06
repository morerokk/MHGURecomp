// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: MIT

// Behaviour follows Ryujinx's LdnServer (MIT, Copyright (c) Ryujinx Team and Contributors):
// written from the protocol and the server's observable behaviour, not copied.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "protocol.h"
#include "server.h"
#include "socket_util.h"

namespace Network::RyuLdn {

namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 kNetworkBase = 0x0A720000; // 10.114.0.0: the lobbies' virtual network
constexpr u32 kSubnetMask = 0xFFFF0000;
constexpr u32 kBroadcast = kNetworkBase | ~kSubnetMask;
constexpr u32 kAltBroadcast = 0xC0A800FF; // 192.168.0.255, used by some games
constexpr auto kPingAfter = std::chrono::seconds(10);    // idle time before the server pings
constexpr auto kPingTimeout = std::chrono::seconds(10); // no echo by then: disconnect
constexpr auto kGhostLobbyAge = std::chrono::hours(16);  // older lobbies aren't listed
constexpr std::size_t kMaxMacs = 4096;                    // remembered client MACs (HandleInitialize)
constexpr std::size_t kMaxQueuedBytes = 4u << 20;       // a client this far behind is dropped

// Nintendo LDN layouts (= Service::LDN types) used by the server.
namespace Info { // NetworkInfo, 0x480
constexpr std::size_t Lcid = 0x00, SceneId = 0x0A, SessionId = 0x10, Bssid = 0x20,
                      SsidLength = 0x26, SsidName = 0x27, Channel = 0x48, LinkLevel = 0x4A,
                      NetworkType = 0x4B, SecurityMode = 0x60, AcceptPolicy = 0x62,
                      NodeCountMax = 0x66, NodeCount = 0x67, Nodes = 0x68, AdvertSize = 0x26A,
                      Advert = 0x26C, AdvertMax = 0x180;
}
namespace NodeOff { // NodeInfo, 0x40 each
constexpr std::size_t Size = 0x40, Ip = 0x00, Mac = 0x04, Id = 0x0A, Connected = 0x0B,
                      Name = 0x0C, NameSize = 0x21, Lcv = 0x2E;
}
namespace Config { // NetworkConfig, 0x20
constexpr std::size_t Lcid = 0x00, SceneId = 0x0A, Channel = 0x10, NodeCountMax = 0x12,
                      Lcv = 0x14;
}
namespace Filter { // ScanFilter, 0x60
constexpr std::size_t Lcid = 0x00, SceneId = 0x0A, SessionId = 0x10, NetworkType = 0x20,
                      SsidLength = 0x2A, SsidName = 0x2B, Flag = 0x5C;
constexpr u32 FlagLcid = 1 << 0, FlagSessionId = 1 << 1, FlagNetworkType = 1 << 2,
              FlagSsid = 1 << 4, FlagSceneId = 1 << 5;
}

template <typename T, std::size_t N>
T Get(const std::array<u8, N>& blob, std::size_t offset) {
    T value;
    std::memcpy(&value, blob.data() + offset, sizeof(T));
    return value;
}

template <typename T, std::size_t N>
void Put(std::array<u8, N>& blob, std::size_t offset, T value) {
    std::memcpy(blob.data() + offset, &value, sizeof(T));
}

using SessionKey = std::array<u8, 16>;

} // namespace

struct Server::Impl {
    struct Game;

    struct Session {
        u64 serial = 0;
        int logged = 0, logged_bc = 0, logged_uni = 0, logged_rx = 0;
        std::uintptr_t sock = kInvalidSocket;

        // Below: under Impl::lock.
        SessionKey id{};
        std::array<u8, 6> mac{};
        std::string passphrase;
        bool initialized = false;
        u32 ip = 0; // virtual address while in a lobby
        int node_id = -1;
        std::shared_ptr<Game> game;
        Clock::time_point last_message = Clock::now();
        int waiting_ping = -1;
        Clock::time_point ping_sent{};
        u8 next_ping = 0;
        bool finished = false; // reader ended; reaped by the housekeeping thread

        // Outgoing queue, drained by the writer thread (handlers never block on a socket).
        std::mutex out_lock;
        std::condition_variable out_cv;
        std::deque<std::vector<u8>> out;
        std::size_t out_bytes = 0;
        bool closing = false;
        // Closed once, in Reap, after both threads have ended: closed earlier, the number could go to
        // the next client while this session's threads still use it. Windows doesn't wake a blocked
        // recv on shutdown(), so Drop sets `dropped`, which the reader checks every second.
        std::atomic<bool> sock_closed{false};
        std::atomic<bool> dropped{false};
        u32 remote_ip = 0; // the client's real address (host order)

        std::thread reader;
        std::thread writer;
    };

    struct Game {
        SessionKey id{};
        NetworkInfoBlob info{};
        Session* owner = nullptr;
        std::string passphrase;
        std::string version;
        std::vector<Session*> players;
        Clock::time_point created = Clock::now();
        bool closed = false;
        // Virtual DHCP: 10.114.0.1 upward, reserved addresses from a private lobby's list.
        u32 next_ip = kNetworkBase + 1;
        std::set<u32> taken;
        std::map<std::array<u8, 6>, u32> reserved;
    };

    mutable std::mutex lock;
    std::list<std::unique_ptr<Session>> sessions;
    std::map<SessionKey, std::shared_ptr<Game>> games;
    std::map<SessionKey, std::array<u8, 6>> macs; // session id -> MAC, kept across reconnects
    std::set<std::array<u8, 6>> used_macs;
    std::mt19937_64 rng{std::random_device{}()};
    u64 next_serial = 1;

    std::uintptr_t listener = kInvalidSocket;
    std::atomic<bool> running{false};
    u16 port = 0;
    std::thread acceptor;
    std::thread housekeeping;
    std::mutex stop_lock;
    std::condition_variable stop_cv;

    // ── sending ──

    /// Ends a connection now: the writer stops, the reader's recv fails, cleanup follows.
    static void Drop(Session& s) {
        {
            std::scoped_lock lk{s.out_lock};
            s.closing = true;
            s.out.clear();
            s.out_bytes = 0;
            s.out_cv.notify_all();
        }
        s.dropped = true;
        ShutdownSocket(s.sock);
    }

    void Send(Session& s, std::vector<u8> bytes) {
        {
            std::scoped_lock lk{s.out_lock};
            if (s.closing) {
                return;
            }
            if (s.out_bytes + bytes.size() <= kMaxQueuedBytes) {
                s.out_bytes += bytes.size();
                s.out.push_back(std::move(bytes));
                s.out_cv.notify_all();
                return;
            }
        }
        LOG_WARNING(Network, "ryuldn server: dropping a client that stopped reading");
        Drop(s);
    }

    void SendError(Session& s, NetworkError error) {
        LOG_INFO(Network, "ryuldn server: client #{} gets error {}", s.serial, static_cast<int>(error));
        Send(s, Encode(PacketId::NetworkError, NetworkErrorMessage{error}));
    }

    void WriterLoop(Session* s) {
        for (;;) {
            std::vector<u8> bytes;
            {
                std::unique_lock lk{s->out_lock};
                s->out_cv.wait(lk, [&] { return s->closing || !s->out.empty(); });
                if (s->out.empty()) {
                    return; // closing and drained
                }
                bytes = std::move(s->out.front());
                s->out.pop_front();
                s->out_bytes -= bytes.size();
            }
            if (!SendAll(s->sock, bytes)) {
                Drop(*s); // the reader notices and cleans up
                return;
            }
        }
    }

    // ── helpers (lock held) ──

    std::array<u8, 6> NewMac() {
        std::array<u8, 6> mac;
        do {
            const u64 r = rng();
            std::memcpy(mac.data(), &r, mac.size());
        } while (used_macs.contains(mac));
        used_macs.insert(mac);
        return mac;
    }

    void BroadcastInfo(Game& game) {
        const auto bytes = Encode(PacketId::SyncNetwork, game.info);
        for (Session* p : game.players) {
            Send(*p, bytes);
        }
    }

    u32 RequestIp(Game& game, const std::array<u8, 6>& mac) {
        if (const auto it = game.reserved.find(mac); it != game.reserved.end()) {
            return it->second;
        }
        while (game.taken.contains(game.next_ip)) {
            CycleIp(game);
        }
        const u32 ip = game.next_ip;
        game.taken.insert(ip);
        CycleIp(game);
        return ip;
    }

    static void CycleIp(Game& game) {
        do {
            game.next_ip = kNetworkBase | ((game.next_ip + 1) & ~kSubnetMask);
        } while (game.next_ip == kNetworkBase || game.next_ip == kBroadcast);
    }

    static void ReturnIp(Game& game, u32 ip) {
        bool reserved = false;
        for (const auto& [mac, rip] : game.reserved) {
            reserved |= rip == ip;
        }
        if (!reserved) {
            game.taken.erase(ip);
        }
    }

    /// Adds s to the lobby with its node info; false when it's full or closed.
    bool JoinGame(const std::shared_ptr<Game>& game, Session& s, std::array<u8, NodeOff::Size> node) {
        auto& info = game->info;
        const u8 count = info[Info::NodeCount];
        if (game->closed || count >= info[Info::NodeCountMax]) {
            return false;
        }
        int slot = -1;
        for (int i = 0; i < 8 && slot < 0; i++) {
            if (info[Info::Nodes + i * NodeOff::Size + NodeOff::Connected] == 0) {
                slot = i;
            }
        }
        if (slot < 0) {
            return false;
        }
        s.ip = RequestIp(*game, s.mac);
        Send(s, Encode(PacketId::ProxyConfig, ProxyConfig{s.ip, kSubnetMask}));

        Put(node, NodeOff::Ip, s.ip);
        std::memcpy(node.data() + NodeOff::Mac, s.mac.data(), 6);
        node[NodeOff::Id] = static_cast<u8>(slot);
        node[NodeOff::Connected] = 1;
        std::memcpy(info.data() + Info::Nodes + slot * NodeOff::Size, node.data(), node.size());
        info[Info::NodeCount] = static_cast<u8>(count + 1);
        s.node_id = slot;

        BroadcastInfo(*game); // the others learn about the new node
        s.game = game;
        game->players.push_back(&s);
        LOG_INFO(Network, "ryuldn server: client #{} joined as node {} ({})", s.serial, slot, s.ip);
        Send(s, Encode(PacketId::Connected, game->info));
        return true;
    }

    /// Takes s out of its lobby (closing the lobby when s hosted it). tell_self: s gets a
    /// Disconnect message too (rejected).
    void LeaveGame(Session& s, bool tell_self) {
        const std::shared_ptr<Game> game = std::move(s.game);
        s.game.reset();
        if (!game) {
            return;
        }
        std::erase(game->players, &s);
        ReturnIp(*game, s.ip);
        for (int i = 0; i < 8; i++) {
            u8* node = game->info.data() + Info::Nodes + i * NodeOff::Size;
            u32 ip;
            std::memcpy(&ip, node + NodeOff::Ip, 4);
            if (node[NodeOff::Connected] && ip == s.ip) {
                node[NodeOff::Connected] = 0;
                game->info[Info::NodeCount]--;
            }
        }
        s.ip = 0;
        s.node_id = -1;
        if (tell_self) {
            Send(s, Encode(PacketId::Disconnect, DisconnectMessage{}));
        }
        BroadcastInfo(*game);
        if (game->owner == &s) {
            CloseGame(game);
        }
    }

    void CloseGame(const std::shared_ptr<Game>& game) {
        if (game->closed) {
            return;
        }
        game->closed = true;
        games.erase(game->id);
        const auto bye = Encode(PacketId::Disconnect, DisconnectMessage{});
        for (Session* p : game->players) {
            Send(*p, bye);
            ReturnIp(*game, p->ip);
            p->ip = 0;
            p->node_id = -1;
            p->game.reset();
        }
        game->players.clear();
        LOG_INFO(Network, "ryuldn server: lobby closed ({} open)", games.size());
    }

    // ── handlers (lock held) ──

    void HandleInitialize(Session& s, std::span<const u8> data) {
        LOG_INFO(Network, "ryuldn server: client #{} initializes", s.serial);
        const auto msg = Read<InitializeMessage>(data);
        if (!msg || s.initialized) {
            return;
        }
        SessionKey old_id;
        std::memcpy(old_id.data(), msg->id.data(), old_id.size());
        std::array<u8, 6> old_mac;
        std::memcpy(old_mac.data(), msg->mac.data(), old_mac.size());
        // Same MAC as before for a client that comes back with its id and MAC. (The tables only grow,
        // one entry per Initialize: past kMaxMacs they start again from the clients still here.)
        if (macs.size() >= kMaxMacs) {
            macs.clear();
            used_macs.clear();
            for (const auto& o : sessions) {
                if (o->initialized && !o->finished) {
                    macs[o->id] = o->mac;
                    used_macs.insert(o->mac);
                }
            }
        }
        if (const auto it = macs.find(old_id); it != macs.end() && it->second == old_mac) {
            s.mac = it->second;
        } else {
            s.mac = NewMac();
        }
        macs[s.id] = s.mac;
        InitializeMessage reply{};
        std::memcpy(reply.id.data(), s.id.data(), reply.id.size());
        std::memcpy(reply.mac.data(), s.mac.data(), reply.mac.size());
        Send(s, Encode(PacketId::Initialize, reply));
        s.initialized = true;
    }

    void HandlePassphrase(Session& s, std::span<const u8> data) {
        const auto msg = Read<PassphraseMessage>(data);
        if (!msg) {
            return;
        }
        std::string text(reinterpret_cast<const char*>(msg->passphrase.data()),
                         msg->passphrase.size());
        text.resize(std::strlen(text.c_str()));
        bool valid = text.empty();
        if (text.size() == 16 && text.rfind("Ryujinx-", 0) == 0) {
            valid = std::all_of(text.begin() + 8, text.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }
        s.passphrase = valid ? text : std::string{};
    }

    void CreateGame(Session& s, const SessionKey& id, const NetworkConfigBlob& config,
                    const UserConfigBlob& user, const RyuNetworkConfig& ryu,
                    const SecurityConfigBlob& security, const AddressListBlob* addresses,
                    std::span<const u8> advert) {
        if (s.game || !s.initialized) {
            SendError(s, NetworkError::Unknown);
            return;
        }
        if (ryu.external_proxy_port != 0) {
            // The host offers a P2P proxy; this server relays everything instead (as LdnServer
            // does when it can't reach that port).
            SendError(s, NetworkError::PortUnreachable);
        }
        if (const auto it = games.find(id); it != games.end()) {
            if (it->second->owner == &s) {
                CloseGame(it->second);
            } else {
                SendError(s, NetworkError::Unknown); // id taken
                return;
            }
        }

        auto game = std::make_shared<Game>();
        game->id = id;
        game->owner = &s;
        game->passphrase = s.passphrase;
        game->version.assign(reinterpret_cast<const char*>(ryu.game_version.data()),
                             ryu.game_version.size());
        game->version.resize(std::strlen(game->version.c_str()));

        auto& info = game->info;
        std::memcpy(info.data() + Info::Lcid, config.data() + Config::Lcid, 8);
        std::memcpy(info.data() + Info::SceneId, config.data() + Config::SceneId, 2);
        std::memcpy(info.data() + Info::SessionId, id.data(), id.size());
        std::memcpy(info.data() + Info::Bssid, s.mac.data(), 6);
        info[Info::SsidLength] = 32;
        std::memcpy(info.data() + Info::SsidName, "12345678123456781234567812345678", 32);
        std::memcpy(info.data() + Info::Channel, config.data() + Config::Channel, 2);
        info[Info::LinkLevel] = 3;
        info[Info::NetworkType] = 2;
        std::memcpy(info.data() + Info::SecurityMode, security.data(), 2);
        info[Info::NodeCountMax] = std::min<u8>(config[Config::NodeCountMax], 8);
        const std::size_t advert_size = std::min(advert.size(), Info::AdvertMax);
        Put(info, Info::AdvertSize, static_cast<u16>(advert_size));
        std::memcpy(info.data() + Info::Advert, advert.data(), advert_size);

        if (addresses) {
            for (int i = 0; i < 8; i++) {
                u32 ip;
                std::memcpy(&ip, addresses->data() + i * 12, 4);
                if (ip == 0) {
                    break;
                }
                std::array<u8, 6> mac;
                std::memcpy(mac.data(), addresses->data() + i * 12 + 4, 6);
                game->reserved[mac] = ip;
                game->taken.insert(ip);
            }
        }

        std::array<u8, NodeOff::Size> node{};
        std::memcpy(node.data() + NodeOff::Name, user.data(), NodeOff::NameSize);
        std::memcpy(node.data() + NodeOff::Lcv, config.data() + Config::Lcv, 2);

        games[id] = game;
        if (!JoinGame(game, s, node)) {
            games.erase(id);
            SendError(s, NetworkError::Unknown);
            return;
        }
        LOG_INFO(Network, "ryuldn server: lobby opened ({} open, game version '{}')", games.size(),
                 game->version);
    }

    void HandleCreate(Session& s, std::span<const u8> data) {
        const auto req = Read<CreateAccessPointRequest>(data);
        if (!req) {
            return;
        }
        SessionKey id;
        for (std::size_t i = 0; i < id.size(); i += 8) {
            const u64 r = rng();
            std::memcpy(id.data() + i, &r, 8);
        }
        CreateGame(s, id, req->network_config, req->user_config, req->ryu_network_config,
                   req->security_config, nullptr, Trailing<CreateAccessPointRequest>(data));
    }

    void HandleCreatePrivate(Session& s, std::span<const u8> data) {
        const auto req = Read<CreateAccessPointPrivateRequest>(data);
        if (!req) {
            return;
        }
        SessionKey id;
        std::memcpy(id.data(), req->security_parameter.data() + 0x10, id.size());
        CreateGame(s, id, req->network_config, req->user_config, req->ryu_network_config,
                   req->security_config, &req->address_list,
                   Trailing<CreateAccessPointPrivateRequest>(data));
    }

    void ConnectTo(Session& s, const SessionKey& id, const UserConfigBlob& user, u32 lcv) {
        if (!s.initialized) {
            SendError(s, NetworkError::ConnectFailure);
            return;
        }
        const auto it = games.find(id);
        if (it == games.end()) {
            SendError(s, NetworkError::ConnectNotFound);
            return;
        }
        const auto game = it->second;
        const u16 host_lcv = Get<u16>(game->info, Info::Nodes + NodeOff::Lcv);
        LOG_INFO(Network, "ryuldn server: client #{} wants to join: its version {}, the host's {}", s.serial, lcv, host_lcv);
        if (lcv > host_lcv) {
            SendError(s, NetworkError::VersionTooHigh);
            return;
        }
        if (lcv < host_lcv) {
            SendError(s, NetworkError::VersionTooLow);
            return;
        }
        if (s.game) {
            LeaveGame(s, false);
        }
        std::array<u8, NodeOff::Size> node{};
        std::memcpy(node.data() + NodeOff::Name, user.data(), NodeOff::NameSize);
        const u16 node_lcv = static_cast<u16>(lcv);
        std::memcpy(node.data() + NodeOff::Lcv, &node_lcv, 2);
        if (!JoinGame(game, s, node)) {
            SendError(s, NetworkError::TooManyPlayers);
        }
    }

    void HandleConnect(Session& s, std::span<const u8> data) {
        if (const auto req = Read<ConnectRequest>(data)) {
            SessionKey id;
            std::memcpy(id.data(), req->network_info.data() + Info::SessionId, id.size());
            ConnectTo(s, id, req->user_config, req->local_communication_version);
        } else {
            LOG_INFO(Network, "ryuldn server: client #{} sent a Connect that is too short ({} bytes)", s.serial, data.size());
        }
    }

    void HandleConnectPrivate(Session& s, std::span<const u8> data) {
        if (const auto req = Read<ConnectPrivateRequest>(data)) {
            SessionKey id;
            std::memcpy(id.data(), req->security_parameter.data() + 0x10, id.size());
            ConnectTo(s, id, req->user_config, req->local_communication_version);
        }
    }

    void HandleScan(Session& s, std::span<const u8> data) {
        const auto filter = Read<ScanFilterBlob>(data);
        if (!filter) {
            return;
        }
        const u32 flag = Get<u32>(*filter, Filter::Flag);
        LOG_INFO(Network, "ryuldn server: client #{} scans ({} lobbies, filter flags {})", s.serial, games.size(), flag);
        const auto now = Clock::now();
        for (const auto& [id, game] : games) {
            const auto& info = game->info;
            if (game->passphrase != s.passphrase || game == s.game || game->players.empty() ||
                now - game->created > kGhostLobbyAge || info[Info::AcceptPolicy] == 1) {
                continue;
            }
            if ((flag & Filter::FlagLcid) &&
                Get<u64>(info, Info::Lcid) != Get<u64>(*filter, Filter::Lcid)) {
                continue;
            }
            if ((flag & Filter::FlagSceneId) &&
                Get<u16>(info, Info::SceneId) != Get<u16>(*filter, Filter::SceneId)) {
                continue;
            }
            if ((flag & Filter::FlagSessionId) &&
                std::memcmp(info.data() + Info::SessionId, filter->data() + Filter::SessionId,
                            16) != 0) {
                continue;
            }
            if (flag & Filter::FlagSsid) {
                const u8 len = std::min<u8>((*filter)[Filter::SsidLength], 32);
                if (info[Info::SsidLength] != len ||
                    std::memcmp(info.data() + Info::SsidName, filter->data() + Filter::SsidName,
                                len) != 0) {
                    continue;
                }
            }
            if ((flag & Filter::FlagNetworkType) &&
                info[Info::NetworkType] != Get<u32>(*filter, Filter::NetworkType)) {
                continue;
            }
            Send(s, Encode(PacketId::ScanReply, info));
        }
        Send(s, Encode(PacketId::ScanReplyEnd));
    }

    void HandleReject(Session& s, std::span<const u8> data) {
        const auto req = Read<RejectRequest>(data);
        const auto game = s.game;
        bool done = false;
        if (req && game && game->owner == &s) {
            for (Session* p : game->players) {
                if (p != &s && p->node_id == static_cast<int>(req->node_id)) {
                    LeaveGame(*p, true);
                    done = true;
                    break;
                }
            }
        }
        if (!done) {
            SendError(s, NetworkError::RejectFailed);
        }
        Send(s, Encode(PacketId::RejectReply));
    }

    void HandleSetAcceptPolicy(Session& s, std::span<const u8> data) {
        const auto req = Read<SetAcceptPolicyRequest>(data);
        if (req && s.game && s.game->owner == &s) {
            s.game->info[Info::AcceptPolicy] = req->accept_policy;
            BroadcastInfo(*s.game);
        }
    }

    void HandleSetAdvertiseData(Session& s, std::span<const u8> data) {
        if (!s.game || s.game->owner != &s) {
            return;
        }
        auto& info = s.game->info;
        const std::size_t size = std::min(data.size(), Info::AdvertMax);
        Put(info, Info::AdvertSize, static_cast<u16>(size));
        std::memset(info.data() + Info::Advert, 0, Info::AdvertMax);
        std::memcpy(info.data() + Info::Advert, data.data(), size);
        BroadcastInfo(*s.game);
    }

    /// Relays a proxy message (its leading ProxyInfo) to the addressed node(s) of the lobby.
    template <typename T>
    void Route(Session& s, PacketId type, std::span<const u8> data) {
        auto msg = Read<T>(data);
        if (!msg || !s.game) {
            return;
        }
        ProxyInfo& info = msg->info;
        if (info.source_ip == 0) {
            info.source_ip = s.ip; // sent from a socket bound to 0.0.0.0
        } else if (info.source_ip != s.ip) {
            LOG_INFO(Network, "ryuldn server: client #{} (ip {}) sent a game packet as {}: dropped", s.serial, s.ip, info.source_ip);
            return; // can't pretend to be someone else
        }
        u32 dest = info.dest_ip;
        if (dest == kAltBroadcast || dest == 0xFFFFFFFFu) {
            dest = kBroadcast;
        }
        int& seen = dest == kBroadcast ? s.logged_bc : s.logged_uni;
        if (std::getenv("MHGU_LOG_NET") && seen < (dest == kBroadcast ? 10 : 100)) {
            seen++;
            LOG_INFO(Network, "ryuldn server: game packet from {} to {} ({} -> {}, {} bytes)", info.source_ip, info.dest_ip, info.source_port,
                     info.dest_port, data.size());
        }
        const auto bytes = Encode(type, *msg, Trailing<T>(data));
        for (Session* p : s.game->players) {
            if (dest == kBroadcast || p->ip == dest) {
                if (std::getenv("MHGU_LOG_NET") && p != &s && p->logged_rx < 10) {
                    p->logged_rx++;
                    LOG_INFO(Network, "ryuldn server: relayed {} bytes (to {}:{}) to client #{} ({})", bytes.size(), info.dest_ip, info.dest_port,
                             p->serial, p->ip);
                }
                Send(*p, bytes);
            }
        }
    }

    void HandlePing(Session& s, std::span<const u8> data) {
        const auto ping = Read<PingMessage>(data);
        if (ping && ping->requester == 0 && ping->id == s.waiting_ping) {
            s.waiting_ping = -1;
        }
    }

    void Handle(Session& s, const Packet& packet) {
        std::scoped_lock lk{lock};
        if (packet.type != PacketId::ProxyData) {
            LOG_INFO(Network, "ryuldn server: client #{} sent packet {} ({} bytes)", s.serial, static_cast<int>(packet.type), packet.data.size());
        }
        s.last_message = Clock::now();
        const std::span<const u8> data{packet.data};
        switch (packet.type) {
        case PacketId::Initialize:
            return HandleInitialize(s, data);
        case PacketId::Passphrase:
            return HandlePassphrase(s, data);
        case PacketId::CreateAccessPoint:
            return HandleCreate(s, data);
        case PacketId::CreateAccessPointPrivate:
            return HandleCreatePrivate(s, data);
        case PacketId::Connect:
            return HandleConnect(s, data);
        case PacketId::ConnectPrivate:
            return HandleConnectPrivate(s, data);
        case PacketId::Scan:
            return HandleScan(s, data);
        case PacketId::Disconnect:
            return LeaveGame(s, false);
        case PacketId::Reject:
            return HandleReject(s, data);
        case PacketId::SetAcceptPolicy:
            return HandleSetAcceptPolicy(s, data);
        case PacketId::SetAdvertiseData:
            return HandleSetAdvertiseData(s, data);
        case PacketId::ProxyData:
            return Route<ProxyDataHeader>(s, PacketId::ProxyData, data);
        case PacketId::ProxyConnect:
            return Route<ProxyConnectRequest>(s, PacketId::ProxyConnect, data);
        case PacketId::ProxyConnectReply:
            return Route<ProxyConnectResponse>(s, PacketId::ProxyConnectReply, data);
        case PacketId::ProxyDisconnect:
            return Route<ProxyDisconnectMessage>(s, PacketId::ProxyDisconnect, data);
        case PacketId::Ping:
            return HandlePing(s, data);
        default:
            return; // ExternalProxyState etc.: no P2P here
        }
    }

    // ── connections ──

    void AcceptLoop() {
        while (running.load()) {
            u32 remote_ip = 0;
            const std::uintptr_t sock = AcceptTcp(listener, remote_ip);
            if (sock == kInvalidSocket) {
                if (!running.load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::scoped_lock lk{lock};
            std::size_t live = 0, same_ip = 0;
            for (const auto& s : sessions) {
                live += s->finished ? 0 : 1;
                same_ip += !s->finished && s->remote_ip == remote_ip;
            }
            // (one address can't take every place; this computer's own game opens one per room
            // player it brings into a hub, xbridge.c, so it isn't counted)
            const bool loopback = (remote_ip >> 24) == 127;
            if (live >= MaxConnections || (!loopback && same_ip >= MaxConnectionsPerIp)) {
                CloseSocket(sock);
                continue;
            }
            auto session = std::make_unique<Session>();
            Session* s = session.get();
            s->serial = next_serial++;
            s->sock = sock;
            s->remote_ip = remote_ip;
            for (std::size_t i = 0; i < s->id.size(); i += 8) {
                const u64 r = rng();
                std::memcpy(s->id.data() + i, &r, 8);
            }
            s->writer = std::thread([this, s] { WriterLoop(s); });
            s->reader = std::thread([this, s] {
                ReadMessages(s->sock, [this, s](const Packet& p) { Handle(*s, p); }, &s->dropped);
                EndSession(*s);
            });
            sessions.push_back(std::move(session));
        }
    }

    void EndSession(Session& s) {
        {
            std::scoped_lock lk{lock};
            LeaveGame(s, false);
            s.finished = true;
        }
        {
            std::scoped_lock lk{s.out_lock};
            s.closing = true;
            s.out_cv.notify_all();
        }
        ShutdownSocket(s.sock);
    }

    /// Pings idle clients, drops silent ones, and frees finished sessions.
    void HousekeepingLoop() {
        std::unique_lock stop{stop_lock};
        while (running.load()) {
            stop_cv.wait_for(stop, std::chrono::seconds(1));
            if (!running.load()) {
                return;
            }
            std::vector<std::unique_ptr<Session>> done;
            {
                std::scoped_lock lk{lock};
                const auto now = Clock::now();
                for (auto it = sessions.begin(); it != sessions.end();) {
                    Session& s = **it;
                    if (s.finished) {
                        done.push_back(std::move(*it));
                        it = sessions.erase(it);
                        continue;
                    }
                    if (s.waiting_ping != -1 && now - s.ping_sent > kPingTimeout) {
                        Drop(s); // no echo: gone
                    } else if (s.waiting_ping == -1 && now - s.last_message > kPingAfter) {
                        s.waiting_ping = s.next_ping++;
                        s.ping_sent = now;
                        Send(s, Encode(PacketId::Ping,
                                       PingMessage{0, static_cast<u8>(s.waiting_ping)}));
                    }
                    ++it;
                }
            }
            for (auto& s : done) {
                Reap(*s);
            }
        }
    }

    static void Reap(Session& s) {
        if (s.reader.joinable()) {
            s.reader.join();
        }
        if (s.writer.joinable()) {
            s.writer.join();
        }
        if (!s.sock_closed.exchange(true)) {
            CloseSocket(s.sock);
        }
    }
};

Server::Server() : impl{std::make_unique<Impl>()} {}

Server::~Server() {
    Stop();
}

bool Server::Start(const std::string& address, u16 port, std::string& error) {
    if (impl->running.load()) {
        return true;
    }
    const std::uintptr_t listener = ListenTcp(address, port, error);
    if (listener == kInvalidSocket) {
        LOG_ERROR(Network, "ryuldn server: {}", error);
        return false;
    }
    impl->listener = listener;
    impl->port = port;
    impl->running = true;
    impl->acceptor = std::thread([this] { impl->AcceptLoop(); });
    impl->housekeeping = std::thread([this] { impl->HousekeepingLoop(); });
    LOG_INFO(Network, "ryuldn server: listening on port {}", port);
    return true;
}

void Server::Stop() {
    if (!impl->running.exchange(false)) {
        return;
    }
    ShutdownSocket(impl->listener);
    CloseSocket(impl->listener); // wakes accept()
    {
        std::scoped_lock lk{impl->stop_lock};
        impl->stop_cv.notify_all();
    }
    if (impl->acceptor.joinable()) {
        impl->acceptor.join();
    }
    if (impl->housekeeping.joinable()) {
        impl->housekeeping.join();
    }
    std::list<std::unique_ptr<Impl::Session>> sessions;
    {
        std::scoped_lock lk{impl->lock};
        for (auto& s : impl->sessions) {
            Impl::Drop(*s);
        }
        sessions = std::move(impl->sessions);
    }
    for (auto& s : sessions) {
        Impl::Reap(*s); // readers end (EndSession) once their socket is shut down
    }
    std::scoped_lock lk{impl->lock};
    impl->games.clear();
    impl->listener = kInvalidSocket;
    LOG_INFO(Network, "ryuldn server: stopped");
}

Server::Status Server::GetStatus() const {
    std::scoped_lock lk{impl->lock};
    Status status;
    status.running = impl->running.load();
    status.port = impl->port;
    for (const auto& s : impl->sessions) {
        status.connections += s->finished ? 0 : 1;
    }
    status.lobbies = impl->games.size();
    for (const auto& [id, game] : impl->games) {
        status.players += game->players.size();
    }
    return status;
}

} // namespace Network::RyuLdn
