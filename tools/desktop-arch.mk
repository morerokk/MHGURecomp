# Desktop targets only. Android selects its compiler and CPU through the NDK and ABI.
HOST_ARCH := $(shell uname -m)
TARGET_ARCH ?= $(HOST_ARCH)
normalize_arch = $(if $(filter arm64 aarch64,$(1)),aarch64,$(if $(filter amd64 x86_64,$(1)),x86_64,$(1)))
override TARGET_ARCH := $(call normalize_arch,$(TARGET_ARCH))
HOST_ARCH := $(call normalize_arch,$(HOST_ARCH))
ifeq ($(filter x86_64 aarch64,$(TARGET_ARCH)),)
$(error Unsupported TARGET_ARCH=$(TARGET_ARCH))
endif
ifeq ($(PLATFORM)-$(TARGET_ARCH),windows-aarch64)
$(error ARM64 is supported on Linux only)
endif
SYSROOT ?=
TARGET_FLAGS :=
CMAKE_TARGET_FLAGS :=
FFMPEG_TARGET_FLAGS :=
ifeq ($(PLATFORM)-$(TARGET_ARCH),linux-aarch64)
TARGET_FLAGS += --target=aarch64-linux-gnu
ifneq ($(HOST_ARCH),$(TARGET_ARCH))
CMAKE_TARGET_FLAGS += -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
FFMPEG_TARGET_FLAGS += --enable-cross-compile --arch=aarch64 --target-os=linux --host-cc="$(CC)" \
    --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip --extra-ldflags=-fuse-ld=lld
endif
ifneq ($(SYSROOT),)
TARGET_FLAGS += --sysroot=$(SYSROOT) --gcc-toolchain=$(SYSROOT)/usr
CMAKE_TARGET_FLAGS += -DCMAKE_SYSROOT=$(SYSROOT) -DCMAKE_FIND_ROOT_PATH=$(SYSROOT) \
    -DCMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN=$(SYSROOT)/usr -DCMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN=$(SYSROOT)/usr \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
export PKG_CONFIG_SYSROOT_DIR := $(SYSROOT)
export PKG_CONFIG_LIBDIR := $(SYSROOT)/usr/lib/aarch64-linux-gnu/pkgconfig:$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/share/pkgconfig
unexport PKG_CONFIG_PATH
endif
endif
OBJDUMP ?= $(if $(filter aarch64,$(TARGET_ARCH)),llvm-objdump,objdump)
GENARCH  ?= $(if $(filter aarch64,$(TARGET_ARCH)),-march=armv8-a,-march=x86-64-v3)  # generic release CPUs
