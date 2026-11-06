// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: MIT

#include <algorithm>

#include <fmt/format.h>

#include "protocol.h"

namespace Network::RyuLdn {

const char* PacketName(PacketId id) {
    switch (id) {
    case PacketId::Initialize:
        return "Initialize";
    case PacketId::Passphrase:
        return "Passphrase";
    case PacketId::CreateAccessPoint:
        return "CreateAccessPoint";
    case PacketId::CreateAccessPointPrivate:
        return "CreateAccessPointPrivate";
    case PacketId::ExternalProxy:
        return "ExternalProxy";
    case PacketId::ExternalProxyToken:
        return "ExternalProxyToken";
    case PacketId::ExternalProxyState:
        return "ExternalProxyState";
    case PacketId::SyncNetwork:
        return "SyncNetwork";
    case PacketId::Reject:
        return "Reject";
    case PacketId::RejectReply:
        return "RejectReply";
    case PacketId::Scan:
        return "Scan";
    case PacketId::ScanReply:
        return "ScanReply";
    case PacketId::ScanReplyEnd:
        return "ScanReplyEnd";
    case PacketId::Connect:
        return "Connect";
    case PacketId::ConnectPrivate:
        return "ConnectPrivate";
    case PacketId::Connected:
        return "Connected";
    case PacketId::Disconnect:
        return "Disconnect";
    case PacketId::ProxyConfig:
        return "ProxyConfig";
    case PacketId::ProxyConnect:
        return "ProxyConnect";
    case PacketId::ProxyConnectReply:
        return "ProxyConnectReply";
    case PacketId::ProxyData:
        return "ProxyData";
    case PacketId::ProxyDisconnect:
        return "ProxyDisconnect";
    case PacketId::SetAcceptPolicy:
        return "SetAcceptPolicy";
    case PacketId::SetAdvertiseData:
        return "SetAdvertiseData";
    case PacketId::Ping:
        return "Ping";
    case PacketId::NetworkError:
        return "NetworkError";
    }
    return "Unknown";
}

const char* ErrorName(NetworkError error) {
    switch (error) {
    case NetworkError::None:
        return "None";
    case NetworkError::PortUnreachable:
        return "PortUnreachable";
    case NetworkError::TooManyPlayers:
        return "TooManyPlayers";
    case NetworkError::VersionTooLow:
        return "VersionTooLow";
    case NetworkError::VersionTooHigh:
        return "VersionTooHigh";
    case NetworkError::ConnectFailure:
        return "ConnectFailure";
    case NetworkError::ConnectNotFound:
        return "ConnectNotFound";
    case NetworkError::ConnectTimeout:
        return "ConnectTimeout";
    case NetworkError::ConnectRejected:
        return "ConnectRejected";
    case NetworkError::RejectFailed:
        return "RejectFailed";
    case NetworkError::BannedByServer:
        return "BannedByServer";
    case NetworkError::Unknown:
        return "Unknown";
    }
    return "?";
}

std::vector<u8> Encode(PacketId type, std::span<const u8> payload) {
    Header header{};
    header.magic = Magic;
    header.type = static_cast<u8>(type);
    header.version = ProtocolVersion;
    header.data_size = static_cast<s32>(payload.size());

    std::vector<u8> out(HeaderSize + payload.size());
    std::memcpy(out.data(), &header, HeaderSize);
    if (!payload.empty()) {
        std::memcpy(out.data() + HeaderSize, payload.data(), payload.size());
    }
    return out;
}

PassphraseMessage MakePassphrase(const std::string& passphrase) {
    PassphraseMessage message{};
    const std::size_t n = std::min(passphrase.size(), message.passphrase.size() - 1);
    std::memcpy(message.passphrase.data(), passphrase.data(), n);
    return message;
}

void Framer::Reset() {
    buffer.clear();
    error_message.clear();
    broken = false;
}

bool Framer::Feed(std::span<const u8> bytes, std::vector<Packet>& out) {
    if (broken) {
        return false;
    }
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());

    std::size_t pos = 0;
    while (buffer.size() - pos >= HeaderSize) {
        Header header;
        std::memcpy(&header, buffer.data() + pos, HeaderSize);
        if (header.magic != Magic) {
            error_message = fmt::format("bad magic 0x{:08X}", header.magic);
            broken = true;
            return false;
        }
        if (header.version != ProtocolVersion) {
            error_message = fmt::format("protocol version {} (expected {})", header.version,
                                        ProtocolVersion);
            broken = true;
            return false;
        }
        if (header.data_size < 0 || static_cast<std::size_t>(header.data_size) > MaxPayloadSize) {
            error_message = fmt::format("bad payload size {}", header.data_size);
            broken = true;
            return false;
        }
        const std::size_t total = HeaderSize + static_cast<std::size_t>(header.data_size);
        if (buffer.size() - pos < total) {
            break;
        }
        Packet packet;
        packet.type = static_cast<PacketId>(header.type);
        packet.data.assign(buffer.begin() + pos + HeaderSize, buffer.begin() + pos + total);
        out.push_back(std::move(packet));
        pos += total;
    }
    buffer.erase(buffer.begin(), buffer.begin() + pos);
    return true;
}

} // namespace Network::RyuLdn
