# MHGU Recomp build
# Windows: MSYS2 UCRT64 (clang, lld, make; see README.md). Linux: clang. macOS: Xcode's clang + make macos-deps.
#
#   make gen GAME=<folder with exefs/ and romfs/>   recompile the game code from your own dump into generated/
#   make -j4                                        build build/mhgurecomp(.exe) and the game code library, with its
#                                                   DLLs and the game files (build/data): start build/mhgurecomp.exe
# build/ holds only the game folder (what a player copies); the compiler's files go to obj/.
UNAME := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Darwin)
PLATFORM := darwin
else
PLATFORM := linux
endif

GAME     ?= ../mhgu/game
GEN      ?= generated
B        ?= build
O        ?= obj
CC       := clang
CXX      := clang++
ifeq ($(PLATFORM)-$(shell uname -m),darwin-arm64)
GENARCH  ?= -mcpu=apple-m1    # Apple Silicon
else
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA (Haswell or newer)
endif
GENFLAGS := -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type -Wno-unused-label \
            -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden -Iruntime $(GENARCH)

GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(O)/gen/%.o,$(GEN_SRC)) $(O)/gen/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/net/*.c) \
            runtime/nvn/objects.c runtime/nvn/cmdbuf.c runtime/nvn/nvn_args.c runtime/nvn/tegra.c runtime/nvn/formats.c \
            runtime/nvn/backend_null.c runtime/nvn/astc.c runtime/nvn/maxwell/translate.c $(wildcard runtime/nvn/vulkan/*.c) \
            runtime/platform/$(PLATFORM).c
# (simd_int.c / neon.c, helpers of the recompiled code, live in the executable: the game code library imports them,
# and runtime/interp.c uses them too)
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/net/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) $(wildcard runtime/nvn/*/*.h)

# FFmpeg (the game's movies, runtime/hle/movie.c): by default a small static build of a pinned FFmpeg release, made
# here from its source (downloaded once to third_party/ffmpeg/, checked against FFMPEG_SHA256) with only what the movies
# use: MP4 holding H.264 video and AAC audio. Nothing to install or ship (Linux distributions carry different FFmpeg
# versions, and an executable linked to one doesn't start with another), and no GPL parts: FFmpeg's LGPL 2.1 only.
# FFMPEG=system links the system's own avformat / avcodec / avutil instead.
# (macOS: the one tools/macos_deps.sh builds into $(DEPS), shipped in the app like SDL3 and shaderc)
ifeq ($(PLATFORM),darwin)
FFMPEG        ?= system
endif
FFMPEG        ?= bundled
FFMPEG_VER    := 9.0.2
FFMPEG_SHA256 := 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_TAR    := third_party/ffmpeg/ffmpeg-$(FFMPEG_VER).tar.xz
FFMPEG_DIR    := $(O)/ffmpeg-$(FFMPEG_VER)
FFMPEG_LIBS   := $(addprefix $(FFMPEG_DIR)/lib/,libavformat.a libavcodec.a libavutil.a)
FFMPEG_CONF   := --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
                 --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
                 --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
                 --enable-parser=h264,aac --enable-decoder=h264,aac
# (nasm: FFmpeg's assembly for x86; without it the plain C versions, fast enough for 960x540 movies)
FFMPEG_CONF   += $(if $(shell command -v nasm 2>/dev/null),,--disable-x86asm)
ifeq ($(FFMPEG),system)
AV_LIBS  := -lavformat -lavcodec -lavutil
AV_DEPS  :=
else
AV_DEPS  := $(FFMPEG_LIBS)
AV_LIBS  :=
endif

# SDL3 and shaderc (Linux): by default static builds of pinned releases, made here from their sources like FFmpeg
# (downloaded once to third_party/, checked against their SHA-256), the versions MSYS2 has for the Windows build. Not
# every distribution has them (Ubuntu 24.04 / Linux Mint 22: no SDL3), and a distribution's own can need a newer glibc
# than GLIBC_MAX. SDL still loads X11, Wayland, libdecor, PipeWire / PulseAudio / ALSA, udev... from the system when
# it runs, as its shared library does. Needs cmake. SDL3=system / SHADERC=system link the system's libSDL3.so.0 /
# libshaderc_shared.so.1 instead. (Windows: always MSYS2's, their DLLs bundled. macOS: the ones tools/macos_deps.sh
# builds into $(DEPS), shipped in the app.)
SDL3          ?= $(if $(filter windows darwin,$(PLATFORM)),system,bundled)
SHADERC       ?= $(if $(filter windows darwin,$(PLATFORM)),system,bundled)
SDL3_VER      := 3.4.18
SDL3_SHA256   := 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
SDL3_TAR      := third_party/sdl3/SDL3-$(SDL3_VER).tar.gz
SDL3_DIR      := $(O)/sdl3-$(SDL3_VER)
SDL3_CONF     := -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
# (what a player's desktop needs: the configure must find these, or SDL would quietly build without them)
SDL3_NEED     := X11 WAYLAND WAYLAND_LIBDECOR PIPEWIRE PULSEAUDIO ALSA HIDAPI LIBUDEV DBUS
# shaderc with the glslang and SPIRV-Tools / SPIRV-Headers releases it was released with, in one static library
SHADERC_VER   := 2026.4
GLSLANG_VER   := 16.6.0
SPIRV_VER     := 1.4.363.0
SHADERC_PARTS := shaderc-$(SHADERC_VER) glslang-$(GLSLANG_VER) SPIRV-Tools-$(SPIRV_VER) SPIRV-Headers-$(SPIRV_VER)
SHADERC_TARS  := $(patsubst %,third_party/shaderc/%.tar.gz,$(SHADERC_PARTS))
SHADERC_DIR   := $(O)/shaderc-$(SHADERC_VER)
SHADERC_CONF  := -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
                 -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
                 -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF
