// Pause/Suspend ([Controls] PauseKey, the Pause key by default; P on the Mac): the game stops where it is, offline only.
//
// The game's render thread is held in its present (runtime/nvn/objects.c nvnQueuePresentTexture), which stops the
// game: its main thread waits for the render thread before the next frame. Meanwhile the held thread shows the last
// picture again ~60 times a second, dimmed with PAUSED over it (runtime/overlay.cpp), so the settings menu and Forge's
// stay usable. The guest's clock stops too (runtime/kernel.c rt_ticks: the system tick, the steady clock), so the
// game sees no time pass, and the audio mixer renders nothing (runtime/services/audio.c): voices stay where they are.
//
// Refused while the game is on a network of any kind (a hub, Hunter Search: nn::ldn past Initialize) or writes files
// (a save: runtime/services/fs.c fs_writing). The pause key, Esc or Enter resume.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Window thread: the pause key. Pauses (or says why it can't, as a notice), or resumes.
void pause_toggle(void);
// Window thread: Esc / Enter. True: the game was paused and resumes (the key is taken).
bool pause_resume(void);
// A pause asked for or holding (until the game runs again)
bool pause_wanted(void);
// The game is held: the picture stays, the overlay dims it (any thread)
bool pause_held(void);
// The game's render thread, after each present: holds while a pause is wanted, showing the picture again with
// present(arg) and building the overlay's frames with the render thread's cpu (the main thread waits meanwhile).
void pause_frame(void *cpu, void (*present)(void *), void *arg);
// The guest's clock, in host nanoseconds: plat_now_ns() less the time spent paused (stopped while held)
uint64_t pause_clock_ns(void);

#ifdef __cplusplus
}
#endif
