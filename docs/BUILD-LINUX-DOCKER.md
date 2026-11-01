# Build both Linux packages

Use one x86-64 Docker container to build the Linux x86-64 and ARM64 packages.
The compiler runs on x86-64 for both targets.
QEMU runs small ARM64 checks, not the compiler.
These packages target Linux with glibc, not Android.

## Requirements

- An x86-64 Linux host.
- Git, Bash, `tar`, `flock`, `realpath`, and `sha256sum`.
- A local Docker engine.
- Your own complete MHGU 1.4.0 dump.
- Internet access for the first build.

Install Docker with its [official installation guide](https://docs.docker.com/engine/install/).
Use the [Linux post-installation guide](https://docs.docker.com/engine/install/linux-postinstall/)
or [rootless mode](https://docs.docker.com/engine/security/rootless/) for access without `sudo`.
The `docker` group grants root-level privileges.
Do not run the build command with `sudo`.

Allow 20 GB of free space, excluding your dump, and 8 GB of RAM.
These are planning margins, not measured minimum requirements.
Start with two jobs.
The clean build time has not been measured.

The image uses Ubuntu 24.04, Clang 18, and GCC 13 target runtimes.
It includes an external ARM64 sysroot from signed Ubuntu packages.
It does not execute ARM64 package installers.
The Makefile builds its pinned SDL3, shaderc, and FFmpeg sources.
The existing glibc 2.39 and GLIBCXX_3.4.32 ceilings apply.

## Use the published tools image

This is the recommended path when a public image is available.
GitHub Actions checks public code and publishes the tools image to GHCR.
It does not publish playable packages in Actions artifacts.
You compile your own game library locally.
You do not need a fork, a GitHub token, or your own Actions run.
Public GHCR images permit anonymous downloads.

1. Check access to the local Docker server without `sudo`:

   ```bash
   docker info
   ```

2. Open the repository's **Actions** tab.
3. Select **Linux builder**.
4. Select a successful run on `master` or `linux-arm64-0118`.
   Use a repository and branch you trust.
   Do not select a PR run.
5. Make sure both architecture jobs and **Publish the tools image** passed.
6. Copy the full source commit SHA from that run.
7. Clone the same repository and select that commit:

   ```bash
   REPOSITORY=morerokk/MHGURecomp
   OWNER=${REPOSITORY%%/*}
   read -r -p "Full source SHA from the successful run: " REVISION
   git clone "https://github.com/$REPOSITORY.git" MHGURecomp-linux
   cd MHGURecomp-linux
   git checkout --detach "$REVISION"
   IMAGE="ghcr.io/${OWNER,,}/mhgurecomp-builder:$REVISION"
   ```

   For a fork, replace `morerokk/MHGURecomp` before you run the commands.
   The image owner must match the repository owner.
   A fork's publication does not create an upstream image.
   Upstream needs its own successful publication after merge.

8. Download the image for that same SHA:

   ```bash
   docker pull "$IMAGE"
   ```

9. Build both packages from the repository root:

   ```bash
   bash tools/build-linux-release.sh --game "/absolute/path/to/data" \
     --version 0.12.0 --jobs 2 --image "$IMAGE" --skip-image-build
   ```

   The dump folder must contain `exefs/main`, `exefs/sdk`, and `romfs/`.
   The script generates the game C once and builds each target separately.

10. Check the package hashes:

   ```bash
   (cd dist && sha256sum -c SHA256SUMS)
   ```

**Use a published SHA, not an arbitrary checkout SHA.**
The workflow's path filters exclude documentation-only changes.
Those pushes do not create an image tag.
The moving `linux-amd64` tag is not a fixed source revision.
Use the full-SHA tag for the matching source and builder.

If no public image exists, use the local builder option below.
Do not send your dump to a maintainer or upload it to resolve a pull failure.
For a missing tag, check the publication job and source SHA.
For an access error, ask the image owner to check package visibility.

The script exports the current commit with `git archive`.
Commit or restore tracked changes before a build.
Untracked files and previous build folders do not enter that source export.

## Build the tools image locally

Use this option if GHCR is unavailable or no suitable image exists.
It does not require GitHub Actions, GHCR access, or a GitHub token.
Clone the repository and select the source revision you want to build.
Run this command from its root:

```bash
bash tools/build-linux-release.sh --game "/absolute/path/to/data" \
  --version 0.12.0 --jobs 2
```

Without `--skip-image-build`, the script first builds the tools image locally.
It then builds both packages.
Use a clean tracked checkout, as with the published-image path.

## Package contents and privacy

The output files are:

- `dist/Linux-MHGURecomp.0.12.0.tar.gz`.
- `dist/LinuxARM64-MHGURecomp.0.12.0.tar.gz`.
- `dist/SHA256SUMS`.

Each archive contains the runtime, game library, updater, licenses, and pipelines.
The game library, `mhgugame.so`, contains game-derived code.
It does not contain your dump, saves, generated C, or donor content.
Creating a package does not establish permission to distribute game-derived code.
The project license does not cover the game or its recompiled code.
Keep these builds private.
Do not upload them to CI or GHCR.

The dump enters only a read-only local container mount, never an image layer.
The build context permits only selected files under `tools/`.
Neither GitHub Actions nor GHCR receives your dump or generated game code.
Only the local release command compiles your game library.

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

## Docker on Fedora and SELinux

Use the same Docker build commands on Fedora.
Rootful Docker uses your host UID and GID.
It also adds your supplementary groups.
Rootless Docker uses its default user mapping.
Keep the dump readable by your host user.
Supplementary-group access alone might not work with rootless Docker.
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

Remote Docker contexts are not supported.
Bind mounts must refer to files on this host.
Dump and checkout paths can contain spaces, but not colons or newlines.
Run one build at a time in a checkout.

## CI scope

The Linux builder workflow checks both targets without game files.
Each job compiles the runtime and updater and runs public-code tests.
These jobs do not produce playable releases.
The builder itself remains `linux/amd64` for both targets.
ARM64 target checks use QEMU.
CI does not validate graphics or gameplay.

The upstream 0.12 navigation test still skips the new Cheats tab.
CI runs that test as an advisory check and reports any failure.
The other build, architecture, ABI, and regression checks remain required.
This change does not modify the upstream menu or its test.

## Maintainer: Actions and GHCR after merge

This setup is for repository maintainers, not users who download a public image.
Review the workflow before you approve a run from a fork.
GitHub can require [manual approval for fork PRs](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/approve-runs-from-forks).
PR runs do not publish images or need game files.

### Permit the workflow

Actions is normally enabled, but repository or organization policies can restrict it.
A merge does not change those policies.

1. Open the repository's **Settings → Actions → General**.
2. Check the [Actions permissions](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/enabling-features-for-your-repository/managing-github-actions-settings-for-a-repository).
3. Permit the four actions that this workflow uses:
   `actions/checkout`, `docker/setup-buildx-action`,
   `docker/build-push-action`, and `docker/login-action`.
   Keep their full-SHA pins in the workflow.
4. If **Linux builder** is disabled, open it under **Actions**.
5. Select **Enable workflow**.

See GitHub's [workflow enable instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/disable-and-enable-workflows).
Do not enable global write access for all workflows.
The publisher already requests `packages: write` for its own job.
It uses the repository's `GITHUB_TOKEN`.
No personal access token is needed for this workflow.

### Run the first upstream publication

The merge into `master` should trigger a run when Actions permits it.
The merge includes files that match this workflow's path filters.
Both architecture jobs must pass before the publisher starts.
See GitHub's [trigger rules](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).

If the merge did not start a run, use the manual trigger after merge:

1. Open **Actions → Linux builder**.
2. Select **Run workflow**.
3. Select `master`.
4. Select **Run workflow** to start it.
5. Wait for both architecture jobs and **Publish the tools image** to pass.

The manual trigger requires write access.
The workflow file must exist on the default branch for this control to appear.
See [Manually running a workflow](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).
Documentation-only pushes do not start this workflow.

### Make the image public

The workflow publishes `ghcr.io/<owner>/mhgurecomp-builder`.
The owner is the repository owner in lowercase.
It writes a full source-SHA tag and the moving `linux-amd64` tag.
A new GHCR package is private by default, even for a public repository.
See the [Container registry guide](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry).

1. Open the repository owner's **Packages** page.
2. Select **mhgurecomp-builder**.
3. Open **Package settings**.
4. Under **Danger Zone**, select **Change visibility**.
5. Select **Public** and confirm the change.

Public visibility permits anonymous downloads and exposes the image contents.
Review the package before this change.
Once a package is public, you cannot make it private again.
For organization packages, the organization policy can restrict visibility changes.
See [package access and visibility](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility).

Verify an anonymous pull with an empty temporary Docker configuration:

```bash
OWNER=morerokk
read -r -p "Full source SHA from the upstream publication: " REVISION
IMAGE="ghcr.io/${OWNER,,}/mhgurecomp-builder:$REVISION"
ANONYMOUS_CONFIG=$(mktemp -d)
docker --config "$ANONYMOUS_CONFIG" --host unix:///var/run/docker.sock pull "$IMAGE"
rm -r "$ANONYMOUS_CONFIG"
```

This example uses the standard local Docker socket.
For rootless Docker, replace the socket with your configured local socket.
For a fork, replace `OWNER` with that fork's owner.
Do not publish a game dump, generated game C, or playable packages.

## Image reproducibility

APT package manifests and hashes remain under `/opt/mhgu/` in the image.
Ubuntu repositories can change, so rebuilds are not necessarily byte-identical.