# (MAKEFLAGS=: cmake's own make runs on its own, not in this one's jobs)
CMAKE_BUILD   = cd $(1) && MAKEFLAGS= cmake -S src -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$(CC) \
                    -DCMAKE_CXX_COMPILER=$(CXX) -DCMAKE_POSITION_INDEPENDENT_CODE=ON $(2) > configure.log 2>&1 \
                    || { tail -20 configure.log; exit 1; }; \
                MAKEFLAGS= cmake --build build --parallel $$(nproc) $(3) > build.log 2>&1 || { tail -30 build.log; exit 1; }
ifeq ($(SDL3),system)
SDL_DEPS :=
SDL_LIBS := -lSDL3
else
SDL_DEPS := $(SDL3_DIR)/lib/libSDL3.a
SDL_LIBS := $(SDL_DEPS) -pthread -lm
endif
ifeq ($(SHADERC),system)
SHADERC_DEPS :=
SHADERC_LIBS := -lshaderc_shared
else
SHADERC_DEPS := $(SHADERC_DIR)/lib/libshaderc_combined.a
SHADERC_LIBS := $(SHADERC_DEPS)
endif
# the libraries' headers, for everything compiled here (the stamps: built before anything includes them)
LIB_INC    := $(if $(SDL_DEPS),-I$(SDL3_DIR)/include -DMHGU_SDL_BUNDLED) $(if $(SHADERC_DEPS),-I$(SHADERC_DIR)/include)
LIB_STAMPS := $(if $(SDL_DEPS),$(SDL3_DIR)/.built) $(if $(SHADERC_DEPS),$(SHADERC_DIR)/.built)

ifeq ($(PLATFORM),windows)
EXE      := $(B)/mhgurecomp.exe
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS
GENFLAGS += -D_FILE_OFFSET_BITS=64
# (the GCC runtime - libstdc++, libgcc, winpthreads - linked in: three DLLs fewer to ship, and their licenses ask no
# more than their notices then; build/licenses has those)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(SDL_LIBS) $(AV_LIBS) -lvulkan-1 $(SHADERC_LIBS) -lwinmm -lversion -lole32 -lshell32 -luuid -lws2_32 \
            -lbcrypt -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic \
            -Wl,--export-all-symbols -Wl,--out-implib,$(O)/mhgurecomp.dll.a
GAMELIB  := $(B)/mhgugame.dll
GAMELIB_LDFLAGS := -shared -fuse-ld=lld -static-libgcc $(O)/mhgurecomp.dll.a
UPDATER  := $(B)/mhgurecomp-updater.exe
# (a window program; zlib linked in, WinHTTP is Windows')
UPD_LDFLAGS := -fuse-ld=lld -mwindows $(SDL_LIBS) -lwinhttp -Wl,-Bstatic -lz -Wl,-Bdynamic -static-libgcc
else ifeq ($(PLATFORM),darwin)
# Built for macOS $(MACOS_MIN) and newer, against SDL3, ffmpeg, shaderc and the Vulkan loader that tools/macos_deps.sh
# built for it in $(DEPS) (make macos-deps; Homebrew's load only on the macOS they were installed on). From Homebrew:
# vulkan-headers and molten-vk (README.md); VMA in third_party/vk_mem_alloc
BREW     ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
DEPS     ?= third_party/macos-deps
MACOS_MIN ?= 13.0
ifeq ($(wildcard $(DEPS)/lib/libSDL3.dylib),)
ifeq ($(filter macos-deps gen clean test-lift,$(MAKECMDGOALS)),)
$(error no libraries in $(DEPS): make macos-deps first (README.md, macOS))
endif
endif
EXE      := $(B)/mhgurecomp
MINFLAG  := -mmacosx-version-min=$(MACOS_MIN)
OSFLAGS  := -I$(DEPS)/include -I$(BREW)/opt/vulkan-headers/include $(MINFLAG)
GENFLAGS += -fPIC $(MINFLAG)
# (the rpath: build/mhgurecomp runs from there; make_app replaces it with the app's Frameworks folder)
LDFLAGS  := -L$(DEPS)/lib -Wl,-rpath,$(abspath $(DEPS)/lib) $(MINFLAG) -rdynamic -lSDL3 $(AV_LIBS) \
            -lvulkan -lshaderc_shared -lpthread -lm -framework Cocoa
