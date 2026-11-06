// The types of the suyu tree's announce header that the room uses (no web service backend here).
#pragma once
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/socket_types.h"
namespace AnnounceMultiplayerRoom {
struct GameInfo { std::string name{""}; u64 id{0}; std::string version{""}; };
struct Member { std::string username, nickname, display_name, avatar_url; Network::IPv4Address fake_ip; GameInfo game; };
struct RoomInformation { std::string name, description; u32 member_slots{}; u16 port{}; GameInfo preferred_game; std::string host_username; };
}
