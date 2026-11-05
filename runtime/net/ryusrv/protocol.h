// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

/**
 * RyuLdn: the protocol of Ryujinx's LDN master server (LdnServer). Clients keep one TCP
 * connection to the server, which owns the lobbies and relays the games' traffic.
 *
 * Every message: a 12-byte header {u32 magic 'RLDN', u8 type, u8 version (1), 2 bytes padding,
 * s32 payload size} and the payload, little-endian, in the C# servers' sequential struct layout.
 * Sizes and offsets below were measured from the server's own types (the C# StructLayout Size
 * attributes are minimums: the header and the two connect requests are larger than they say).
 * The Nintendo LDN structs inside (NetworkInfo, NetworkConfig, ...) are carried as opaque blocks
 * here; they have the same layout as Service::LDN's types.
 *
 * Protocol and behaviour: Ryujinx's LdnServer and LDN client, MIT licensed, Copyright (c)
 * Ryujinx Team and Contributors (LICENSES/MIT.txt). Written from the protocol, not copied.
 */
namespace Network::RyuLdn {

static_assert(std::endian::native == std::endian::little, "RyuLdn structs are little-endian");

constexpr u32 Magic = 'R' | ('L' << 8) | ('D' << 16) | ('N' << 24);
constexpr u8 ProtocolVersion = 1;
constexpr std::size_t HeaderSize = 12;
/// The server drops the connection when header + payload reaches this.
constexpr std::size_t MaxPacketSize = 131072;
constexpr std::size_t MaxPayloadSize = MaxPacketSize - HeaderSize - 1;
constexpr u16 DefaultPort = 30456;

enum class PacketId : u8 {
    Initialize = 0,
    Passphrase = 1,
    CreateAccessPoint = 2,
    CreateAccessPointPrivate = 3,
    ExternalProxy = 4,
    ExternalProxyToken = 5,
    ExternalProxyState = 6,
    SyncNetwork = 7,
    Reject = 8,
    RejectReply = 9,
    Scan = 10,
    ScanReply = 11,
    ScanReplyEnd = 12,
    Connect = 13,
    ConnectPrivate = 14,
    Connected = 15,
    Disconnect = 16,
    ProxyConfig = 17,
    ProxyConnect = 18,
    ProxyConnectReply = 19,
    ProxyData = 20,
    ProxyDisconnect = 21,
    SetAcceptPolicy = 22,
    SetAdvertiseData = 23,
    Ping = 254,
    NetworkError = 255,
};

const char* PacketName(PacketId id);

enum class NetworkError : s32 {
    None = 0,
    PortUnreachable = 1,
    TooManyPlayers = 2,
    VersionTooLow = 3,
    VersionTooHigh = 4,
    ConnectFailure = 5,
    ConnectNotFound = 6,
    ConnectTimeout = 7,
    ConnectRejected = 8,
    RejectFailed = 9,
    BannedByServer = 127,
    Unknown = -1,
};

const char* ErrorName(NetworkError error);

/// IPv4 addresses on the wire are host-order u32 (10.114.0.1 = 0x0A720001).
constexpr u32 ProtocolUdp = 17;
constexpr u32 ProtocolTcp = 6;

// Opaque Nintendo LDN structs (layout = Service::LDN::* of the same name).
using SecurityConfigBlob = std::array<u8, 0x44>;
using UserConfigBlob = std::array<u8, 0x30>;
using NetworkConfigBlob = std::array<u8, 0x20>;
using SecurityParameterBlob = std::array<u8, 0x20>;
using AddressListBlob = std::array<u8, 0x60>;
using ScanFilterBlob = std::array<u8, 0x60>;
using NetworkInfoBlob = std::array<u8, 0x480>;

#pragma pack(push, 1)

struct Header {
    u32 magic;
    u8 type;
    u8 version;
    u8 padding[2];
    s32 data_size;
};

struct InitializeMessage {
    std::array<u8, 16> id;  // the server's session id for this client (zeros on first contact)
    std::array<u8, 6> mac;  // the server-assigned MAC (zeros on first contact)
};

struct PassphraseMessage {
    std::array<u8, 0x80> passphrase; // UTF-8, zero padded; "" or "Ryujinx-" + 8 hex digits
};

struct PingMessage {
    u8 requester; // 0 = the server asks (echo it back), 1 = a client asked
    u8 id;
};

struct DisconnectMessage {
    u32 disconnect_ip;
};

struct NetworkErrorMessage {
    NetworkError error;
};

struct RejectRequest {
    u32 node_id;
    u32 disconnect_reason;
};

struct SetAcceptPolicyRequest {
    u8 accept_policy;
};

struct ProxyInfo {
    u32 source_ip;
    u16 source_port;
    u32 dest_ip;
    u16 dest_port;
    u32 protocol; // 6 TCP, 17 UDP
};

struct ProxyDataHeader {
    ProxyInfo info;
    u32 data_length; // followed by that many bytes
};

struct ProxyConfig {
    u32 proxy_ip;
    u32 proxy_subnet_mask;
};

struct ProxyConnectRequest {
    ProxyInfo info;
};

struct ProxyConnectResponse {
    ProxyInfo info;
};

struct ProxyDisconnectMessage {
    ProxyInfo info;
    s32 disconnect_reason;
};

struct RyuNetworkConfig {
    std::array<u8, 16> game_version; // the game's display version, e.g. "1.4.0"
    std::array<u8, 16> private_ip;   // below: only for a P2P proxy host (zeros otherwise)
    u32 address_family;
    u16 external_proxy_port;
    u16 internal_proxy_port;
};

struct ExternalProxyConfig {
    std::array<u8, 16> proxy_ip;
    u32 address_family;
    u16 proxy_port;
    std::array<u8, 16> token;
};

struct ExternalProxyToken {
    u32 virtual_ip;
    std::array<u8, 16> token;
    std::array<u8, 16> physical_ip;
    u32 address_family;
};

struct ExternalProxyConnectionState {
    u32 ip_address;
    u8 connected;
    u8 padding[3];
};

struct CreateAccessPointRequest {
    SecurityConfigBlob security_config;
    UserConfigBlob user_config;
    NetworkConfigBlob network_config;
    RyuNetworkConfig ryu_network_config;
    // followed by the advertise data (up to 0x180 bytes)
};

struct CreateAccessPointPrivateRequest {
    SecurityConfigBlob security_config;
    SecurityParameterBlob security_parameter;
    UserConfigBlob user_config;
    NetworkConfigBlob network_config;
    AddressListBlob address_list;
    RyuNetworkConfig ryu_network_config;
    // followed by the advertise data
};

struct ConnectRequest {
    SecurityConfigBlob security_config;
    UserConfigBlob user_config;
    u32 local_communication_version;
    u32 option_unknown;
    u8 padding[4]; // NetworkInfo is 8-aligned in the C# layout
    NetworkInfoBlob network_info;
};

struct ConnectPrivateRequest {
    SecurityConfigBlob security_config;
    SecurityParameterBlob security_parameter;
    UserConfigBlob user_config;
    u32 local_communication_version;
    u32 option_unknown;
    u8 padding[4];
    NetworkConfigBlob network_config;
};

#pragma pack(pop)

static_assert(sizeof(Header) == HeaderSize && offsetof(Header, data_size) == 8);
static_assert(sizeof(InitializeMessage) == 0x16);
static_assert(sizeof(PassphraseMessage) == 0x80);
static_assert(sizeof(PingMessage) == 0x2);
static_assert(sizeof(DisconnectMessage) == 0x4);
static_assert(sizeof(NetworkErrorMessage) == 0x4);
static_assert(sizeof(RejectRequest) == 0x8);
static_assert(sizeof(SetAcceptPolicyRequest) == 0x1);
static_assert(sizeof(ProxyInfo) == 0x10 && offsetof(ProxyInfo, dest_ip) == 0x6 &&
              offsetof(ProxyInfo, dest_port) == 0xA && offsetof(ProxyInfo, protocol) == 0xC);
static_assert(sizeof(ProxyDataHeader) == 0x14);
static_assert(sizeof(ProxyConfig) == 0x8);
static_assert(sizeof(ProxyConnectRequest) == 0x10 && sizeof(ProxyConnectResponse) == 0x10);
static_assert(sizeof(ProxyDisconnectMessage) == 0x14);
static_assert(sizeof(RyuNetworkConfig) == 0x28 && offsetof(RyuNetworkConfig, address_family) == 0x20);
static_assert(sizeof(ExternalProxyConfig) == 0x26 && offsetof(ExternalProxyConfig, token) == 0x16);
static_assert(sizeof(ExternalProxyToken) == 0x28 && offsetof(ExternalProxyToken, address_family) == 0x24);
static_assert(sizeof(ExternalProxyConnectionState) == 0x8);
static_assert(sizeof(CreateAccessPointRequest) == 0xBC &&
              offsetof(CreateAccessPointRequest, network_config) == 0x74 &&
              offsetof(CreateAccessPointRequest, ryu_network_config) == 0x94);
static_assert(sizeof(CreateAccessPointPrivateRequest) == 0x13C &&
              offsetof(CreateAccessPointPrivateRequest, address_list) == 0xB4 &&
              offsetof(CreateAccessPointPrivateRequest, ryu_network_config) == 0x114);
static_assert(sizeof(ConnectRequest) == 0x500 && offsetof(ConnectRequest, network_info) == 0x80);
static_assert(sizeof(ConnectPrivateRequest) == 0xC0 &&
              offsetof(ConnectPrivateRequest, network_config) == 0xA0);

/// One complete message.
struct Packet {
    PacketId type;
    std::vector<u8> data; // payload without the header
};

/// A whole message: header + payload.
std::vector<u8> Encode(PacketId type, std::span<const u8> payload = {});

template <typename T>
    requires(!std::is_same_v<T, std::vector<u8>>) // byte vectors are payloads
std::vector<u8> Encode(PacketId type, const T& message, std::span<const u8> extra = {}) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<u8> payload(sizeof(T) + extra.size());
    std::memcpy(payload.data(), &message, sizeof(T));
    if (!extra.empty()) {
        std::memcpy(payload.data() + sizeof(T), extra.data(), extra.size());
    }
    return Encode(type, payload);
}

