// A suyu-style multiplayer room inside the game (runtime/net/roomsrv.c, written from the room protocol): suyu, yuzu,
// Eden and Citron players join it as they would any room, so nobody needs a room server of their own.
// ini [Multiplayer] RoomServe = On, RoomServePort. This game joins it as the host member (its hubs are shared with it).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the room on every adapter. host_nick: the member that acts as the room's host (it also answers for the
// addresses of the Ryujinx players). true when it runs.
bool roomsrv_start(int port, const char *name, const char *password, const char *host_nick, char *err, int err_size);
void roomsrv_stop(void);
bool roomsrv_running(void);

#ifdef __cplusplus
}
#endif
