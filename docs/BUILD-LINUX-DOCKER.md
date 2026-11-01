# Build both Linux packages

Use one x86-64 container to build the Linux x86-64 and ARM64 packages.
The compiler runs on x86-64 for both targets.
QEMU runs small ARM64 checks, not the compiler.
These packages target Linux with glibc, not Android.

## Requirements

- An x86-64 Linux host.
- Git, Bash, `tar`, `flock`, `realpath`, and `sha256sum`.
- A local Docker engine, or local Podman.
- Your own complete MHGU 1.4.0 dump.
- Internet access for the first build.

Allow 20 GB of free space, excluding your dump, and 8 GB of RAM.
These are planning margins, not measured minimum requirements.
Start with two jobs.
The clean build time has not been measured.

The image uses Ubuntu 24.04, Clang 18, and GCC 13 target runtimes.
It includes an external ARM64 sysroot from signed Ubuntu packages.
It does not execute ARM64 package installers.
The Makefile builds its pinned SDL3, shaderc, and FFmpeg sources.
The existing glibc 2.39 and GLIBCXX_3.4.32 ceilings apply.

## Build

Run these commands from the repository root.
Commit or restore changes to tracked files first.
The script exports the current commit with `git archive`.
It excludes untracked files, including previous build folders.

1. Check access to Docker:

   ```bash
   docker info
   ```

2. Build both packages:

   ```bash
   bash tools/build-linux-release.sh --game "/absolute/path/to/data" \
     --version 0.12.0 --jobs 2
   ```

   The dump folder must contain `exefs/main`, `exefs/sdk`, and `romfs/`.
   The script builds the tools image before it compiles the game.
   It generates the game C once and builds each target separately.

3. Check the package hashes:

   ```bash
   cd dist
   sha256sum -c SHA256SUMS
   ```

The output files are:

- `dist/Linux-MHGURecomp.0.12.0.tar.gz`.
- `dist/LinuxARM64-MHGURecomp.0.12.0.tar.gz`.
- `dist/SHA256SUMS`.

Each archive contains the runtime, game library, updater, licenses, and pipelines.
It does not contain your dump, saves, generated C, or donor content.
Creating a package does not establish permission to distribute game-derived code.
The project license does not cover the game or its recompiled code.

## Test the packages

1. Extract the x86-64 archive into a new folder on your PC.
2. Put your own `exefs` and `romfs` folders in its `data/` folder.
3. Start `mhgurecomp` from the extracted folder.
4. Extract the ARM64 archive into a new folder on your Linux ARM64 device.
5. Put your own `exefs` and `romfs` folders in its `data/` folder.
6. Start `mhgurecomp` on that device.

Check graphics, audio, controls, and save loading on both targets.
Back up your saves before a device test.
The default save location remains `~/.local/share/MHGURecomp/save/`.
The `MHGU_HOME` setting does not move that save location.

Do not start the updater during this first test.
It still contacts the original repository, not your fork.
This workflow does not publish a game release.

## Podman and Fedora

Use this command with local rootless Podman:

```bash
CONTAINER_ENGINE=podman bash tools/build-linux-release.sh \
  --game "/absolute/path/to/data" --version 0.12.0 --jobs 2
```

The script preserves the host user with `--userns=keep-id`.
With `crun`, rootless Podman also preserves supplementary groups.
Rootful Docker uses your host UID and GID.
It also adds your supplementary groups.
Rootless Docker uses its default user mapping.
With rootless Docker or Podman without `crun`, use an owner-readable dump.
Access through supplementary groups alone might not work in those modes.
Do not add `sudo` to the build command.

On SELinux hosts, the work and output mounts use `:Z`.
The dump mount uses `:ro,z` for shared read access.
These options relabel only the selected folders.
Use a dedicated dump folder, not your whole home folder.
The script does not disable SELinux or request privileged containers.

## Cache and failures

The ignored `.linux-release/` folder holds sources, objects, and generated C.
It contains private game-derived files after a release build.
Do not upload or share that folder.
Each source commit and builder image has a separate cache.
The script never uses your existing `build-arm64/` or `obj-arm64/`.

A failed target stops the command.
The script copies packages to `dist/` only after both targets build.
On failure, previous packages in `dist/` can still exist.
Use the exit status and new hash check to identify a successful build.
Rerun the same command to reuse compatible build dependencies.
The script generates fresh game C on each release run.

Remote Docker contexts and remote Podman connections are not supported.
Bind mounts must refer to files on this host.
Dump and checkout paths can contain spaces, but not colons or newlines.
Run one build at a time in a checkout.

## CI and the public tools image

The Linux builder workflow checks both targets without game files.
Each job compiles the runtime and updater and runs public-code tests.
These jobs do not produce playable releases.
Only the local release command compiles your game library.

The upstream 0.12 navigation test still skips the new Cheats tab.
CI runs that test as an advisory check and reports any failure.
The other build, architecture, ABI, and regression checks remain required.
This change does not modify the upstream menu or its test.

After both jobs pass, trusted branch pushes can publish the tools image.
PR jobs do not publish images.
The image name is `ghcr.io/<owner>/mhgurecomp-builder`.
Tags identify the source commit and the moving `linux-amd64` version.
The owner may need to make the new GHCR package public.
Public repository visibility does not make a new package public.

To reuse a published image:

```bash
docker pull ghcr.io/<owner>/mhgurecomp-builder:<commit>
bash tools/build-linux-release.sh --game "/absolute/path/to/data" \
  --version 0.12.0 --jobs 2 \
  --image ghcr.io/<owner>/mhgurecomp-builder:<commit> --skip-image-build
```

The default command builds the image locally and does not require GHCR.
The build context permits only selected files under `tools/`.
The dump enters only a read-only runtime mount, never an image layer.
APT package manifests and hashes remain under `/opt/mhgu/` in the image.
Ubuntu repositories can change, so rebuilds are not necessarily byte-identical.
