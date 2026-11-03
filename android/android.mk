# The Android build's settings (Makefile, PLATFORM=android; the APK itself: android/apk.mk). docs/research/android.md.
#
#   make PLATFORM=android ABI=x86_64      (the emulator)        make PLATFORM=android ABI=arm64-v8a      (phones)
#
# Each ABI builds its libraries into build-android/lib/<abi>/ (objects in obj-android/<abi>/), and the APK
# (build-android/mhgurecomp.apk) holds every ABI built so far:
#   libSDL3.so      SDL 3 (its Java activity in the APK starts the game)
#   libmain.so      the runtime (SDL_main), with shaderc and FFmpeg linked in
#   libmhgugame.so  the recompiled game code, linked against libmain.so for the runtime's functions
#
# Needs the Android SDK (ANDROID_HOME, default /c/Android/Sdk) with ndk, cmake, build-tools and platforms/android-35,
# and a JDK 17 or newer (JAVA_HOME, default /c/Android/jdk-21).
to_unix   = $(if $(1),$(shell cygpath -u '$(1)' 2>/dev/null || echo '$(1)'))
ANDROID_SDK ?= $(or $(call to_unix,$(ANDROID_HOME)),/c/Android/Sdk)
NDK       ?= $(lastword $(sort $(wildcard $(ANDROID_SDK)/ndk/*)))
JDK       ?= $(or $(call to_unix,$(JAVA_HOME)),/c/Android/jdk-21)
API       := 29
TRIPLE    := $(if $(filter x86_64,$(ABI)),x86_64,aarch64)-linux-android$(API)
LLVM      := $(firstword $(wildcard $(NDK)/toolchains/llvm/prebuilt/*/bin))
ifeq ($(LLVM),)
$(error no Android NDK in $(ANDROID_SDK)/ndk: install it (android sdk install ndk/<version>) or set NDK=)
endif
# (-fPIC: everything here goes into shared libraries; the NDK's default is -fPIE, for executables)
CC        := $(LLVM)/clang --target=$(TRIPLE) -fPIC
CXX       := $(LLVM)/clang++ --target=$(TRIPLE) -fPIC
# 16 KB pages (newer phones): every library's segments aligned for them
PAGE16    := -Wl,-z,max-page-size=16384

EXE       := $(B)/lib/$(ABI)/libmain.so
GAMELIB   := $(B)/lib/$(ABI)/libmhgugame.so
UPDATER   :=
OSFLAGS   :=
GENFLAGS  += -fPIC
RT_SRC    += runtime/platform/linux.c
# (the main Makefile's GLIBC_CHECK is Linux's; nothing to check against bionic)

# SDL3 as a shared library, as SDL's Java activity expects (System.loadLibrary("SDL3")), from the same release as the
# Linux build's; shaderc static, as on Linux. Both with the NDK's CMake toolchain (the SDK's cmake and ninja).
CMAKE_DIR := $(lastword $(sort $(wildcard $(ANDROID_SDK)/cmake/*)))
win_path   = $(shell cygpath -m '$(1)' 2>/dev/null || echo '$(1)')
CMAKE_BUILD = cd $(1) && MAKEFLAGS= "$(CMAKE_DIR)/bin/cmake" -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
                  -DCMAKE_MAKE_PROGRAM="$(call win_path,$(CMAKE_DIR)/bin/ninja)" \
                  -DCMAKE_TOOLCHAIN_FILE="$(call win_path,$(NDK)/build/cmake/android.toolchain.cmake)" \
                  -DANDROID_ABI=$(ABI) -DANDROID_PLATFORM=android-$(API) -DANDROID_STL=c++_static \
                  -DCMAKE_SHARED_LINKER_FLAGS="$(PAGE16)" -DCMAKE_POSITION_INDEPENDENT_CODE=ON $(2) > configure.log 2>&1 \
                  || { tail -20 configure.log; exit 1; }; \
              MAKEFLAGS= "$(CMAKE_DIR)/bin/cmake" --build build --parallel $$(nproc) $(3) > build.log 2>&1 || { tail -30 build.log; exit 1; }
SDL3      := bundled
SDL3_CONF := -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
SDL3_NEED :=
SDL_DEPS  := $(SDL3_DIR)/lib/libSDL3.so
SDL_LIBS  := -L$(SDL3_DIR)/lib -lSDL3
$(SDL3_DIR)/lib/libSDL3.so: $(SDL3_DIR)/.built ;
LIB_INC   := -I$(SDL3_DIR)/include -I$(SHADERC_DIR)/include

# FFmpeg cross-compiled with the NDK's clang (its configure runs here, its test programs aren't run). No assembly: its
# configure checks the arm64 assembler without --target (so the NDK's clang fails it), and plain C decodes the game's
# 960x540 movies easily.
FFMPEG_CC    = $(LLVM)/clang
FFMPEG_CONF += --enable-cross-compile --target-os=android --arch=$(if $(filter x86_64,$(ABI)),x86_64,aarch64) \
               --extra-cflags=--target=$(TRIPLE) --extra-ldflags=--target=$(TRIPLE) --cxx=$(LLVM)/clang++ \
               --ar=$(LLVM)/llvm-ar --nm=$(LLVM)/llvm-nm --ranlib=$(LLVM)/llvm-ranlib --strip=$(LLVM)/llvm-strip \
               --host-cc=clang --enable-pic --disable-asm

# libmain.so: SDL calls its SDL_main. (--exclude-libs: shaderc and FFmpeg stay inside, as on Linux; libc++ static.)
# (--no-undefined: a function newer than Android $(API) fails here, not on the player's device when the game starts)
LDFLAGS   := -shared -Wl,-soname,libmain.so -Wl,--no-undefined $(PAGE16) -Wl,--exclude-libs,ALL $(SDL_LIBS) $(AV_LIBS) -lvulkan $(SHADERC_LIBS) \
             -landroid -llog -ldl -lm -static-libstdc++
GAMELIB_LDFLAGS := -shared -Wl,-soname,libmhgugame.so -Wl,--no-undefined $(PAGE16) -L$(B)/lib/$(ABI) -lmain
