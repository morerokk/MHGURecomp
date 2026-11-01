#!/usr/bin/env bash
set -euo pipefail
umask 077

die() { echo "linux-release: $*" >&2; exit 1; }
usage() {
  cat <<'HELP'
Usage: bash tools/build-linux-release.sh --game DIR --version VERSION [--jobs N]
       bash tools/build-linux-release.sh --check ARCH --version VERSION [--jobs N]

Build both Linux packages with your own MHGU 1.4.0 dump.
--check accepts x86_64 or aarch64 and does not require game files.
--image NAME selects the builder image. --skip-image-build reuses that image.
CONTAINER_ENGINE defaults to docker. Set it to podman for local Podman.
HELP
}

game=''
version=''
arch=''
jobs=2
mode=release
image=mhgurecomp-builder:local
skip_build=false
while (($#)); do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --skip-image-build) skip_build=true; shift; continue ;;
    --game|--version|--jobs|--image|--check)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --game) game=$2 ;;
        --version) version=$2 ;;
        --jobs) jobs=$2 ;;
        --image) image=$2 ;;
        --check) mode=check; arch=$2 ;;
      esac
      shift 2 ;;
    *) die "unknown option: $1" ;;
  esac
done
[[ $version =~ ^[0-9]+(\.[0-9]+)*$ ]] || die "use a numeric release version"
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer"
[[ -n $image && $image != -* ]] || die "invalid image name"
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] \
  || die "this builder requires an x86-64 Linux host"
if [[ $mode == check ]]; then
  [[ -z $game ]] || die "do not supply game files in check mode"
  [[ $arch == x86_64 || $arch == aarch64 ]] || die "unsupported check architecture"
else
  [[ -n $game ]] || die "supply your dump with --game"
  game=$(realpath -e -- "$game") || die "dump folder does not exist"
  [[ -r $game/exefs/main && -r $game/exefs/sdk && -d $game/romfs ]] \
    || die "dump must contain exefs/main, exefs/sdk and romfs"
  [[ $game != *:* && $game != *$'\n'* ]] || die "dump path contains unsupported characters"
fi
root=$(git rev-parse --show-toplevel) || die "run from the repository"
cd "$root"
git diff --quiet HEAD -- || die "commit or restore tracked changes first"
[[ -z $(git ls-files -u) ]] || die "resolve the merge first"
git cat-file -e HEAD:tools/build-linux-release.sh \
  || die "commit the build tools before using this command"
[[ $root != *:* && $root != *$'\n'* ]] || die "checkout path contains unsupported characters"
revision=$(git rev-parse HEAD)

engine=${CONTAINER_ENGINE:-docker}
command -v "$engine" >/dev/null || die "install $engine or set CONTAINER_ENGINE=podman"
engine_cmd=("$engine")
case "$(basename "$engine")" in
  docker)
    endpoint=
    if [[ -n ${DOCKER_CONTEXT:-} ]]; then
      endpoint=$("$engine" context inspect "$DOCKER_CONTEXT" \
        --format '{{.Endpoints.docker.Host}}') || die "cannot inspect Docker context"
    elif [[ -n ${DOCKER_HOST:-} ]]; then
      endpoint=$DOCKER_HOST
    fi
    if [[ -z $endpoint ]]; then
      endpoint=$("$engine" context inspect \
        --format '{{.Endpoints.docker.Host}}') || die "cannot inspect Docker context"
    fi
    [[ $endpoint == unix://* ]] || die "select a local Docker socket"
    unset DOCKER_HOST DOCKER_CONTEXT
    engine_cmd+=(--host "$endpoint")
    security=$("${engine_cmd[@]}" info --format '{{json .SecurityOptions}}') \
      || die "cannot connect to Docker"
    if [[ $security == *rootless* ]]; then
      user_args=()
    else
      user_args=(--user "$(id -u):$(id -g)")
      for group in $(id -G); do
        [[ $group == "$(id -g)" ]] || user_args+=(--group-add "$group")
      done
    fi
    ;;
  podman)
    [[ -z ${CONTAINER_HOST:-} && -z ${CONTAINER_CONNECTION:-} ]] \
      || die "select local Podman"
    engine_cmd+=(--remote=false)
    rootless=$("${engine_cmd[@]}" info --format '{{.Host.Security.Rootless}}') \
      || die "cannot connect to Podman"
    user_args=(--user "$(id -u):$(id -g)")
    if [[ $rootless == true ]]; then
      user_args+=(--userns=keep-id)
      runtime=$("${engine_cmd[@]}" info --format '{{.Host.OCIRuntime.Name}}')
      [[ $runtime != crun ]] || user_args+=(--group-add keep-groups)
    else
      for group in $(id -G); do
        [[ $group == "$(id -g)" ]] || user_args+=(--group-add "$group")
      done
    fi
    ;;
  *) die "CONTAINER_ENGINE must name docker or podman" ;;
esac

work="$root/.linux-release"
mkdir -p "$work" "$root/dist"
exec 9>"$work/lock"
flock -n 9 || die "another Linux build is active"
if [[ $skip_build == false ]]; then
  "${engine_cmd[@]}" build --platform linux/amd64 \
    --file tools/docker/Dockerfile --tag "$image" tools
fi
image_id=$("${engine_cmd[@]}" image inspect --format '{{.Id}}' "$image") \
  || die "builder image is missing; build or pull it first"
[[ $image_id =~ ^sha256:[0-9a-f]{64}$ ]] || die "invalid builder image ID"
cache="$work/$revision-${image_id#sha256:}-$mode${arch:+-$arch}"
mkdir -p "$cache"
# Keep downloads outside the source export so stale source files cannot survive.
rm -rf "$cache/src"
mkdir -p "$cache/src/third_party"
git archive HEAD | tar -x -C "$cache/src"
for library in ffmpeg sdl3 shaderc; do
  mkdir -p "$cache/downloads/$library"
  ln -s "/work/downloads/$library" "$cache/src/third_party/$library"
done
mounts=(-v "$cache:/work:Z")
if [[ $mode == release ]]; then
  mounts+=(-v "$game:/game:ro,z" -v "$root/dist:/out:Z")
fi
echo "linux-release: sources $revision"
echo "linux-release: builder $image_id"
"${engine_cmd[@]}" run --rm --platform linux/amd64 \
  "${user_args[@]}" "${mounts[@]}" "$image" "$mode" "$version" "$jobs" "$arch"
if [[ $mode == release ]]; then
  echo "linux-release: both packages and SHA256SUMS are in dist/"
fi
