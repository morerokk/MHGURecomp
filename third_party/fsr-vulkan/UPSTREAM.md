# Upstream inventory, provenance, and implementation plan

## Provenance and license boundary

The Vulkan baseline is AMD FidelityFX SDK tag `v1.1.4`, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, released 2025-05-08. Its
vendored source files carry AMD's MIT notice; `upstream/ffx-1.1.4/sdk/LICENSE.txt`
is retained. `upstream/ffx-1.1.4/ORIGINAL_SHA256SUMS` records each pristine
file before the portability changes below, while `CURRENT_SHA256SUMS` records
the complete patched source subset used by the build.

The current algorithm reference is AMD FSR SDK tag `v2.3.0`, commit
`60f4ea81909200d8542eca14dccb2628b763a9a3`, released 2026-06-24. Its relevant
FSR 3 Upscaler 3.1.5, Frame Interpolation 3.1.6, and Optical Flow source files
also carry individual MIT notices. The repository-level 2.3 license is more
restrictive for files without an overriding notice and for signed binaries.
Every imported 2.3 file must therefore be checked individually. Do not import,
redistribute, or derive source from the signed FSR DLLs.

Files outside `upstream/` are original MIT-licensed module work.

## Implemented 1.1.4 host baseline

The current vendored subset is the exact header/source closure required to
compile the FSR 3.1.4 combined host scheduler, FSR 3 Upscaler 3.1.4, Frame
Interpolation 1.1.3, and Optical Flow 1.1.2. The standalone CMake target is
`ffx-vulkan::fsr3-host-1.1.4`. It intentionally contains no GPU backend or
shader blobs itself.

Nine vendored files have narrow Linux/compute-only or correctness changes:

- `ffx_types.h`: opaque context storage is doubled on non-Windows platforms,
  because upstream sizes it for Windows' two-byte `wchar_t` while the private
  context contains many wide-character arrays. Its dynamic-resource ring is
  increased from four to eight effect calls: Frame Interpolation records both
  Prepare and Dispatch per rendered frame, so a two-frame Vulkan host needs a
  longer view-retirement horizon than the stock one-dispatch-per-frame model.
- `ffx_fsr3.cpp`: three wide `swprintf` format specifiers use portable `%ls`.
- `ffx_message.cpp`: includes `ffx_util.h` for the upstream `FFX_UNUSED` macro.
- `ffx_fsr3upscaler.cpp` and `ffx_frameinterpolation.cpp`: use `std::fabs`
  when generating Lanczos weights; on Clang the upstream unqualified
  `abs(float)` binds to integer `abs` and changes the LUT values.
- `ffx_fsr3upscaler_reproject.h`: the FP16 history sample uses `PreviousFrameUpscaleSize()` instead of `UpscaleSize()`, as in
  FSR 3.1.6 (SDK 2.4.0, the only shader difference to 1.1.4 apart from copyright years; it matters when the upscale size changes).
- `ffx_fsr3upscaler_callbacks_glsl.h`: declares the luma-history storage image
  as `rgba16f`, matching the host scheduler's `R16G16B16A16_FLOAT` resource.
  Upstream declares `rgba8`, which Vulkan validation rejects and which makes
  shader writes undefined.
- `ffx_vk.cpp`: leaves `fpSwapChainConfigureFrameGeneration` null because this
  target intentionally excludes the Win32-only swapchain implementation, and
  uses the portability shim's non-deprecated UTF-8/UTF-32 label conversion. It
  also aligns the `alignas(32)` effect-context array within caller scratch
  memory; upstream aligns only the surrounding sizes to four bytes, causing a
  release-build fault on aligned stores. Its memory-requirements2 loader first
  requests the KHR device command and then the Vulkan 1.1 core command, because
  applications using core Vulkan 1.1+ functionality need not enable
  `VK_KHR_get_memory_requirements2`. Dedicated-allocation capability now
  follows the callable logical-device entry point instead of physical-device
  extension availability, avoiding a null call when an extension is supported
  but not enabled. Memory selection similarly treats AMD device-coherent
  memory as an explicit logical-device opt-in rather than inferring it from
  physical extension support. Without that opt-in, all memory types carrying
  `VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD` are excluded (including the
  general allocator, uniform-buffer fallback, and Breadcrumbs path), avoiding
  `VUID-VkMemoryAllocateInfo-memoryTypeIndex-02790`. Requested memory property
  sets must also match in full rather than matching any one requested bit.
  Other optional capabilities are likewise enabled only from application
  metadata describing the logical device/instance. Physical support alone no
  longer enables FP16 permutations, required subgroup-size metadata,
  synchronization2/AMD buffer-marker Breadcrumbs, descriptor indexing, ray
  tracing, or debug-utils labels/names.
