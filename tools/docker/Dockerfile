# syntax=docker/dockerfile:1
FROM --platform=linux/amd64 ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d

LABEL org.opencontainers.image.title="MHGURecomp Linux builder"

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates clang-18 lld-18 llvm-18 gcc-13 g++-13 make cmake \
    ninja-build nasm pkg-config git util-linux shellcheck \
    python3 python3-venv python3-pip \
    curl tar xz-utils file binutils qemu-user \
    zlib1g-dev libvulkan-dev libegl-dev libwayland-dev wayland-protocols \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
    libdbus-1-dev libdecor-0-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 50 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 50 \
    && update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-18 50 \
    && update-alternatives --install /usr/bin/llvm-ar llvm-ar /usr/bin/llvm-ar-18 50 \
    && update-alternatives --install /usr/bin/llvm-nm llvm-nm /usr/bin/llvm-nm-18 50 \
    && update-alternatives --install /usr/bin/llvm-ranlib llvm-ranlib /usr/bin/llvm-ranlib-18 50 \
    && update-alternatives --install /usr/bin/llvm-strip llvm-strip /usr/bin/llvm-strip-18 50 \
    && update-alternatives --install /usr/bin/llvm-objdump llvm-objdump /usr/bin/llvm-objdump-18 50 \
    && update-alternatives --install /usr/bin/llvm-readelf llvm-readelf /usr/bin/llvm-readelf-18 50

COPY requirements.txt /opt/mhgu/requirements.txt
RUN python3 -m venv /opt/mhgu/venv \
    && /opt/mhgu/venv/bin/python -m pip install --no-cache-dir \
       -r /opt/mhgu/requirements.txt \
    && /opt/mhgu/venv/bin/python -m pip check

COPY docker/prepare-sysroot.sh docker/normalize-sysroot.py /opt/mhgu/docker/
RUN bash /opt/mhgu/docker/prepare-sysroot.sh \
    && mkdir -p /opt/mhgu/include \
    && curl --fail --location --proto '=https' \
       https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h \
       -o /opt/mhgu/include/vk_mem_alloc.h \
    && echo '8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a  /opt/mhgu/include/vk_mem_alloc.h' \
       | sha256sum -c - \
    && dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' \
       > /opt/mhgu/host-packages.tsv

COPY docker/build.sh docker/check-elf.py /opt/mhgu/docker/
ENV PATH="/opt/mhgu/venv/bin:${PATH}" HOME=/tmp
WORKDIR /work/src
ENTRYPOINT ["bash", "/opt/mhgu/docker/build.sh"]
