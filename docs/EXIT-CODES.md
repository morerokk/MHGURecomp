# Exit codes

For launchers and mod managers: the process's exit code says how the game ended. Windows and Linux (desktop) only.
Android gives an app no exit code anyone could read.

| Code | Meaning |
|---|---|
| 0 | Closed normally: the window closed, Quit Game, Exit from the title screen, the game's own exit |
| 20 | Crashed in the port's own code: the runtime, the renderer, or a library or driver it called |
| 21 | Crashed in the game's code, or the game gave up: a failed check (`nn::diag` abort), `abort()`, `svcBreak` |
| 22 | Crashed in a Forge plugin's code, or the game gave up during a plugin's call (a file read past the end) |
| 23 | Crashed in a mod's subsdk code, or a mod's subsdk can't be loaded (not an NSO, no room for it) |
| 24 | Crashed in an engine module's native code, or a library its folder brought along |
| 30 | Can't start on this PC: no AVX2, no Vulkan 1.1 driver, damaged or missing game files. A message box said why |
| 31 | Closed to restart (the settings menu's Restart): a new process takes over, the code doesn't mean it ended |

**A crash's code says whose code was running when it happened, not who caused it.** A mod that writes over the game's
memory makes the game crash later, in the game's own code (21); a port bug can do the same. Word it as "the game crashed
in a Forge plugin", not "the plugin's fault". `log.txt` has the details either way, ending with a line such as
`[exit] code 22 (a crash in a Forge plugin)`.

How a crash is put in a group:

* **A fault (an access violation, SIGSEGV...)** goes by where it happened. While guest code is being interpreted (a mod's
  subsdk, a Forge plugin, a game function a patch or a hook changed), the module that holds that instruction: the game
  (main, sdk), a mod's subsdk, a Forge plugin. In the game's recompiled code: the game. In a library loaded from an engine
  module's folder: that engine module. Anything else, including the port's code called by a mod (a host `memset` into
  memory the mod passed): the port.
* **The game giving up** (abort, a failed check, exit with an error) goes by the code that called it: a mod's code, or a
  Forge plugin's file call, is that mod's; else the game's.
* **A jump to where no code is** goes by the code that jumped there (its return address): a mod's, or else the port's (a
  function the recompilation missed). A jump a mod's patches meant for a subsdk that moved: the subsdk mod.

Codes the port never uses on purpose, so they mean something else:

* **1** on Windows: the process was killed (Task Manager), or something ended before the port's code ran.
* **3** on Windows: a C runtime `abort()` the crash handler never saw.
* **128 and up** on Linux: killed by a signal (137 = SIGKILL, 143 = SIGTERM). On Windows, codes such as `0xC0000005`
  (access violation) or `0xC00000FD` (stack overflow) are crashes the handler couldn't catch.

Treat any other nonzero code as "crashed, cause unknown". A game that hangs never exits, so it gets no code at all.