- `ffx_frameinterpolation_shaderblobs.cpp` and
  `ffx_opticalflow_shaderblobs.cpp`: select the generated FP16 tables when
  the low-precision permutation is requested. Upstream generated all four
  tables but its accessors ignored `is16bit`, silently selecting the FP32
  variant instead.

  The backend descriptor pool has a single entry for each of sampler, sampled
  image, storage image, uniform buffer, and storage buffer. Upstream declared
  the storage-buffer size but accidentally passed a count that excluded it;
  Frame Interpolation then failed descriptor allocation for a valid storage
  buffer binding. This portable correction removes the duplicate sampler entry
  and derives the count from the array.

`src/ffx_1_1_4_portability.hpp` supplies `_countof`, the two-argument MSVC
`wcscpy_s` array overload, and missing C string declarations without changing
the effect data layout. The target explicitly uses C++17; AMD's public 1.1.4
host headers are not directly C-compilable because they include C++ mutex
headers even though their exported functions have C linkage.

## Implemented 1.1.4 Vulkan upscaler slice

The first compute slice now vendors these MIT-licensed groups at their
upstream-relative paths:

- `sdk/include/FidelityFX/host/backends/vk/ffx_vk.h`;
- `sdk/src/backends/vk/ffx_vk.cpp`;
- `sdk/src/shared/ffx_breadcrumbs_list.{h,cpp}`;
- `sdk/src/backends/shared/ffx_shader_blobs.{h,cpp}`;
- `ffx_fsr3upscaler_shaderblobs.h` in `sdk/src/backends/shared/blob_accessors/` (the accessor behind it is ours, below);
- the 10 `sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl` passes; and
- the GPU headers those passes include (`ffx_core_hlsl.h`, which nothing includes for GLSL, is not kept).

The shaders are not pre-built: `src/ffx_fsr3_shaderblobs.cpp` replaces AMD's `ffx_fsr3upscaler_shaderblobs.cpp` and
compiles the requested pass and permutation from the embedded GLSL (`src/ffx_fsr3_glsl.c`) with shaderc, Vulkan 1.2,
`spirv-opt` at the size level, and the defines of AMD's FidelityFX_SC run: `FFX_GPU=1`, `FFX_GLSL=1`, `FFX_HALF=0|1`, the
four `..._SAMPLERS_USE_DATA_HALF` options (0, 0, 1, 0), `..._UPSAMPLE_USE_LANCZOS_TYPE=2`, and the six permutation options
(`REPROJECT_USE_LANCZOS_TYPE`, `HDR_COLOR_INPUT`, `LOW_RESOLUTION_MOTION_VECTORS`, `JITTERED_MOTION_VECTORS`,
`INVERTED_DEPTH`, `APPLY_SHARPENING`) from the option bits. As in AMD's accessor, only prepare reactivity, shading change,
accumulate and generate reactive have an FP16 build, and wave64 selects the same module as the baseline (AMD's wave64
modules are byte-identical to the baseline ones). The `FfxShaderBlob` reflection (names, bindings, counts, spaces) is
read from the SPIR-V: the descriptor variables the optimised module still uses, named by the unoptimised build of the same
shader (`spirv-opt` strips names). For every pass and every one of the 256 option values (wave64 and FP16 bits included)
it matched the reflection of AMD's pre-built headers (removed from this subset) exactly.

