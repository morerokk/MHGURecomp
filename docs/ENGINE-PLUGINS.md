# Engine plugins

Engine plugins are native code (`plugins/<name>/<name>.dll`, or `.so`) that extends the port itself: how it renders or
runs. **They are not mods**, and they do not touch mods.

| | Mods (`mods/`) | Forge plugins | Engine plugins (`plugins/`) |
|---|---|---|---|
| What they change | the game's data and code | the game's code (ARM `.nro`) | the port (host side) |
| Run as | files layered over RomFS, patches | guest code, interpreted | native code in the port's process |
| Safe to drop in blindly | yes | mostly | **no**, it is arbitrary native code |

* **Opt-in.** A plugin loads only when `plugins/enabled.txt` lists its folder name (one per line, `#` comments). A fresh
  install loads none, and with no `enabled.txt` nothing in the loader runs.
* **Independent of mods.** A plugin never enables, disables, reorders or changes a mod or a Forge plugin; mods load the
  same with or without plugins. (A plugin that wants to work with mods would need an explicit API for it; there is none.)
* `MHGU_PLUGINS_DIR` moves the folder, like `MHGU_MODS_DIR`. Not available on Android.

## The port has no hooks for any plugin

The port knows nothing of what a plugin does. There is no "upscaler provider" or any other feature-specific seam in
`device.c` or `fsr.c`. A plugin applies its changes itself, in memory, once it is loaded: by intercepting calls the port
makes (the way a patch would). That keeps a feature the port's maintainers cannot test out of the port's code, and puts
it, and its upkeep, with the plugin.

The loader (`runtime/plugin.c`, API in `runtime/plugin_api.h`) does only this:

1. read `enabled.txt`; for each name (letters, digits, `-_.`; nothing that leaves the folder) load `plugins/<name>/<name>.dll`;
2. call its `mhgu_plugin_entry(host, out)`; the host table has `log`, `setting` (read a key of `mhgurecomp.ini`) and the
   plugin's own folder; the plugin returns its name and an optional `shutdown`;
3. a plugin that is missing, not a plugin, built for a newer API, or says no is logged and skipped; the game carries on.

Both tables start with their `size` and `api` and only grow at the end, so a plugin built for API 1 keeps loading.
The entry runs once, early, after the settings are read and before the window, renderer and game exist.

## What the first plugin does (FSR 4)

FSR 4 lives in its own repository (`mhgu-fsr4-plugin`). Against the port's FSR 3.1 it:

* intercepts `vkCreateInstance` / `vkCreateDevice` through the exe's import table: Vulkan 1.3 for the instance, and the
  device extensions and features AMD's models use, where the GPU has them;
* intercepts `ffxVkPortableUpscaleContextRecordDispatch`, the FSR 3.1 call the exe exports, which carries every input
  FSR 4 needs (colour, depth, motion vectors, reactive mask, jitter, camera, sizes), and records AMD's FSR 4 into the same
  command buffer;
* on any failure puts the original code back for good: FSR 3.1 runs that frame from a fresh history.

With `[Upscaler] FSR = Off` the plugin touches nothing. AMD's DLLs are fetched by that repository's `make` (SHA-256
checked) and sit in the plugin's folder, never in this repository.

What a plugin relies on in the port is its own to check: if the port renames the function it intercepts or changes a
struct it reads (FSR 4 checks `structSize`), the plugin declines to load or steps aside and logs why.

## Trying it

    make example-plugin          # a plugin that only logs, into obj/plugin-test/plugins/example
    make test-plugins            # the loader against a good plugin and each way one can fail

Copy a plugin's folder to `plugins/` and add its name to `plugins/enabled.txt`.