/// The leading T of a payload (the server reads the same way); nullopt when too short.
template <typename T>
std::optional<T> Read(std::span<const u8> payload) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (payload.size() < sizeof(T)) {
        return std::nullopt;
    }
    T value;
    std::memcpy(&value, payload.data(), sizeof(T));
    return value;
}

/// What follows the leading T (advertise data, proxied bytes).
template <typename T>
std::span<const u8> Trailing(std::span<const u8> payload) {
    return payload.size() > sizeof(T) ? payload.subspan(sizeof(T)) : std::span<const u8>{};
}

/// Passphrase payload from a string (cut to 0x7F bytes so it stays zero-terminated).
PassphraseMessage MakePassphrase(const std::string& passphrase);

/// Splits a TCP byte stream into messages. After an error the stream is unusable (the server
/// closes the connection in that case too).
class Framer {
public:
    /// Appends received bytes; complete messages go to out. False on a broken stream (bad magic,
    /// version or size); error() says why.
    bool Feed(std::span<const u8> bytes, std::vector<Packet>& out);

    const std::string& error() const {
        return error_message;
    }

    /// Bytes of an incomplete message waiting for more data.
    std::size_t Pending() const {
        return buffer.size();
    }

    void Reset();

private:
    std::vector<u8> buffer;
    std::string error_message;
    bool broken = false;
};

} // namespace Network::RyuLdn