The backend smoke tests deliberately hide
`vkGetBufferMemoryRequirements2KHR` from the injected device procedure loader.
Both the low-level and public-API context paths must query and use the core
`vkGetBufferMemoryRequirements2` fallback, complete two temporal dispatches,
and retain zero validation warnings or errors. They create a device with the
AMD coherent-memory feature deliberately disabled and audit every allocation
made through the backend procedure table; no selected memory type may carry
`VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD` even when such physical types are
available.
The smoke device also leaves subgroup-size control and synchronization2
disabled while their physical support is queried. Capability reporting must
remain conservative, pipeline creation must not attach required-subgroup-size
metadata, and disabled debug-utils/AMD buffer-marker procedures must not be
loaded.

## Implemented 1.1.4 analytical frame-generation slice

The exact source closure for Frame Interpolation 1.1.3 and Optical Flow 1.1.2
is now vendored alongside the upscaler: their 18 GLSL pass wrappers, GPU
headers, host implementations, and shader-blob accessors. The reusable public
C API owns two independent backend/scratch allocations, Optical Flow output,
and FI's three shared depth/motion resources. `Prepare` records OF and FI
preparation for one real frame; `RecordDispatch` records the generated image.
It deliberately does not acquire, submit, or present.

`tools/import_fsr3_framegen_upstream.sh` imports this exact closure using
`git archive` from the pinned tag and refuses to overwrite a mismatched source.
`tools/generate_fsr3_framegen_shaders.sh` reproduces the FI/OF four-variant
matrix. The checked-in set has 164 aggregate/blob headers: all 352 valid FI
and 56 valid OF lookups resolve, yielding 30 and 14 unique SPIR-V modules
respectively. They pass `spirv-val --target-env vulkan1.2` together with the
upscaler modules.

The pinned v1.1.4 FFX_SC hash remains required for a release reproduction.
The current checked-in FI/OF headers were generated with the explicitly
acknowledged newer local FFX_SC hash
`6504321763d333de77edef63103777bb0c0d761f86c4467d48d45b4f14a68793`
and the pinned glslang hash, because the historical FFX_SC executable was not
available locally. The generator rejects that substitution unless
`FFX_VK_ALLOW_UNPINNED_TOOLS=1` is deliberately set; this is provenance, not a
claim of byte-identical historical output.

On the RX 6800M/RADV test device, the public API records and submits reset
frame 1 plus temporal frame 2 at 64x64 to 128x128. Both outputs are finite,
nonzero, and completely overwrite RGBA16F poison; Vulkan validation reports
zero warnings and errors. The test also enforces that an interpolation source
is sampled and transfer-source capable (FI copies it into history) and that a
generated output is both storage and sampled capable (later FI passes sample
it). These required usages were previously implicit in the upstream effect.

Do not add `sdk/src/backends/vk/FrameInterpolationSwapchain` to the portable
presenter. Despite its Vulkan API, it unconditionally uses `Windows.h`, Win32
events, critical sections, threads, performance counters, and priority APIs.
It also requires game, async-compute, present, and acquire queue handles to all
be distinct.

## SDK 2.3 algorithm port

The public v2.3.0 upscaler, Frame Interpolation, and Optical Flow
source/header closure is now vendored at
`upstream/ffx-2.3.0/Kits/FidelityFX` by
`tools/import_fsr3_3_1_5_upstream.sh`. The importer pins commit
`60f4ea81909200d8542eca14dccb2628b763a9a3`, admits only files carrying an
individual MIT grant, refuses to overwrite a modified import, and records the
166 pristine source hashes in `ORIGINAL_SHA256SUMS`. The three public DX12
backend source files are retained strictly as the authoritative ABI reference;
they are not built on Vulkan. `CURRENT_SHA256SUMS`
records the documented narrow Linux-port changes: the non-Windows empty `FFX_API_ENTRY`,
the larger opaque context budget required by four-byte `wchar_t`, the
compile-time exclusion of the unpublished AMD watermark/git-header path, three
documented portability/callback fixes in the FSR3 upscaler source and its HLSL
callback declarations, and FI/OF error propagation. The latter ensures a
portable Vulkan bridge failure cannot be silently presented as an undefined
generated frame.