# (the Settings menu and window; building the game code on a player's Mac)
RT_OBJC  := runtime/platform/darwin_menu.m runtime/platform/darwin_settings.m runtime/platform/darwin_gamecode.m
GAMELIB  := $(B)/mhgugame.dylib
# (a Mach-O bundle: the game code's imports of runtime functions resolve against the executable at link time)
GAMELIB_LDFLAGS := -bundle -bundle_loader $(EXE) $(MINFLAG)
# (make test-settings / test-settings-panel: SDL from $(DEPS))
TEST_LDFLAGS := -L$(DEPS)/lib -Wl,-rpath,$(abspath $(DEPS)/lib)
else
EXE      := $(B)/mhgurecomp
OSFLAGS  :=
GENFLAGS += -fPIC
RT_SRC   += runtime/platform/linux_glibc.c $(if $(SDL_DEPS),runtime/platform/linux_sdl.c)
FFMPEG_CONF += --enable-pic
# (--exclude-libs: the static libraries linked in - SDL3, shaderc with glslang, FFmpeg - stay inside; only the runtime's
# own functions are exported, for the game code library. A Vulkan layer or overlay with its own glslang or SDL then
# doesn't bind to ours, as happened with ImGui: see IMGUIFLAGS.)
LDFLAGS  := -fuse-ld=lld -rdynamic -Wl,--exclude-libs,ALL $(SDL_LIBS) $(AV_LIBS) -lvulkan $(SHADERC_LIBS) -lpthread -ldl -lm
# The programs start with glibc GLIBC_MAX or later (Ubuntu 24.04, Linux Mint 22, Debian 13, SteamOS): calls to the
# functions newer glibc versions gave a new symbol version go to the old one (runtime/platform/linux_glibc.c), and the
# link checks that nothing newer is left.
GLIBC_MAX  := 2.39
GLIBC_WRAP := sqrtf acosf asinf atan2f log10f sinh cosh
GLIBC_WRAP_FLAGS := $(foreach f,$(GLIBC_WRAP),-Wl,--wrap=$(f))
LDFLAGS  += $(GLIBC_WRAP_FLAGS)
GAMELIB  := $(B)/mhgugame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld
UPDATER  := $(B)/mhgurecomp-updater
# (libcurl: opened at run time, updater/http.c)
UPD_LDFLAGS := -fuse-ld=lld $(SDL_LIBS) -lz -ldl $(GLIBC_WRAP_FLAGS)
endif

# GAMELIB_ID: what the game code library is compiled against. A library with another id is refused at startup.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c \
                runtime/hle/hle_symbols.txt runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)
# VERSION: this build's release (git describe: 0.10.3 on a release's tag, 0.10.3-4-gabc1234 after it), in the log and
# the updater's (updater/updater.c: only a build of exactly a release updates itself). make dist VERSION=0.10.4 for a
# release built before its tag is made.
VERSION ?= $(shell git describe --tags 2>/dev/null || echo unknown)

RT_OBJ   = $(patsubst runtime/%.c,$(O)/rt/%.o,$(RT_SRC)) $(patsubst runtime/%.m,$(O)/rt/%.o,$(RT_OBJC)) $(RT_CXX) \
           $(O)/rt/overlay.o $(O)/rt/settings_panel.o $(IMGUI_OBJ) $(O)/updater/sha256.o
# (updater/sha256.c: runtime/net/ldn.c hashes the matchmaking key with it)
# Dear ImGui with cimgui's C API (third_party/cimgui: Forge plugins' ImGui, runtime/forge_imgui.c, overlay.cpp)
IMGUI    := third_party/cimgui
IMGUI_SRC := $(IMGUI)/cimgui.cpp $(addprefix $(IMGUI)/imgui/,imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp)
IMGUI_SRC += $(if $(filter 1,$(NULL_GPU)),,$(IMGUI)/imgui/backends/imgui_impl_vulkan.cpp)
IMGUI_OBJ := $(patsubst $(IMGUI)/%.cpp,$(O)/imgui/%.o,$(IMGUI_SRC))
# (-fvisibility=hidden: without it the executable exports Dear ImGui's C++ symbols, and an overlay that links the
# system ImGui (MangoHud -> libimgui.so.1) binds its HUD's calls into ours: its 1.91 ImFontAtlas handed to our 1.92
# code crashes at swap-chain creation. Our own code and the Forge plugins' ImGui thunks bind inside the executable.)
IMGUIFLAGS := -std=gnu++17 -O2 -g -fvisibility=hidden -DCIMGUI_NO_EXPORT -I$(IMGUI) -I$(IMGUI)/imgui $(LIB_INC)
# (vendored VulkanMemoryAllocator fallback: -I on a missing dir is ignored, so MSYS2 builds
# with the system vulkan-memory-allocator package are unaffected)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Ithird_party/vk_mem_alloc -I$(IMGUI) $(OSFLAGS) $(LIB_INC)
# FSR 3.1 temporal upscaler (runtime/nvn/vulkan/fsr.c): third_party/fsr-vulkan = AMD FidelityFX SDK 1.1.4's FSR 3.1.4
# upscaler (host scheduler, Vulkan backend, its GLSL) + a portable C wrapper, all MIT. No AMD binaries: the shaders are
# compiled with shaderc from the embedded GLSL when first used (src/ffx_fsr3_shaderblobs.cpp), linked into the executable.
FSR      := third_party/fsr-vulkan
FFX      := $(FSR)/upstream/ffx-1.1.4/sdk
FSR_SRC  := $(FSR)/src/ffx_vk_portable.c $(FSR)/src/ffx_vk_fsr3_portable.cpp $(FSR)/src/ffx_fsr3_shaderblobs.cpp \
            $(FSR)/src/ffx_fsr3_glsl.c \
            $(addprefix $(FFX)/src/,shared/ffx_assert.cpp shared/ffx_message.cpp shared/ffx_object_management.cpp \
              shared/ffx_breadcrumbs_list.cpp components/fsr3upscaler/ffx_fsr3upscaler.cpp backends/vk/ffx_vk.cpp \
              backends/shared/ffx_shader_blobs.cpp)
