// The C entry points of the embedded room (roomsrv.h).
#include "../roomsrv.h"

#include <cstdio>
#include <memory>
#include <string>

#include "room.h"

static std::unique_ptr<Network::Room> g_room;

extern "C" bool roomsrv_start(int port, const char *name, const char *password, const char *host_nick, char *err, int err_size) {
    if (g_room) return true;
    auto room = std::make_unique<Network::Room>();
    if (!room->Create(name ? name : "MHGU", "", "", static_cast<u16>(port), password ? password : "", 16,
                      host_nick ? host_nick : "")) {
        if (err && err_size > 0) std::snprintf(err, static_cast<size_t>(err_size), "the port may be in use");
        return false;
    }
    room->SetHostGateway(true);
    g_room = std::move(room);
    return true;
}
extern "C" void roomsrv_stop(void) {
    if (g_room) g_room->Destroy();
    g_room.reset();
}
extern "C" bool roomsrv_running(void) { return g_room != nullptr; }