The v2.3 Frame Interpolation 3.1.6 and Optical Flow sources are imported for
the reusable analytical-FG port. They compile as the separate object-only
`ffx-vulkan::fsr3-framegen-3.1.6-scaffold` target, with every
exported FI/OF entry point privately prefixed `ffxVk316...`; this proves they
can coexist with the linked 1.1.4 runtime without accidentally binding its
symbols. `tools/generate_fsr3_3_1_6_framegen_spirv.sh` derives the fixed
Q2RTX profile from all 11 FI plus 7 OF public HLSL entry points and emits the
checked 18-module Vulkan 1.2 bundle under
`generated/ffx-2.3.0/vk/fsr3framegeneration-q2-v1`. It probes each HLSL
module at disjoint descriptor ranges, then recompiles it with the smallest
non-overlapping ranges; this both catches collisions and avoids sparse Vulkan
layouts. Source and output SHA-256 manifests plus CTests make the bundle
reproducible. The fixed Q2RTX profile's real SDK blob accessors now return the
18 checked embedded Vulkan modules by FI/OF pass and reject every unsupported
permutation; their dedicated test verifies all returned blobs are SPIR-V and
checks both error and Wave64-query paths. A separate host-graph test links the
real 3.1.6 FI/OF schedulers with the public SDK core and proves context
creation allocates 42 persistent resources and creates all 18 embedded-SPIR-V
pipelines (11 FI plus 7 OF), then releases them cleanly. It also records the
real reset Optical-Flow → FI Prepare → FI Dispatch graph against a strict mock:
15 dynamic resource registrations, 12 staged constant buffers, and 30 final
FI GPU jobs. A direct RX 6800M test also creates every one of the 18 Vulkan
compute pipelines. This is still not a Vulkan scheduler/backend bridge or
portable presenter. Those are the next independent implementation and
validation milestones; do not replace them with a dummy scheduler merely to
make this target link.

`ffx-vulkan::fsr3-vk-framegeneration-3.1.6` packages that source closure with
the shared Vulkan bridge behind an opaque, versioned C lifecycle:
`create -> prepare -> dispatch -> retire -> destroy`. It owns the five shared
FI/OF images and retains imported application image views until its explicit
post-fence `RetireFrame(completedFrameId)` call; monotonic IDs permit multiple
queue-ordered frames before a fence retires them. The public-only RX 6800M
smoke records reset and temporal frames with zero validation warnings/errors
for both RGBA8 and RGBA16F presentation images. It is a compute
library only; portable presentation and engine integration are intentionally
separate layers.

The existing SDK-2.3 Vulkan resource/job bridge now accepts a valid SPIR-V
`FfxShaderBlob` directly before consulting its legacy 3.1.5 pass catalogue.
That keeps allocation, import, barrier, descriptor, and job-recording code
reusable for public effects whose scheduler supplies Vulkan blobs (including
the fixed 3.1.6 FI/OF profile), while preserving the working 3.1.5 fallback.
The RX 6800M bridge test passes an FI blob with an intentionally empty pipeline
name, proving this direct-SPIR-V route is selected rather than the fallback.
The first direct FI/OF context-creation attempt also supplied the measured
next gap: these modules use `rw_counters`/`r_counters` storage-buffer bindings
and declare storage-image read/write-without-format capabilities. Commit
`1c51d094` binds those known counter resources through reflected bridge
metadata, the SDK buffer tables, Vulkan storage-buffer descriptors, and
compute-job recording. The follow-up reflector parses SPIR-V variables,
pointer storage classes, and image declarations rather than inferring the
descriptor type from a resource name. Only the SDK `r_`/`rw_` convention
selects StorageBuffer direction because Vulkan uses one descriptor type for
both. The RX 6800M bridge test now queries and enables the two required
storage-image-without-format feature bits only when both are supported, then
creates FI and OF contexts and submits their initialization jobs with no Vulkan
validation warning or error. This establishes the context-creation seam only;
importing real frame resources and dispatching Optical Flow plus FI remain the
next validation gate.

