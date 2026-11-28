# Recording the pipeline list

`extras/pipelines.list` lists the shader pipelines (a shader program plus its blend / depth / vertex layout / target
formats) the game uses in the common areas. `make` copies it next to the executable and `make dist` ships it. On a
first run the game builds them in the background, so entering those areas for the first time doesn't show surfaces
black for a moment. Background: [STATUS.md](STATUS.md), "Shipped pipeline list".

The game never says which combinations it will use, so they are recorded by playing. Every computer records the
pipelines it draws in `cache/pipelines.bin` next to the game; exporting turns those into a list that works on any
GPU and keeps working after shader translator changes. The list holds only hashes of the game's shader code and
render state, nothing of the game itself.

## What to record

- Pipelines with MSAA are separate entries: record each MSAA setting players use (Off, 2x, 4x, 8x). The HUD and menus
  don't change with MSAA, so the town and a few hunts per setting are enough there.
- Areas: the hubs (village, Hub, Bherna...) and the hunting areas players visit most. Each area's terrain, effects
  and monsters add their own entries; the more areas, the fewer first-visit black surfaces.
- FSR may change some entries (not checked): if players use it, record a hunt or two with it on. Resolution, frame
  rate and draw distance don't change pipelines.
- HDR draws its post-processing and HUD with pipelines of its own (RGBA16F targets, HDR variants of the colour
  grading, bloom and HUD). Record the town and a hunt or two with HDR on and `UnclampedScene = Off`: those entries
  are marked as HDR's, so the game builds them only while the window is HDR. With `UnclampedScene = On` the game
  makes the RGBA16F twin of every other entry by itself, so that setting needs no recording of its own (entries
  recorded with it are marked too and only built with it).

## Steps (Windows)

You need a build from this repository at or after commit 7cc82f1 (`make`; the game is `build\mhgurecomp.exe`).

1. Set the MSAA you want to record (F1 menu, Graphics & Audio, or `MSAA` in `build\mhgurecomp.ini`), restart if asked.
2. Play: walk through the town and do a few hunts in different areas. Everything you see is recorded in
   `build\cache\pipelines.bin` as you play; nothing else to do.
3. Quit the game. Export, from the repository folder in PowerShell:
   ```powershell
   $env:MHGU_PIPELIST_EXPORT = "$PWD\pipelines.list.new"; Start-Process build\mhgurecomp.exe
   ```
   The export happens at the first frame the game draws. `build\log.txt` then has a line like
   `[vk] pipeline list exported: ...\pipelines.list.new (1520 pipelines)`. Quit the game.
4. Replace the list and copy it next to the game:
   ```powershell
   Move-Item -Force pipelines.list.new extras\pipelines.list; make
   ```
   (From the MSYS2 shell: `mv -f pipelines.list.new extras/pipelines.list && make`.)
5. Repeat from step 1 for the next MSAA setting. Each export contains the current list plus everything new, so the
   list only grows.
6. Commit `extras/pipelines.list`.

On Linux the same, with `MHGU_PIPELIST_EXPORT=$PWD/pipelines.list.new ./build-linux/mhgurecomp` (or wherever the build
is) in step 3.

## Things to know

- Export with the same build you played with: `cache/pipelines.bin` is thrown away (silently started over) when the
  shader translator changes (`runtime/nvn/maxwell/translate.c`, `shader.h`). Don't delete `cache\` before exporting.
- The export merges the list next to the executable (`build\pipelines.list`) with `cache\pipelines.bin`. Pipelines the
  game built from the list aren't written to `pipelines.bin`, so step 4's `make` (which copies the new list there)
  matters before the next round.
- Lists from several computers combine the same way: export on one computer with the other's list in `extras/` (and
  `make`), and the result has both.
- If the log says `not a pipeline list of this version, not used`, the shader key or pipeline record layout changed in
  the code (`ShaderKey`, `PipeKey` in `runtime/nvn/vulkan/exec.c`): the list has to be recorded again.
- Checking it worked: start with an empty `cache\` (rename it) and `MHGU_VK_STATS=1`; the log's `[vk] 2s builds: ...
  skipped N` lines show how many draws were skipped while shaders were built. `MHGU_NO_PIPELIST=1` turns the list off
  for comparing.
