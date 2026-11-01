#!/usr/bin/env bash
set -euo pipefail

test "$(dpkg --print-architecture)" = amd64
test -r /usr/share/keyrings/ubuntu-archive-keyring.gpg
apt_root=$(mktemp -d)
trap 'rm -rf "$apt_root"' EXIT
mkdir -p "$apt_root/lists/partial" "$apt_root/archives/partial" \
  /opt/mhgu/sysroot
touch "$apt_root/status"
cat > "$apt_root/sources.list" <<'SOURCES'
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble main universe
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-updates main universe
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-security main universe
SOURCES
cat > "$apt_root/config" <<APTCONF
APT::Architecture "arm64";
APT::Architectures { "arm64"; };
APT::Sandbox::User "root";
Dir::Etc::sourcelist "$apt_root/sources.list";
Dir::Etc::sourceparts "-";
Dir::Etc::trustedparts "-";
Dir::State::lists "$apt_root/lists";
Dir::State::status "$apt_root/status";
Dir::Cache::archives "$apt_root/archives";
Acquire::Languages "none";
APTCONF
apt-get -c "$apt_root/config" update
apt-get -c "$apt_root/config" \
  --download-only --no-install-recommends -y install \
  libc6-dev libgcc-13-dev libstdc++-13-dev \
  zlib1g-dev libvulkan-dev libegl-dev libwayland-dev wayland-protocols \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
  libdbus-1-dev libdecor-0-dev libcurl4-openssl-dev

for deb in "$apt_root"/archives/*.deb; do
  arch=$(dpkg-deb -f "$deb" Architecture)
  case "$arch" in
    arm64|all) ;;
    *) echo "sysroot: unexpected package architecture: $arch" >&2; exit 1 ;;
  esac
  # dpkg-deb expands these fields, not the shell.
  # shellcheck disable=SC2016
  dpkg-deb --show --showformat='${Package}\t${Version}\t${Architecture}\n' \
    "$deb"
  dpkg-deb --extract "$deb" /opt/mhgu/sysroot
done | sort > /opt/mhgu/arm64-packages.tsv
(
  cd "$apt_root/archives"
  sha256sum ./*.deb > /opt/mhgu/arm64-packages.sha256
)
cp "$apt_root/sources.list" /opt/mhgu/arm64-sources.list
python3 /opt/mhgu/docker/normalize-sysroot.py /opt/mhgu/sysroot
test -r /opt/mhgu/sysroot/lib/ld-linux-aarch64.so.1
test -r /opt/mhgu/sysroot/usr/lib/aarch64-linux-gnu/crt1.o
test -r /opt/mhgu/sysroot/usr/lib/gcc/aarch64-linux-gnu/13/libstdc++.so
