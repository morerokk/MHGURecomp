// Typed chat anywhere (runtime/hle/chat.c): "/" opens a line to type; Enter sends it as the game sends a shoutout.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Cpu;
void chat_init(void);                          // at start: a message's later rows don't repeat the sender's name
void chat_open(void);                          // window side: "/" pressed (no text entry or menu open)
void chat_close(void);                         // the game opened its own text entry
bool chat_composing(char *text, size_t size);  // the overlay: the line being typed
bool chat_active(void);
// Where the box is, as fractions of the window: its left edge and its bottom (it grows upwards). The overlay reports
// the box it drew; the mouse drags it while typing (runtime/host_input.c); chat_box.txt keeps it.
void chat_position(float *left, float *bottom);
void chat_set_box(float x0, float y0, float x1, float y1);
bool chat_in_box(float x, float y);
void chat_move(float dx, float dy);
void chat_save_position(void);
void chat_tick(struct Cpu *c);                 // game thread, each frame (its input polling): send what was typed

#ifdef __cplusplus
}
#endif
