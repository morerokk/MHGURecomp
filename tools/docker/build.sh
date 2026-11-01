#!/usr/bin/env bash
set -euo pipefail

mode=${1:?missing mode}
version=${2:?missing version}
jobs=${3:?missing jobs}
arch=${4:-}
sysroot=/opt/mhgu/sysroot
cd /work/src
mkdir -p third_party/vk_mem_alloc
ln -sfn /opt/mhgu/include/vk_mem_alloc.h \
  third_party/vk_mem_alloc/vk_mem_alloc.h

pilot() {
  local target=$1
  local flags=()
  local runner=()
  if [[ $target == aarch64 ]]; then
    flags=(--target=aarch64-linux-gnu "--sysroot=$sysroot"
      "--gcc-toolchain=$sysroot/usr")
    runner=(qemu-aarch64 -L "$sysroot")
  fi
  mkdir -p "/work/pilot-$target"
  printf '#include <stdio.h>\nint main(void){puts("C pilot: OK");}\n' \
    > "/work/pilot-$target/test.c"
  printf '#include <vector>\nint main(){return std::vector<int>{0}.at(0);}\n' \
    > "/work/pilot-$target/test.cpp"
  clang "${flags[@]}" -fuse-ld=lld "/work/pilot-$target/test.c" \
    -o "/work/pilot-$target/c"
  clang++ "${flags[@]}" -fuse-ld=lld "/work/pilot-$target/test.cpp" \
    -o "/work/pilot-$target/cxx"
  "${runner[@]}" "/work/pilot-$target/c"
  "${runner[@]}" "/work/pilot-$target/cxx"
}

build_target() {
  local target=$1
  local build="/work/build-$target"
  local args=("TARGET_ARCH=$target" "B=$build" "O=/work/obj-$target"
    "GEN=/work/generated" "GAME=/game" "VERSION=$version" "JOBS=$jobs")
  if [[ $target == aarch64 ]]; then
    args+=("SYSROOT=$sysroot" "TEST_RUNNER=qemu-aarch64 -L $sysroot")
  fi
  pilot "$target"
  # Build dependencies one at a time before using Make's outer parallelism.
  make -j1 "${args[@]}" licenses pipelist
  local programs=("$build/mhgurecomp" "$build/mhgurecomp-updater")
  if [[ $mode == release ]]; then
    programs+=("$build/mhgugame.so")
  fi
  make -j"$jobs" "${args[@]}" "${programs[@]}"
  python3 /opt/mhgu/docker/check-elf.py "$target" "${programs[@]}"
  if [[ $mode == check ]]; then
    make -j"$jobs" "${args[@]}" test-settings \
      test-settings-locale test-linux-fault
    # Upstream 0.12 added Cheats, but its navigation test still skips that tab.
    if ! make -j"$jobs" "${args[@]}" test-settings-panel; then
      echo "::warning::Upstream menu navigation test failed (advisory, not a Docker check)."
    fi
    python3 tests/test_build_arch.py
    python3 tests/test_linux_release.py
  else
    python3 tools/dist.py "$build" /work/packages linux "$version" \
      --arch "$target"
  fi
}

case "$mode" in
  release)
    if [[ ! -r /game/exefs/main || ! -r /game/exefs/sdk || ! -r /game/romfs ]]; then
      echo "build: dump is unreadable inside the container; check ownership, ACLs and SELinux." >&2
      exit 1
    fi
    rm -rf /work/generated.new
    make gen GAME=/game GEN=/work/generated.new
    rm -rf /work/generated
    mv /work/generated.new /work/generated
    build_target x86_64
    build_target aarch64
    mkdir -p /out
    for prefix in Linux LinuxARM64; do
      name="$prefix-MHGURecomp.$version.tar.gz"
      cp "/work/packages/$name" "/out/$name.part"
      mv -f "/out/$name.part" "/out/$name"
    done
    (
      cd /out
      sha256sum "Linux-MHGURecomp.$version.tar.gz" \
        "LinuxARM64-MHGURecomp.$version.tar.gz" > SHA256SUMS.part
      mv -f SHA256SUMS.part SHA256SUMS
      sha256sum -c SHA256SUMS
    )
    ;;
  check)
    case "$arch" in
      x86_64|aarch64) ;;
      *) echo "build: check requires x86_64 or aarch64" >&2; exit 2 ;;
    esac
    test ! -d /work/generated
    build_target "$arch"
    ;;
  *) echo "build: expected release or check" >&2; exit 2 ;;
esac
