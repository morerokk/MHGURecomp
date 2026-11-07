// A small Ryujinx-LDN-compatible server inside the game (runtime/net/ryusrv/): friends
// connect their Ryujinx (LDN server = this PC's address) or this game (RyuServers) to it, nobody needs a separate LdnServer.
// ini [Multiplayer] RyuServe = On, RyuServePort.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ryusrv_start(int port, char *err, int err_size);  // listens on every adapter; true when it runs
void ryusrv_stop(void);
bool ryusrv_running(void);

#ifdef __cplusplus
}
#endif