`ffx-vulkan::fsr3-host-3.1.5-scaffold` compiles the effect and the necessary
core helpers as an object library with those port defines. It is intentionally
not linked into the 1.1.4 backend or public C API: success proves that the
public scheduler source is portable enough to begin the port, not that any
SDK-2.3 GPU work has run. Its always-on host-graph CTest drives reset and
temporal sharpened dispatches through a mock `FfxInterface`: it proves 19
persistent resources, four initialization uploads, eleven live pipelines,
and the first/temporal job graphs (27/21 jobs). The manifest, object build,
and graph test are required before changing the 2.3 backend bridge.

`tools/generate_fsr3_3_1_5_spirv.sh` compiles the ten public HLSL pass wrappers
plus the distinct AccumulateSharpen permutation (11 modules) to
`generated/ffx-2.3.0/vk/fsr3upscaler-q2-v2`. It pins the local DXC build, uses the
Q2-compatible linear-HDR / low-resolution / unjittered-motion /
non-inverted-depth profile, packs each pass's SRV/UAV/CBV slots densely, and
uses the established static-sampler 1000+ range. Generation validates every
module against Vulkan 1.2 and rejects duplicate descriptor bindings before it
emits both the SPIR-V manifest and a byte-for-byte checked embedded C bundle.
The C pipeline factory reflects those exact bindings,
creates immutable FFX-compatible samplers, and has created all 11 modules as
real Vulkan compute pipelines on the RX 6800M. The first `FfxInterface`
callback layer in `ffx_vk_fsr3_3_1_5_bridge.cpp` maps the SDK scheduler's
pipeline names to those modules, retains the Vulkan objects for each
`FfxPipelineState`, and returns the reflected SRV/UAV/CBV names and compact
binding slots for the SDK's resource patch-up. Its GPU test creates/destroys
all eleven callback pipelines. It now owns SDK-created Vulkan buffers/images,
including full SRVs and one UAV view per mip, and records ordered staging-buffer
copies for initialized images. A real RX 6800M context creates all nineteen
persistent resources, executes all four initialization uploads, and destroys
cleanly. Application-image import, uniform staging, barriers, and compute job
execution are still backend-port work. General-purpose profile and wave/FP16
permutations also remain.

The reusable descriptor bridge now creates one exact descriptor pool/set from
that pipeline metadata and writes every required SRV/UAV/CBV by reflected name.
It deliberately treats static samplers as immutable layout state, and rejects
missing, duplicated, or unrecognised resource names before touching Vulkan.
The RX 6800M pipeline test exercises this for every generated module with real
image views and uniform buffers. Resource creation/import and command recording
remain the next layer; this is descriptor infrastructure, not a dispatched
upscale frame.

`ffx_vk_fsr3_3_1_5_reflection` is a small public C helper which reads the
generated module's `OpName`/`Binding` records and classifies the stable FSR
SRV/UAV/sampler/CBV names. Its test uses a real prepare-inputs module, checks
the color/depth/constant-buffer names, rejects a too-small result array without
partial writes, and rejects duplicate bindings. The resource/job bridge must
use this metadata rather than a hand-copied DX12 root-signature table.

SDK 2.3 supplies no Vulkan backend. Its open effect implementation is split
between:

- `Kits/FidelityFX/api/{include,internal}` for the 2.3 interface and core;
- `Kits/FidelityFX/upscalers/fsr3/{include,internal}` for the 3.1.5 upscaler;
- `Kits/FidelityFX/framegeneration/fsr3/{include,internal}` for 3.1.6 frame
  interpolation and optical flow; and
- the 10 upscaler, 11 interpolation, and 7 optical-flow HLSL pass wrappers.

