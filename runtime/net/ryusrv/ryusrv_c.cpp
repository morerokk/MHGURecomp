// The C entry points of the embedded Ryujinx LDN server (ryusrv.h).
#include "../ryusrv.h"

#include <memory>
#include <string>

#include "server.h"

static std::unique_ptr<Network::RyuLdn::Server> g_server;

extern "C" bool ryusrv_start(int port, char *err, int err_size) {
    if (g_server) return true;
    auto s = std::make_unique<Network::RyuLdn::Server>();
    std::string error;
    if (!s->Start("", static_cast<u16>(port), error)) {
        if (err && err_size > 0) std::snprintf(err, static_cast<size_t>(err_size), "%s", error.c_str());
        return false;
    }
    g_server = std::move(s);
    return true;
}
extern "C" void ryusrv_stop(void) { g_server.reset(); }
extern "C" bool ryusrv_running(void) { return g_server != nullptr; }