FSR_OBJ  := $(patsubst $(FSR)/%,$(O)/fsr/%.o,$(FSR_SRC))
FSRFLAGS := -O2 -w -DFFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1 -DFFX_FSR3UPSCALER=1 -DDISABLE_FFX_DEBUG_BREAK=1 $(OSFLAGS) $(LIB_INC) \
            -I$(FSR)/include -I$(FFX)/include -I$(FFX)/src/shared -I$(FFX)/src/components -I$(FFX)/src/backends/shared \
            -I$(FFX)/src/backends/shared/blob_accessors
# the GLSL embedded by ffx_fsr3_glsl.c (assembler .incbin, paths relative to the directory make runs in)
FFX_GPU  := $(FFX)/include/FidelityFX/gpu
FFX_PASS := $(FFX)/src/backends/vk/shaders/fsr3upscaler
$(O)/fsr/src/ffx_fsr3_glsl.c.o: FSRFLAGS += -DFFX_GPU_DIR='"$(FFX_GPU)"' -DFFX_PASS_DIR='"$(FFX_PASS)"'
$(O)/fsr/src/ffx_fsr3_glsl.c.o: $(wildcard $(FFX_GPU)/*.h $(FFX_GPU)/*/*.h $(FFX_PASS)/*.glsl)
RTFLAGS  += -I$(FSR)/include -Ithird_party/ffx-api
# FSR4_DRIVER_PATCH=1 (opt-in): FSR 4 can use the AMD driver's own amdvkffx64.dll directly, which needs one in-memory patch of
# that DLL's GetModuleHandleW import (runtime/nvn/vulkan/fsr4.c, docs/FSR4-PORT.md). The default, FSR4_DRIVER_PATCH=0, leaves
# all of that code out: nothing of AMD's is touched, and FSR 4 is only used through a loader DLL the player supplies (else FSR 3.1).
FSR4_DRIVER_PATCH ?= 0
.DEFAULT_GOAL := all
FSR4_STAMP := $(O)/fsr4_patch_$(FSR4_DRIVER_PATCH).stamp
$(FSR4_STAMP):
	@mkdir -p $(O) && rm -f $(O)/fsr4_patch_*.stamp && touch $@
$(O)/rt/nvn/vulkan/fsr4.o: $(FSR4_STAMP)
$(O)/rt/nvn/vulkan/fsr4.o: RTFLAGS += -DFSR4_DRIVER_PATCH=$(FSR4_DRIVER_PATCH)
# NULL_GPU=1: no renderer (runtime/nvn/backend_null.c: the game runs, nothing is drawn)
ifeq ($(NULL_GPU),1)
RTFLAGS  += -DMHGU_NULL_BACKEND
RT_SRC   := $(filter-out runtime/nvn/vulkan/%,$(RT_SRC))
RT_CXX   :=
else
RT_CXX   := $(O)/rt/nvn/vulkan/vma.o $(FSR_OBJ)
endif

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(UPDATER) $(if $(filter windows,$(PLATFORM)),bundle) data licenses pipelist

# Host-only INI/settings regression test: no game dump or generated code required.
.PHONY: test-settings
test-settings: $(LIB_STAMPS)
	@mkdir -p $(O)/settings-test
	$(CC) $(RTFLAGS) tests/test_settings.c runtime/config.c runtime/config_store.c $(TEST_LDFLAGS) $(SDL_LIBS) -lm -o $(O)/settings-test/test-settings$(if $(filter windows,$(PLATFORM)),.exe)
	$(O)/settings-test/test-settings$(if $(filter windows,$(PLATFORM)),.exe) $(O)/settings-test/settings.ini

$(O)/gen/%.o: $(GEN)/%.c runtime/cpu.h runtime/vfp.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@


# the id as a file, rewritten only when it changes: both sides of the library interface rebuild together
$(O)/gamelib.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(GAMELIB_ID)' ] || echo '$(GAMELIB_ID)' > $@

$(O)/gen/tools/gamelib_entry.o: tools/gamelib_entry.c $(O)/gamelib.id
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(O)/rt/gamelib.o: $(O)/gamelib.id
$(O)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
$(O)/version.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(VERSION)' ] || echo '$(VERSION)' > $@
$(O)/rt/main.o: $(O)/version.id
$(O)/rt/main.o: RTFLAGS += -DMHGU_VERSION='"$(VERSION)"'

# SHADER_ID / PIPELINE_ID: what the shader caches go with (runtime/nvn/vulkan/exec.c): cache/programs.bin and
# warmed.bin with the translator, pipelines.bin with the pipeline code too. An update that leaves them alone keeps the
# caches (they were tied to the executable: every build started them over).
SHADER_SRCS := $(wildcard runtime/nvn/maxwell/*.c runtime/nvn/maxwell/*.h) runtime/nvn/vulkan/shaders.c runtime/nvn/vulkan/shaders.h
SHADER_ID   := $(shell cat $(SHADER_SRCS) | tr -d '\r' | sha256sum | cut -c1-16)
PIPELINE_ID := $(shell cat $(SHADER_SRCS) runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/vk.h | tr -d '\r' | sha256sum | cut -c1-16)
$(O)/shader.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(SHADER_ID) $(PIPELINE_ID)' ] || echo '$(SHADER_ID) $(PIPELINE_ID)' > $@
$(O)/rt/nvn/vulkan/exec.o: $(O)/shader.id
$(O)/rt/nvn/vulkan/exec.o: RTFLAGS += -DSHADER_ID='"$(SHADER_ID)"' -DPIPELINE_ID='"$(PIPELINE_ID)"'

$(O)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(O)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

$(O)/rt/forge_imgui.o: runtime/forge_imgui_thunks.inc $(IMGUI)/cimgui.h
$(O)/rt/forge_imgui.o: RTFLAGS += -DCIMGUI_NO_EXPORT -Wno-unused-variable

$(O)/rt/overlay.o: runtime/overlay.cpp runtime/overlay.h runtime/forge.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) $(if $(filter 1,$(NULL_GPU)),-DMHGU_NULL_BACKEND) -Wall -Iruntime $(OSFLAGS) -c $< -o $@

$(O)/rt/settings_panel.o: runtime/settings_panel.cpp runtime/settings_panel.h runtime/modorder.h runtime/config.h runtime/settings_locale.h runtime/settings_icons.h runtime/app_actions.h runtime/overlay.h runtime/net/net.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) -Wall -Iruntime $(OSFLAGS) -c $< -o $@

$(O)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) $(OSFLAGS) -w -c $< -o $@

$(O)/rt/nvn/vulkan/vma.o: runtime/nvn/vulkan/vma.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w -Ithird_party/vk_mem_alloc $(OSFLAGS) -c $< -o $@

$(O)/fsr/%.c.o: $(FSR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(FSRFLAGS) -c $< -o $@

$(O)/fsr/%.cpp.o: $(FSR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 $(FSRFLAGS) -include $(FSR)/src/$(if $(filter windows,$(PLATFORM)),ffx_mingw_portability.hpp,ffx_1_1_4_portability.hpp) -c $< -o $@

$(EXE): $(RT_OBJ) $(AV_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS)
	$(GLIBC_CHECK)

# The updater (updater/updater.c): started in place of the game, it installs the latest release and starts the game.
UPD_OBJ  := $(patsubst updater/%.c,$(O)/updater/%.o,$(wildcard updater/*.c))
# (the glibc wrappers and the newer SDL, as the game's)
UPD_OBJ  += $(if $(filter windows,$(PLATFORM)),,$(O)/rt/platform/linux_glibc.o $(if $(SDL_DEPS),$(O)/rt/platform/linux_sdl.o))
$(O)/updater/%.o: updater/%.c updater/updater.h $(O)/version.id
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall $(OSFLAGS) $(LIB_INC) -DMHGU_VERSION='"$(VERSION)"' -c $< -o $@
# (SDL3 / shaderc: their headers before anything is compiled, their archives in LDFLAGS / UPD_LDFLAGS)
$(RT_OBJ) $(UPD_OBJ): $(LIB_STAMPS)
$(UPDATER): $(UPD_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $^ $(UPD_LDFLAGS)
	$(GLIBC_CHECK)

# (Linux: a program starts with glibc GLIBC_MAX and libstdc++ GLIBCXX_MAX or later, see LDFLAGS; this checks the one
# just linked. GLIBCXX_MAX: GCC 13's, the oldest of those distributions.)
GLIBCXX_MAX := 3.4.32
ifeq ($(PLATFORM),linux)
define GLIBC_CHECK
@new=$$(objdump -T $@ | awk -v max=$(GLIBC_MAX) -v cxx=$(GLIBCXX_MAX) ' \
	  match($$0, /GLIBC_[0-9.]+/) { split(substr($$0, RSTART + 6, RLENGTH - 6), v, "."); split(max, m, "."); \
	    if (v[1] * 1000 + v[2] > m[1] * 1000 + m[2]) print "  " $$NF " (glibc " v[1] "." v[2] ")" } \
	  match($$0, /GLIBCXX_[0-9.]+/) { split(substr($$0, RSTART + 8, RLENGTH - 8), v, "."); split(cxx, m, "."); \
	    if (v[3] > m[3]) print "  " $$NF " (libstdc++ GLIBCXX_" v[1] "." v[2] "." v[3] ")" }'); \
	if [ -n "$$new" ]; then printf '%s needs a newer glibc / libstdc++ than $(GLIBC_MAX) / $(GLIBCXX_MAX) for:\n%s\n%s\n' \
	  $@ "$$new" "(glibc: add them to GLIBC_WRAP and runtime/platform/linux_glibc.c)"; rm -f $@; exit 1; fi
endef
endif

# the bundled libraries' sources (see FFMPEG, SDL3 and SHADERC above): downloaded once, checked against their SHA-256
# FETCH(file, url, sha256)
define FETCH
$(1):
	@mkdir -p $$(dir $$@)
	curl -fL --retry 3 -o $$@.part $(2)
	echo "$(3)  $$@.part" | sha256sum -c -
	mv -f $$@.part $$@
endef
$(eval $(call FETCH,$(FFMPEG_TAR),https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VER).tar.xz,$(FFMPEG_SHA256)))
$(eval $(call FETCH,$(SDL3_TAR),https://github.com/libsdl-org/SDL/releases/download/release-$(SDL3_VER)/SDL3-$(SDL3_VER).tar.gz,$(SDL3_SHA256)))
$(eval $(call FETCH,third_party/shaderc/shaderc-$(SHADERC_VER).tar.gz,https://github.com/google/shaderc/archive/refs/tags/v$(SHADERC_VER).tar.gz,f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947))
$(eval $(call FETCH,third_party/shaderc/glslang-$(GLSLANG_VER).tar.gz,https://github.com/KhronosGroup/glslang/archive/refs/tags/$(GLSLANG_VER).tar.gz,9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2))
$(eval $(call FETCH,third_party/shaderc/SPIRV-Tools-$(SPIRV_VER).tar.gz,https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-$(SPIRV_VER).tar.gz,e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688))
$(eval $(call FETCH,third_party/shaderc/SPIRV-Headers-$(SPIRV_VER).tar.gz,https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-$(SPIRV_VER).tar.gz,5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9))

# the bundled FFmpeg: configure, build into $(FFMPEG_DIR)
$(FFMPEG_DIR)/.built: $(FFMPEG_TAR)
	rm -rf $(FFMPEG_DIR) && mkdir -p $(FFMPEG_DIR)/src
	tar -xJf $< -C $(FFMPEG_DIR)/src --strip-components=1
	cd $(FFMPEG_DIR)/src && ./configure --prefix="$(abspath $(FFMPEG_DIR))" --cc=$(CC) $(FFMPEG_CONF) > ../configure.log \
	    || { tail -20 ../configure.log; exit 1; }
	$(MAKE) -C $(FFMPEG_DIR)/src install > $(FFMPEG_DIR)/build.log || { tail -30 $(FFMPEG_DIR)/build.log; exit 1; }
	touch $@
	@echo "ffmpeg: FFmpeg $(FFMPEG_VER) (H.264 + AAC in MP4) built in $(FFMPEG_DIR)"

$(FFMPEG_LIBS): $(FFMPEG_DIR)/.built ;

# the bundled SDL3: configure (checking that it found SDL3_NEED), build, install into $(SDL3_DIR)
$(SDL3_DIR)/.built: $(SDL3_TAR)
	rm -rf $(SDL3_DIR) && mkdir -p $(SDL3_DIR)/src
	tar -xzf $< -C $(SDL3_DIR)/src --strip-components=1
	$(call CMAKE_BUILD,$(SDL3_DIR),-DCMAKE_INSTALL_PREFIX="$(abspath $(SDL3_DIR))" $(SDL3_CONF),--target install)
	@cd $(SDL3_DIR) && for n in $(SDL3_NEED); do grep -Eq "^-- +SDL_$${n} +\(Wanted: ON\): ON$$" configure.log || \
	    { echo "sdl3: SDL $(SDL3_VER) was configured without SDL_$$n: install its development files (README.md, Linux)"; exit 1; }; done
	touch $@
	@echo "sdl3: SDL $(SDL3_VER) built in $(SDL3_DIR)"

$(SDL3_DIR)/lib/libSDL3.a: $(SDL3_DIR)/.built ;

# the bundled shaderc: its release with glslang, SPIRV-Tools and SPIRV-Headers in its third_party/, built as one static
# library (shaderc_combined) into $(SHADERC_DIR)/lib, its header into $(SHADERC_DIR)/include
$(SHADERC_DIR)/.built: $(SHADERC_TARS)
	rm -rf $(SHADERC_DIR) && mkdir -p $(SHADERC_DIR)/src/third_party/glslang $(SHADERC_DIR)/src/third_party/spirv-tools \
	    $(SHADERC_DIR)/src/third_party/spirv-headers $(SHADERC_DIR)/lib $(SHADERC_DIR)/include
	tar -xzf $(word 1,$^) -C $(SHADERC_DIR)/src --strip-components=1
	tar -xzf $(word 2,$^) -C $(SHADERC_DIR)/src/third_party/glslang --strip-components=1
	tar -xzf $(word 3,$^) -C $(SHADERC_DIR)/src/third_party/spirv-tools --strip-components=1
	tar -xzf $(word 4,$^) -C $(SHADERC_DIR)/src/third_party/spirv-headers --strip-components=1
	$(call CMAKE_BUILD,$(SHADERC_DIR),$(SHADERC_CONF),--target shaderc_combined)
	cp $(SHADERC_DIR)/build/libshaderc/libshaderc_combined.a $(SHADERC_DIR)/lib/
	cp -r $(SHADERC_DIR)/src/libshaderc/include/shaderc $(SHADERC_DIR)/include/
	touch $@
	@echo "shaderc: shaderc $(SHADERC_VER) (glslang $(GLSLANG_VER), SPIRV-Tools $(SPIRV_VER)) built in $(SHADERC_DIR)"

$(SHADERC_DIR)/lib/libshaderc_combined.a: $(SHADERC_DIR)/.built ;

ifneq ($(FFMPEG),system)
$(O)/rt/hle/movie.o: $(FFMPEG_DIR)/.built
$(O)/rt/hle/movie.o: RTFLAGS += -I$(FFMPEG_DIR)/include
endif

# The licenses a copy of the game folder must carry, in $(B)/licenses: this program's own (LICENSE) and its
# THIRD_PARTY_NOTICES (code compiled in: LAS Recomp, Dear ImGui, cimgui, VulkanMemoryAllocator), FFmpeg's LGPL (in the
# executable), on Linux SDL3's and shaderc's (in the programs), and on Windows those of the MSYS2 packages behind the DLLs bundled and the libraries linked in
# (bundle: tools/bundle_dlls.py, which lists them all with their versions and sources in licenses/README.txt).
# The pipelines recorded in the common areas (extras/pipelines.list: the game's shader code hashes and render state,
# nothing of the game itself), built in the background on a first run: runtime/nvn/vulkan/exec.c "pipeline list"
pipelist: $(B)/pipelines.list
$(B)/pipelines.list: extras/pipelines.list
	@mkdir -p $(dir $@)
	cp $< $@

licenses: $(B)/licenses/LICENSE.txt $(B)/licenses/THIRD_PARTY_NOTICES.txt $(if $(AV_DEPS),$(B)/licenses/FFmpeg.txt) \
          $(if $(SDL_DEPS),$(B)/licenses/SDL3.txt) $(if $(SHADERC_DEPS),$(B)/licenses/shaderc.txt)
$(B)/licenses/%.txt: %
	@mkdir -p $(dir $@)
	cp $< $@
$(B)/licenses/FFmpeg.txt: $(FFMPEG_DIR)/.built
	@mkdir -p $(dir $@)
	@{ echo "This program contains FFmpeg $(FFMPEG_VER) (libavformat, libavcodec, libavutil), linked statically, under the"; \
	   echo "GNU Lesser General Public License 2.1 or later (below). Its source code is the release at"; \
	   echo "https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VER).tar.xz, built with the options in this program's Makefile (FFMPEG_CONF);"; \
	   echo "the program's own source is at https://github.com/morerokk/MHGURecomp, to rebuild it with a changed FFmpeg."; \
	   echo; cat $(FFMPEG_DIR)/src/COPYING.LGPLv2.1; } > $@

$(B)/licenses/SDL3.txt: $(SDL3_DIR)/.built
	@mkdir -p $(dir $@)
	@{ echo "This program contains SDL $(SDL3_VER), linked statically, under the zlib license (below). Its source code is"; \
	   echo "https://github.com/libsdl-org/SDL/releases/download/release-$(SDL3_VER)/SDL3-$(SDL3_VER).tar.gz."; \
	   echo; cat $(SDL3_DIR)/src/LICENSE.txt; } > $@
$(B)/licenses/shaderc.txt: $(SHADERC_DIR)/.built
	@mkdir -p $(dir $@)
	@{ echo "This program contains shaderc $(SHADERC_VER) with glslang $(GLSLANG_VER), SPIRV-Tools $(SPIRV_VER) and SPIRV-Headers"; \
	   echo "$(SPIRV_VER), linked statically, under their licenses (below: shaderc's and SPIRV-Tools' Apache License 2.0,"; \
	   echo "glslang's and SPIRV-Headers' own). Their source code is these releases on GitHub: google/shaderc,"; \
	   echo "KhronosGroup/glslang, KhronosGroup/SPIRV-Tools and KhronosGroup/SPIRV-Headers."; \
	   for f in LICENSE third_party/glslang/LICENSE.txt third_party/spirv-tools/LICENSE third_party/spirv-headers/LICENSE; do \
	     echo; echo "==== $$f"; echo; cat $(SHADERC_DIR)/src/$$f; done; } > $@

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	@mkdir -p $(O)
	$(CC) $(GAMELIB_LDFLAGS) -o $(O)/gamelib.tmp $^
	mv -f $(O)/gamelib.tmp $@

PY := $(firstword $(wildcard tools/venv/Scripts/python.exe) $(wildcard tools/venv/bin/python) python3)

# The build folder is the game folder: mhgurecomp.exe starts from it on its own (runtime/main.c), with
# - bundle (Windows): the MSYS2 DLLs it needs beside it (tools/bundle_dlls.py; not the GPU driver's vulkan-1.dll)
# - data: the game files from $(GAME) in $(B)/data/exefs and $(B)/data/romfs, once (hard links where the file system
#   allows them - no second copy on disk, and deleting them leaves $(GAME) alone - else copies).
# Nothing else: a player copies it as a whole into their game folder (or over it: their settings, caches, mods/ and
# log.txt are written only when the game runs). run-game.bat runs it with those in run/ (MHGU_HOME), not here.
# (--with: MSYS2 packages linked into the executable or into a bundled DLL, whose licenses go along too: the static
# GCC runtime, VulkanMemoryAllocator, and glslang / SPIRV-Tools inside shaderc's DLL)
LICENSE_PKGS := libstdc++ libgcc libwinpthread vulkan-memory-allocator glslang spirv-tools zlib
bundle: $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(UPDATER)
	@$(PY) tools/bundle_dlls.py "$$(cygpath -m "$$(dirname "$$(command -v $(CC))")")" $(B) $^ \
	    --licenses $(B)/licenses --with $(LICENSE_PKGS)

# (macOS: APFS clones, as free as hard links, which BSD cp can't make for a folder)
CPLINK := $(if $(filter darwin,$(PLATFORM)),cp -cR,cp -rl)
data: $(B)/data/.complete
$(B)/data/.complete:
	@if [ ! -f "$(GAME)/exefs/main" ] || [ ! -d "$(GAME)/romfs" ]; then \
	    echo "data: no game files in $(GAME) (make GAME=<folder with exefs/ and romfs/>): $(B)/data left empty"; \
	else \
	    mkdir -p $(B)/data && for d in exefs romfs; do \
	        rm -rf "$(B)/data/$$d.tmp"; \
	        $(CPLINK) "$(GAME)/$$d" "$(B)/data/$$d.tmp" 2>/dev/null || { rm -rf "$(B)/data/$$d.tmp"; \
	            echo "data: no hard links here, copying $$d"; cp -r "$(GAME)/$$d" "$(B)/data/$$d.tmp"; } || exit 1; \
	        rm -rf "$(B)/data/$$d" && mv "$(B)/data/$$d.tmp" "$(B)/data/$$d" || exit 1; \
	    done && touch $@ && echo "data: game files in $(B)/data"; \
	fi

gen:
	cd tools && ../$(PY) gen.py sdk ../$(GAME)/exefs/sdk 0x02400000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt
	cd tools && ../$(PY) gen.py main ../$(GAME)/exefs/main 0x00200000 ../$(GEN)/main

# macOS: a self-contained app of the build (tools/make_app.py): runtime, game code, game files (APFS clones), the
# Homebrew libraries and MoltenVK; it writes to ~/Library/Application Support/MHGURecomp. For you only: never share it.
APP ?= app/MHGURecomp.app
app: all
	python3 tools/make_app.py --libs $(DEPS)/lib $(B) $(GAME) $(APP)

# macOS: the libraries of $(DEPS), built for macOS $(MACOS_MIN) (tools/macos_deps.sh; once, before the first build)
macos-deps:
	sh tools/macos_deps.sh $(DEPS) $(MACOS_MIN)

# macOS release (the zip players download, as upstream's Windows-/ArchLinux-MHGURecomp.<version>.zip):
#   make release VERSION=0.9.11 [BONUS=<folder with the donor save's system file>]
# makes release/MHGURecomp.app of the runtime with no game code or game files in it, zipped as
# release/macOS-MHGURecomp.$(VERSION).zip. Players put their game files in ~/Library/Application Support/MHGURecomp/data
# and the app builds the game code from them on their Mac (tools/build_gamecode.py, with GENFLAGS and GAMELIB_ID).
# Needs no game files or generated code here.
REL_APP := release/MHGURecomp.app
REL_ZIP := release/macOS-MHGURecomp.$(VERSION).zip
release: $(EXE)
	@for l in sdl3 vulkan-loader shaderc ffmpeg; do [ "$$(cat $(DEPS)/.$$l 2>/dev/null)" = "$(MACOS_MIN)" ] || \
	    { echo "release: $$l for macOS $(MACOS_MIN) is not in $(DEPS): make macos-deps first"; exit 1; }; done
	python3 tools/make_app.py --release --version $(VERSION) --min-macos $(MACOS_MIN) --libs $(DEPS)/lib \
	    --gen-flags '$(GENFLAGS)' --gamelib-id $(GAMELIB_ID) $(if $(BONUS),--bonus $(BONUS)) $(B) $(REL_APP)
	rm -f $(REL_ZIP) && ditto -c -k --keepParent $(REL_APP) $(REL_ZIP)
	@echo "release: $(REL_ZIP)"

# differential test of the recompiler against Unicorn (tools/test_lift.py)
test-lift:
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/main 0x00200000 --tests 6000
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/sdk 0x02400000 --tests 6000

runtime-objs: $(RT_OBJ)

clean:  # (keeps $(B)/data, and the settings, caches and saves wherever the game ran)
	rm -rf $(O) $(B)/*.dll $(B)/*.so $(B)/*.dylib $(EXE) $(UPDATER) $(B)/vkprobe.exe

# make dist VERSION=0.10.4: the release archive of the build folder, for GitHub (tools/dist.py): in $(DIST)/,
# Windows-MHGURecomp.<version>.zip or Linux-MHGURecomp.<version>.tar.gz (permissions kept), holding the programs,
# the game code, the DLLs, licenses/, BONUS (the donor save for bonus/system, when given) and the empty folders the
# player fills; never the game files, mods, settings or caches of $(B). It prints each archive's size and SHA-256.
DIST  ?= dist
BONUS ?= $(wildcard $(B)/bonus/system)
dist: all
	@echo '$(VERSION)' | grep -Eq '^[0-9]+(\.[0-9]+)*$$' || \
	  { echo "dist: '$(VERSION)' is not a release version: make dist VERSION=<the release's tag, e.g. 0.10.4>"; exit 1; }
	$(PY) tools/dist.py $(B) $(DIST) $(PLATFORM) $(VERSION) $(if $(BONUS),--bonus "$(BONUS)")

.PHONY: all app macos-deps release gen test-lift runtime-objs clean bundle data licenses pipelist dist FORCE

# Exercise real ImGui navigation without a game dump or graphics device.
.PHONY: test-settings-panel
test-settings-panel: $(O)/rt/settings_panel.o $(O)/rt/modorder.o $(O)/rt/arcmerge.o $(O)/rt/config.o $(O)/rt/config_store.o $(O)/imgui/imgui/imgui.o $(O)/imgui/imgui/imgui_draw.o $(O)/imgui/imgui/imgui_tables.o $(O)/imgui/imgui/imgui_widgets.o $(LIB_STAMPS)
	@mkdir -p $(O)/settings-test
	$(CXX) $(IMGUIFLAGS) -Iruntime $(OSFLAGS) tests/test_settings_panel.cpp $(filter %.o,$^) $(TEST_LDFLAGS) $(SDL_LIBS) -o $(O)/settings-test/test-settings-panel$(if $(filter windows,$(PLATFORM)),.exe)
	$(O)/settings-test/test-settings-panel$(if $(filter windows,$(PLATFORM)),.exe)

# Translation coverage for both host-menu languages; no game files required.
.PHONY: test-settings-locale
test-settings-locale:
	$(PY) tests/test_settings_locale.py