Use the low-level `ffxFsr3Upscaler*`, `ffxFrameInterpolation*`, and
`ffxOpticalflow*` APIs. The provider and DX12 swapchain layers are not required.
The three effect shader-blob sources expect the same four variant tables per
pass, again totaling 112 generated headers.

The public 2.3 tree is not self-contained as published: the upscaler private
header and implementation refer to absent
`Kits/FidelityFX/amdinternal/api/internal/ffx_watermark.h` and
`git_hash_branch.h`. The Vulkan port must gate out that optional watermark with
a module-specific build define; it must not recreate or copy the absent
internal component. The portable Vulkan backend must also implement
`GetResourceSizeFromDescription`, which the low-level effects call through
`ffx_backends.h`.

Do not link a 2.3 effect directly to the 1.1.4 backend ABI. Port `ffx_vk.cpp`
to the SDK 2.3 `FfxInterface`, using the 2.3 DX12 backend as the authoritative
delta. Important interface changes include `FfxApiResource*` types, effect-side
shader blobs passed to `fpCreatePipeline`, heap callbacks, queued-job queries,
and swapchain ABI reporting. The old generic blob dispatcher is then replaced
by the effect-local 2.3 shader-blob sources.

For the 2.3 HLSL pass wrappers, prefer DXC's native SPIR-V target with explicit
Vulkan binding/target arguments. Compare resource reflection and output against
the older GLSL path before accepting it. A generated-header manifest must pin
the DXC commit, all command arguments, permutation defines, source hashes, and
SPIR-V validation result.

## Stable module ABI

`include/ffx_vk_portable.h` is the application boundary and does not expose an
AMD private context, `FfxInterface`, C++ type, or `wchar_t`. All public structs
start with `structSize`; the ABI has its own version independent of SDK tags.
The implemented 1.1.4 upscaler object owns its Vulkan backend interface,
aligned scratch allocation, temporal resources, and the three shared upscaler
images required by the low-level dispatch. The public create/destroy/record
path has a dedicated validation-layer readback test and remains linkable as an
unsupported stub when the backend build option is disabled.

Opaque implementation objects ultimately own:

1. one Vulkan backend interface and scratch allocator;
2. an independently optional upscaler context;
3. optical-flow and frame-interpolation contexts plus shared resources; and
4. presenter state and synchronization.

Creation receives application-owned `VkInstance`, `VkPhysicalDevice`,
`VkDevice`, `PFN_vkGetDeviceProcAddr`, allocation callbacks, and explicit queue
family/queue handles. Dispatch receives an application command buffer and
external images with their current state. The module creates views/internal
images but never creates a second Vulkan device, destroys application objects,
or advances the application's rendered-frame ID for generated frames.

The record-only frame-generation create/prepare/dispatch/destroy calls are
implemented. Presentation and UI composition stay a separate opt-in object so
upscaling and raw interpolation can be reused without replacing an
application's swapchain.

## Portable presenter contract

The correctness-first presenter uses application swapchain images and explicit
acquire/present calls. It records upscaling and frame-generation work into
off-screen images, composites UI after interpolation for both real and
generated frames, and submits in display order using timeline semaphores (or a
fence/binary-semaphore fallback). It must support one graphics/present queue;
an independent compute queue is an optimization, not a requirement. Pacing uses
a monotonic clock and never Win32 synchronization.

Q2RTX currently exposes one graphics/present queue and one transfer queue. The
test RX 6800M exposes one graphics queue and four compute-only queues, so AMD's
old four-distinct-queue swapchain contract cannot be satisfied on this machine.

## Verification gates

Before Q2RTX integration, require all of the following in the standalone tree:

- host/API contract tests and warning-clean GCC/Clang builds;
- SHA/source-manifest verification for vendored and generated files;
- `spirv-val` on every permutation plus reflection-table checks;
- Vulkan backend creation, all effect-context creation, and one dispatch under
  validation layers;
- readback tests that reject all-zero, NaN/Inf, partially written, and
  incorrect-alpha outputs;
- camera-cut, resize, dynamic-resolution, and frame-ID reset tests; and
- a single-queue acquire/generate/UI/present test on the RX 6800M.
