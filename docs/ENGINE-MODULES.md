# Engine modules

Engine modules are native code (`engine_modules/<name>/<name>.dll`, or `.so`) that extends the port itself: how it renders or
runs. **They are not mods and not Forge plugins**, and they do not touch either. Say "engine module", or "module" where it can't
be mixed up; "plugin" on its own means a Forge plugin.

| | Mods (`mods/`) | Forge plugins | Engine modules (`engine_modules/`) |
|---|---|---|---|
| What they change | the game's data and code | the game's code (ARM `.nro`) | the port (host side) |
| Run as | files layered over RomFS, patches | guest code, interpreted | native code in the port's process |
| Safe to drop in blindly | yes | mostly | **no**, it is arbitrary native code |

* **Opt-in.** A module loads only when `engine_modules/enabled.txt` lists its folder name (one per line, `#` comments). A fresh
  install loads none, and with no `enabled.txt` nothing in the loader runs. This guards against loading one by accident, not
  against a malicious download: whatever can put files in the game's folder could also put a DLL beside the executable.
* **Independent of mods.** A module never enables, disables, reorders or changes a mod or a Forge plugin; mods load the
  same with or without modules. (A module that wants to work with mods would need an explicit API for it; there is none.)
* `MHGU_ENGINE_MODULES_DIR` moves the folder, like `MHGU_MODS_DIR`. Not available on Android.

## The port has no hooks for any module

The port knows nothing of what a module does. There is no "upscaler provider" or any other feature-specific seam in
`device.c` or `fsr.c`. A module applies its changes itself, in memory, once it is loaded: by intercepting calls the port
makes (the way a patch would). That keeps a feature the port's maintainers cannot test out of the port's code, and puts
it, and its upkeep, with the module.

**There is no stable internal API.** Only `runtime/engine_module_api.h` is one. The port's other symbols can be reached (the
executable exports them all) but may change in any release, with no notice. What a module relies on in the port is its own to
check: by `port_version`, or what it finds (FSR 4 checks `structSize`). When it doesn't know what it finds, it declines to load
or steps aside and logs why.

The loader (`runtime/engine_module.c`, API in `runtime/engine_module_api.h`) does only this:

1. read `enabled.txt`; for each name (letters, digits, `-_.`; nothing that leaves the folder) load
   `engine_modules/<name>/<name>.dll`;
2. read its export `mhgu_engine_module_api`: a module built for a newer API than this port's is refused before any of its code
   is called;
3. log that it starts it, then call its `mhgu_engine_module_entry(host, out)`; the host table has `log`, `setting` (read a key of
   `mhgurecomp.ini`), the module's own folder and `port_version`; the module gives its name and an optional `shutdown`;
4. a module that is missing, not a module, built for a newer API, or says no is logged and skipped; the game carries on.

Both tables only grow at the end, so a module built for API 1 keeps loading. The entry runs once, early: the window exists, the
renderer and the game do not yet. A module that returns false has undone anything it changed: it is unloaded straight away.

`shutdown` runs once at a clean exit, last loaded first. With the renderer up that is from its exit handler, after the GPU has
finished and just before the process ends (`nvn/vulkan/device.c`): be quick, and don't count on it after a crash. A crash report
in the log names the modules that were loaded.

## What the first module does (FSR 4)

FSR 4 lives in its own repository (`mhgu-fsr4-plugin`). Against the port's FSR 3.1 it:

* intercepts `vkCreateInstance` / `vkCreateDevice` through the exe's import table: Vulkan 1.3 for the instance, and the
  device extensions and features AMD's models use, where the GPU has them;
* intercepts `ffxVkPortableUpscaleContextRecordDispatch`, the FSR 3.1 call the exe exports, which carries every input
  FSR 4 needs (colour, depth, motion vectors, reactive mask, jitter, camera, sizes), and records AMD's FSR 4 into the same
  command buffer;
* on any failure puts the original code back for good: FSR 3.1 runs that frame from a fresh history.

With `[Upscaler] FSR = Off` the module touches nothing. AMD's DLLs are fetched by that repository's `make` (SHA-256
checked) and sit in the module's folder, never in this repository.

## Trying it

    make example-engine-module    # a module that only logs, into obj/engine-module-test/engine_modules/example
    make test-engine-modules      # the loader against a good module and each way one can fail

Copy a module's folder to `engine_modules/` and add its name to `engine_modules/enabled.txt`.
