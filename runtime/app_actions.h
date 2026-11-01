#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_capture_launch_args(int argc, char **argv);
void ui_request_action(bool restart); // Called after the in-game confirmation, handled on the host thread.
bool ui_action_failed(void);          // Take and clear a failed restart notification.
void ui_request_import(int what);     // Android: 1 the game files, 2 a save, from a folder the player picks.
#ifdef __cplusplus
}
#endif
