#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_capture_launch_args(int argc, char **argv);
void ui_request_action(bool restart); // Called after the in-game confirmation, handled on the host thread.
void ui_request_exit_menu(void);      // Title screen's Main Menu, B: the window-close question (host thread).
bool ui_action_failed(void);          // Take and clear a failed restart notification.
void ui_request_import(int what);     // Android: 1 the game files, 2 a save in, 3 the save out, 4 the save's folder,
                                      // 5 a mod from a zip, 6 a mod from a folder, 7 the game files' folder.
// PC: request 4 opens the system's folder picker for the save's folder.
// The folder chosen by request 4 or 7 (7: Android only), once: which of them (0 none).
int ui_take_picked_folder(char *out, size_t size);
#ifdef __cplusplus
}
#endif
