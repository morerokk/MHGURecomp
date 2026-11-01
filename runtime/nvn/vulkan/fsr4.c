// FSR 4 through AMD's driver (see fsr4.h and docs/FSR4-PORT.md).
//
// The AMD Vulkan driver package carries amdvkffx64.dll, which exports UpdateFfxApiProvider(Ex): the FidelityFX
// "external provider" hook. Called with a description type it fills in a table of function pointers (create / destroy /
// configure / query / dispatch contexts) for that effect - for the upscaler the FSR 4 ML upscaler, which then runs on
// our own VkDevice and records into our own VkCommandBuffer. The structures are those of the MIT-licensed ffx-api
// (third_party/ffx-api, FidelityFX SDK 1.1.4); the driver hands out providers only when it sees the SDK's loader
// modules in the process, which it asks for with GetModuleHandleW, so that one import of the driver DLL answers with the
// DLL itself (as OptiScaler, GPL-3.0, was read to learn - no code taken). Nothing of AMD's is shipped or linked: the
// DLL is found next to the exe, or in the driver store folder of the GPU, at run time. That is route 1 ("driver"), only
// when built with FSR4_DRIVER_PATCH=1 (off by default).
// Route 2 ("loader"): AMD's own FidelityFX loader amd_fidelityfx_vk.dll that the player supplies (beside the exe, or
// MHGU_FSR4_LOADER=<path>), used through the public ffx-api (ffxQuery / ffxCreateContext / ffxDispatch): no patching.
// UNVERIFIED: no loader that offers FSR 4 was available to test with (see docs/FSR4-PORT.md).
// Neither route: FSR 3.1 (fsr.c).
//
//   MHGU_FSR_DEBUG=1   FFX debug checking of the inputs (messages in the log)
//   MHGU_FSR4_ROUTE=driver|loader   try only that route (default: driver if compiled in, then loader)
//   MHGU_FSR4_LOADER=<path>         the loader DLL for route 2
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "fsr4.h"

// FSR4_DRIVER_PATCH (Makefile, default 0): 1 = the driver route below is compiled in (it patches one import of AMD's
// amdvkffx64.dll in memory); 0 = none of that code exists and only the player-supplied loader route is left.
#ifndef FSR4_DRIVER_PATCH
#define FSR4_DRIVER_PATCH 0
#endif

// g_fsr4_fallback (the settings panel shows it on the Upscaler row; settings_locale.h translates it)
#define NO_FSR4_GPU "This GPU can't run FSR 4, so FSR 3.1 is used."
#define NO_FSR4_START "FSR 4 could not start, so FSR 3.1 is used. The [fsr4] lines in log.txt say why."

#ifndef _WIN32

bool vk_fsr4_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext) { (void)ep, (void)n, (void)ext, (void)n_ext; return false; }
bool vk_fsr4_device_features(VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceFeatures *f) { (void)f11, (void)f12, (void)f; return false; }
bool vk_fsr4_init(void) { LOG("[fsr4] FSR 4 needs AMD's Windows driver: FSR 3.1 is used\n"); g_fsr4_fallback = NO_FSR4_START; return false; }
bool vk_fsr4_ready(void) { return false; }
const char *vk_fsr4_name(void) { return "FSR 4"; }
bool vk_fsr4_dispatch(const Fsr4Dispatch *d) { (void)d; return false; }

#else

#include <windows.h>

#include <ffx_api.h>
#include <ffx_upscale.h>

// ---- what the driver's provider table looks like (ffx-api's ffxProviderInterface / ExternalProviderData, with the
// version-3 tail: isSupported takes { VkDevice, VkPhysicalDevice, vkGetDeviceProcAddr })
typedef struct {
    uint64_t versionId;
    const char *versionName;
    bool (*canProvide)(uint64_t descType);
    ffxReturnCode_t (*createContext)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
    ffxReturnCode_t (*destroyContext)(ffxContext *, const ffxAllocationCallbacks *);
    ffxReturnCode_t (*configure)(ffxContext *, const ffxConfigureDescHeader *);
    ffxReturnCode_t (*query)(ffxContext *, ffxQueryDescHeader *);
    ffxReturnCode_t (*dispatch)(ffxContext *, const ffxDispatchDescHeader *);
} ProviderIf;
typedef struct {
    uint32_t structVersion;
    uint64_t descType;
    ProviderIf provider;
    bool (*isSupported)(const void *backend);
    void *reserved;
} ExternalProvider;
typedef struct ProviderOption { uint32_t type, effect; bool enable; struct ProviderOption *next; } ProviderOption;
_Static_assert(sizeof(ExternalProvider) == 0x60, "the driver's version-3 provider record is 0x60 bytes");

// The Vulkan flavour of the create descriptors the driver provider knows (FidelityFX SDK 2.x ids; the layouts are those
// of ffxCreateBackendVKDesc and of the upscaler's version override).
#define BACKEND_VK_V2_TYPE 0x01000003u
#define UPSCALE_VERSION_V2_TYPE 0x0001000Bu
#define FSR4_VERSION ((4u << 22) | (1u << 12) | 2u)  // the driver takes 4.0.0 .. 4.1.2
typedef struct {
    ffxCreateContextDescHeader header;
    VkDevice device;
    VkPhysicalDevice physical;
    PFN_vkGetDeviceProcAddr gdpa;
} BackendVk;
typedef struct {
    ffxCreateContextDescHeader header;
    uint32_t version;
} UpscaleVersion;

#if FSR4_DRIVER_PATCH
static HMODULE drv;
static ExternalProvider prov;
#endif
static char prov_name[64];
// the three calls the dispatch code uses, from either route
static ffxReturnCode_t (*f_create)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_destroy)(ffxContext *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_dispatch)(ffxContext *, const ffxDispatchDescHeader *);
static bool via_loader;       // route 2
static uint64_t loader_ver;   // route 2: the FSR 4 version id the loader offered
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static bool loaded, failed, extensions_ok, linear_in;  // linear_in: MHGU_FSR4_LINEAR=1 (A/B: the colour is not flagged as gamma-encoded)
static uint64_t frames;

// ================================================================== the device
bool vk_fsr4_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext) {
    static const char *const want[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                       VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME};
    for (int i = 0; i < 4; i++) {
        bool have = false;
        for (uint32_t j = 0; j < n; j++) have |= !strcmp(ep[j].extensionName, want[i]);
        if (!have) {
            LOG("[fsr4] the GPU lacks %s: FSR 4 cannot run here, FSR 3.1 will\n", want[i]);
            g_fsr4_fallback = NO_FSR4_GPU;
            return false;
        }
    }
    for (int i = 0; i < 4; i++) ext[(*n_ext)++] = want[i];
    extensions_ok = true;
    return true;
}

static VkPhysicalDeviceCooperativeMatrixFeaturesKHR en_cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
static VkPhysicalDeviceShaderFloat8FeaturesEXT en_f8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
static VkPhysicalDeviceSynchronization2FeaturesKHR en_s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
static VkPhysicalDeviceSubgroupSizeControlFeaturesEXT en_sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};

bool vk_fsr4_device_features(VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceFeatures *f) {
    if (!extensions_ok) return false;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {.sType = en_cm.sType};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {.sType = en_f8.sType, .pNext = &cm};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2 = {.sType = en_s2.sType, .pNext = &f8};
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sg = {.sType = en_sg.sType, .pNext = &s2};
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &sg};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFeatures2 h = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &h);
    if (!cm.cooperativeMatrix || !f8.shaderFloat8 || !f8.shaderFloat8CooperativeMatrix || !s2.synchronization2 ||
        !sg.subgroupSizeControl || !sg.computeFullSubgroups || !h12.shaderFloat16 || !h12.shaderInt8 || !h12.bufferDeviceAddress ||
        !h12.vulkanMemoryModel || !h.features.shaderInt16 || !h11.storageBuffer16BitAccess) {
        LOG("[fsr4] the GPU lacks a feature FSR 4 needs (cooperative matrix %d, float8 %d/%d, synchronization2 %d, subgroup size "
            "control %d/%d, fp16 %d, int8 %d, buffer device address %d, memory model %d): FSR 3.1 will run\n", cm.cooperativeMatrix,
            f8.shaderFloat8, f8.shaderFloat8CooperativeMatrix, s2.synchronization2, sg.subgroupSizeControl, sg.computeFullSubgroups,
            h12.shaderFloat16, h12.shaderInt8, h12.bufferDeviceAddress, h12.vulkanMemoryModel);
        extensions_ok = false;  // (the extensions stay enabled: harmless without their features)
        g_fsr4_fallback = NO_FSR4_GPU;
        return false;
    }
    en_cm.cooperativeMatrix = en_cm.cooperativeMatrixRobustBufferAccess = VK_TRUE;
    en_f8.shaderFloat8 = en_f8.shaderFloat8CooperativeMatrix = VK_TRUE;
    en_s2.synchronization2 = VK_TRUE;
    en_sg.subgroupSizeControl = en_sg.computeFullSubgroups = VK_TRUE;
    f12->shaderFloat16 = f12->shaderInt8 = f12->bufferDeviceAddress = f12->vulkanMemoryModel = VK_TRUE;
    f12->vulkanMemoryModelDeviceScope = h12.vulkanMemoryModelDeviceScope;
    f12->shaderSubgroupExtendedTypes = h12.shaderSubgroupExtendedTypes;
    f12->uniformAndStorageBuffer8BitAccess = f12->storageBuffer8BitAccess = VK_TRUE;
    f12->descriptorIndexing = h12.descriptorIndexing;
    f12->runtimeDescriptorArray = h12.runtimeDescriptorArray;
    f12->descriptorBindingPartiallyBound = h12.descriptorBindingPartiallyBound;
    f12->shaderSampledImageArrayNonUniformIndexing = h12.shaderSampledImageArrayNonUniformIndexing;
    f12->shaderStorageImageArrayNonUniformIndexing = h12.shaderStorageImageArrayNonUniformIndexing;
    f11->storageBuffer16BitAccess = f11->uniformAndStorageBuffer16BitAccess = VK_TRUE;
    f11->variablePointers = h11.variablePointers;
    f11->variablePointersStorageBuffer = h11.variablePointersStorageBuffer;
    f->shaderInt16 = VK_TRUE;
    f->shaderStorageImageReadWithoutFormat = f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // splice the extension structures in after the 1.2 features (whatever f12 pointed to stays behind them)
    en_cm.pNext = f12->pNext;
    en_f8.pNext = &en_cm;
    en_s2.pNext = &en_f8;
    en_sg.pNext = &en_s2;
    f12->pNext = &en_sg;
    return true;
}

static void file_version(const wchar_t *path, char *out, size_t n) {
    snprintf(out, n, "?");
    DWORD z, sz = GetFileVersionInfoSizeW(path, &z);
    if (!sz) return;
    void *buf = malloc(sz);
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &l) && fi)
        snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
    free(buf);
}

#if FSR4_DRIVER_PATCH
// ================================================================== route 1: finding and loading the driver's DLL
typedef UINT KmtHandle;
typedef struct { LUID luid; KmtHandle adapter; } KmtOpen;
typedef struct { UINT version; WCHAR name[MAX_PATH]; } KmtUmd;
typedef struct { KmtHandle adapter; UINT type; void *data; UINT size; } KmtQuery;
typedef struct { KmtHandle adapter; } KmtClose;

// the driver store folder of the Vulkan device's GPU: where D3D12 loads its user-mode driver from (same package)
static bool store_folder(wchar_t *dir) {
    VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (!idp.deviceLUIDValid) return false;
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return false;
    LONG (WINAPI *open)(KmtOpen *) = (void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
    LONG (WINAPI *query)(KmtQuery *) = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    LONG (WINAPI *close)(KmtClose *) = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    bool ok = false;
    KmtOpen o = {0};
    memcpy(&o.luid, idp.deviceLUID, sizeof o.luid);
    if (open && query && close && open(&o) == 0) {
        for (UINT ver = 3; ver != (UINT)-1 && !ok; ver = ver ? 0 : (UINT)-1) {  // (the DX12, then the DX9 user-mode driver)
            KmtUmd u = {.version = ver};
            KmtQuery q = {.adapter = o.adapter, .type = 1 /* KMTQAITYPE_UMDRIVERNAME */, .data = &u, .size = sizeof u};
            if (query(&q) == 0 && u.name[0]) {
                wcsncpy(dir, u.name, MAX_PATH - 1);
                wchar_t *slash = wcsrchr(dir, L'\\');
                if (slash) { slash[1] = 0; ok = wcsstr(dir, L"DriverStore") != NULL; }
            }
        }
        KmtClose c = {o.adapter};
        close(&c);
    }
    FreeLibrary(gdi);
    return ok;
}

// the newest amdvkffx64.dll in the driver store (when the GPU's own folder could not be found)
static bool newest_in_store(wchar_t *out) {
    wchar_t root[MAX_PATH], pat[MAX_PATH];
    UINT n = GetSystemDirectoryW(root, MAX_PATH);
    if (!n || n > MAX_PATH - 40) return false;
    wcscat(root, L"\\DriverStore\\FileRepository\\");
    FILETIME best = {0};
    bool found = false;
    WIN32_FIND_DATAW a, b;
    swprintf(pat, MAX_PATH, L"%ls*", root);
    HANDLE ha = FindFirstFileW(pat, &a);
    if (ha == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || a.cFileName[0] == L'.') continue;
        swprintf(pat, MAX_PATH, L"%ls%ls\\*", root, a.cFileName);
        HANDLE hb = FindFirstFileW(pat, &b);
        if (hb == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || b.cFileName[0] == L'.') continue;
            wchar_t cand[MAX_PATH];
            swprintf(cand, MAX_PATH, L"%ls%ls\\%ls\\amdvkffx64.dll", root, a.cFileName, b.cFileName);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(cand, GetFileExInfoStandard, &fa) && (!found || CompareFileTime(&fa.ftLastWriteTime, &best) > 0)) {
                best = fa.ftLastWriteTime, found = true;
                wcscpy(out, cand);
            }
        } while (FindNextFileW(hb, &b));
        FindClose(hb);
    } while (FindNextFileW(ha, &a));
    FindClose(ha);
    return found;
}

// ---- the driver DLL asks "is amd_fidelityfx_loader_vk.dll / ..._upscaler_vk.dll loaded?" before it hands out a
// provider: it is, as far as it can see (answer: itself). Only that DLL's own import is changed.
static HMODULE (WINAPI *real_gmh)(LPCWSTR);
static HMODULE WINAPI gmh_hook(LPCWSTR name) {
    HMODULE r = real_gmh(name);
    if (!r && name && !_wcsnicmp(name, L"amd_fidelityfx_", 15)) {
        HMODULE caller = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)__builtin_return_address(0), &caller);
        if (caller == drv) return drv;
    }
    return r;
}

static bool patch_import(HMODULE m) {
    uint8_t *b = (uint8_t *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        IMAGE_THUNK_DATA *o = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; o->u1.AddressOfData; o++, t++) {
            if (IMAGE_SNAP_BY_ORDINAL(o->u1.Ordinal)) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(b + o->u1.AddressOfData))->Name, "GetModuleHandleW")) continue;
            DWORD old;
            if (!VirtualProtect(&t->u1.Function, sizeof t->u1.Function, PAGE_READWRITE, &old)) return false;
            real_gmh = (void *)t->u1.Function;
            t->u1.Function = (ULONG_PTR)gmh_hook;
            VirtualProtect(&t->u1.Function, sizeof t->u1.Function, old, &old);
            return true;
        }
    }
    return false;
}

static bool init_driver(void) {
    wchar_t path[MAX_PATH] = {0}, dir[MAX_PATH] = {0};
    const char *how = "beside the exe";
    const char *env_dll = getenv("MHGU_FSR4_DLL");  // (test: another DLL, or a missing one to see the fallback)
    wchar_t *slash = NULL;
    if (env_dll) {
        how = "MHGU_FSR4_DLL";
        MultiByteToWideChar(CP_UTF8, 0, env_dll, -1, path, MAX_PATH);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            LOG("[fsr4] MHGU_FSR4_DLL=%s does not exist: FSR 3.1 is used\n", env_dll);
            return false;
        }
    } else {
        GetModuleFileNameW(NULL, path, MAX_PATH);
        slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"amdvkffx64.dll");
    }
    if (!env_dll && (!slash || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)) {
        how = "driver store folder of this GPU";
        if (store_folder(dir)) {
            swprintf(path, MAX_PATH, L"%lsamdvkffx64.dll", dir);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) path[0] = 0;
        } else {
            path[0] = 0;
        }
        if (!path[0]) {
            how = "newest in the driver store";
            if (!newest_in_store(path)) {
                LOG("[fsr4] amdvkffx64.dll not found (not beside the exe, not in the driver store): the AMD driver has no Vulkan "
                    "FSR 4 (RDNA 4 GPU and a recent Adrenalin driver needed). FSR 3.1 is used\n");
                return false;
            }
        }
    }
    drv = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!drv) { LOG("[fsr4] %ls could not be loaded (error %lu): FSR 3.1 is used\n", path, GetLastError()); return false; }
    char ver[32];
    file_version(path, ver, sizeof ver);
    LOG("[fsr4] loaded %ls (%s), file version %s\n", path, how, ver);
    typedef HRESULT (*UpdateEx)(void *, uint32_t, ProviderOption *);
    typedef HRESULT (*Update)(void *, uint32_t);
    UpdateEx update_ex = (UpdateEx)GetProcAddress(drv, "UpdateFfxApiProviderEx");
    Update update = (Update)GetProcAddress(drv, "UpdateFfxApiProvider");
    if (!update_ex && !update) { LOG("[fsr4] the DLL has no UpdateFfxApiProvider: FSR 3.1 is used\n"); return false; }
    LOG("[fsr4] SDK-module check hook %s\n", patch_import(drv) ? "installed" : "NOT installed (the driver may refuse)");
    ProviderOption opt = {.effect = 0 /* upscaler */, .enable = true};
    prov = (ExternalProvider){.structVersion = 3, .descType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE};
    HRESULT hr = update_ex ? update_ex(&prov, 0x60, &opt) : update(&prov, 0x60);
    if (FAILED(hr) || !prov.provider.createContext) {
        LOG("[fsr4] the driver gave no upscaler provider (0x%08x): FSR 3.1 is used\n", (unsigned)hr);
        return false;
    }
    snprintf(prov_name, sizeof prov_name, "FSR %s", prov.provider.versionName ? prov.provider.versionName : "4");
    for (char *c = prov_name + strlen(prov_name); c > prov_name && (c[-1] == '*' || c[-1] == ' '); c--) c[-1] = 0;
    LOG("[fsr4] driver provider \"%s\" id %#llx\n", prov_name, (unsigned long long)prov.provider.versionId);
    if (prov.isSupported) {
        struct { VkDevice d; VkPhysicalDevice p; PFN_vkGetDeviceProcAddr g; } be = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr};
        if (!prov.isSupported(&be)) { LOG("[fsr4] the provider says this GPU / device setup is not supported: FSR 3.1 is used\n"); return false; }
        LOG("[fsr4] the provider accepts this device\n");
    }
    f_create = prov.provider.createContext, f_destroy = prov.provider.destroyContext, f_dispatch = prov.provider.dispatch;
    return true;
}
#endif  // FSR4_DRIVER_PATCH

// ================================================================== route 2: the loader the player supplies (UNVERIFIED)
// AMD's amd_fidelityfx_vk.dll (FidelityFX SDK, downloaded by the player) exports the public ffx-api. It picks the provider
// itself, the way AMD intends, so nothing is patched. The backend descriptor is ffx-api's public Vulkan one (ffx_api_vk.h:
// type 0x3); the FSR 4 version is chosen from ffxQuery's list (names starting with "4.").
#define BACKEND_VK_PUBLIC_TYPE 0x0000003u
static bool init_loader(void) {
    wchar_t path[MAX_PATH] = {0};
    const char *env = getenv("MHGU_FSR4_LOADER");
    if (env) MultiByteToWideChar(CP_UTF8, 0, env, -1, path, MAX_PATH);
    else {
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"amd_fidelityfx_vk.dll"); else path[0] = 0;
    }
    if (!path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LOG("[fsr4] no loader supplied (amd_fidelityfx_vk.dll beside the exe, or MHGU_FSR4_LOADER=<path>)\n");
        return false;
    }
    HMODULE ld = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ld) { LOG("[fsr4] %ls could not be loaded (error %lu)\n", path, GetLastError()); return false; }
    char ver[32];
    file_version(path, ver, sizeof ver);
    LOG("[fsr4] loader %ls, file version %s\n", path, ver);
    PfnFfxQuery query = (PfnFfxQuery)GetProcAddress(ld, "ffxQuery");
    f_create = (void *)GetProcAddress(ld, "ffxCreateContext");
    f_destroy = (void *)GetProcAddress(ld, "ffxDestroyContext");
    f_dispatch = (void *)GetProcAddress(ld, "ffxDispatch");
    if (!query || !f_create || !f_destroy || !f_dispatch) { LOG("[fsr4] the loader lacks the ffx-api exports\n"); return false; }
    uint64_t n = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions gv = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                         .device = &g_vk.dev, .outputCount = &n, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t rc = query(NULL, &gv.header);
    if (rc != FFX_API_RETURN_OK) { LOG("[fsr4] the loader's version query failed (ffx return code %d)\n", (int)rc); return false; }
    int best = -1;
    for (uint64_t i = 0; i < n; i++) {
        LOG("[fsr4] loader offers upscaler %s (id %#llx)\n", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
        if (names[i] && names[i][0] == '4' && names[i][1] == '.' && (best < 0 || strcmp(names[i], names[best]) > 0)) best = (int)i;
    }
    if (best < 0) { LOG("[fsr4] this loader offers no FSR 4 (4.x) upscaler\n"); return false; }
    loader_ver = ids[best];
    snprintf(prov_name, sizeof prov_name, "FSR %s", names[best]);
    via_loader = true;
    return true;
}

bool vk_fsr4_init(void) {
    if (!extensions_ok) return false;
    const char *route = getenv("MHGU_FSR4_ROUTE");
    bool ok = false;
#if FSR4_DRIVER_PATCH
    if (!route || !strcmp(route, "driver")) {
        ok = init_driver();
        if (ok) LOG("[fsr4] route: the driver's provider (amdvkffx64.dll, import patch)\n");
    }
#else
    if (route && !strcmp(route, "driver")) LOG("[fsr4] built with FSR4_DRIVER_PATCH=0: the driver route does not exist\n");
#endif
    if (!ok && (!route || !strcmp(route, "loader"))) {
        ok = init_loader();
        if (ok) LOG("[fsr4] route: the player-supplied loader (ffx-api, no patching)\n");
    }
    if (!ok) {
        LOG("[fsr4] no FSR 4 route available: FSR 3.1 is used\n");
        g_fsr4_fallback = NO_FSR4_START;
        return false;
    }
    linear_in = getenv("MHGU_FSR4_LINEAR") != NULL;
    loaded = true;
    return true;
}

bool vk_fsr4_ready(void) { return loaded && !failed; }
const char *vk_fsr4_name(void) { return prov_name[0] ? prov_name : "FSR 4"; }

// ================================================================== context and dispatch
static void ffx_message(uint32_t type, const wchar_t *msg) {
    LOG("[fsr4] runtime %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// every image lives in GENERAL here (vk.h), which is what "unordered access" means to the backend: no layout changes
static struct FfxApiResource resource(const Fsr4Image *i, bool uav) {
    struct FfxApiResource r = {0};
    if (!i->image) return r;
    r.resource = (void *)i->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(i->format);
    r.description.width = i->width, r.description.height = i->height, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return r;
}

static bool fail(const char *why, int rc) {
    LOG("[fsr4] %s (ffx return code %d): FSR 4 is off from here on, FSR 3.1 takes over\n", why, rc);
    failed = true;
    g_fsr4_fallback = NO_FSR4_START;
    return false;
}

static bool make_context(const Fsr4Dispatch *d) {
    if (ctx && ctx_rw == d->render[0] && ctx_rh == d->render[1] && ctx_ow == d->out[0] && ctx_oh == d->out[1]) return true;
    if (ctx) { f_destroy(&ctx, NULL); ctx = NULL; }
    // LDR, gamma-encoded colour (the post-processed picture), standard depth, render-size motion vectors without jitter
    struct ffxCreateContextDescUpscale cu = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                      .flags = (linear_in ? 0 : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) | (getenv("MHGU_FSR_DEBUG") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0),
                                      .maxRenderSize = {d->render[0], d->render[1]}, .maxUpscaleSize = {d->out[0], d->out[1]},
                                      .fpMessage = ffx_message};
    UpscaleVersion uv = {.header.type = UPSCALE_VERSION_V2_TYPE, .version = FSR4_VERSION};
    BackendVk bd = {.header.type = via_loader ? BACKEND_VK_PUBLIC_TYPE : BACKEND_VK_V2_TYPE, .device = g_vk.dev, .physical = g_vk.phys,
                    .gdpa = vkGetDeviceProcAddr};
    struct ffxOverrideVersion ov = {.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION, .versionId = loader_ver};
    bd.header.pNext = via_loader ? &ov.header : &uv.header;
    cu.header.pNext = &bd.header;
    uint64_t t0 = plat_now_ns();
    ffxReturnCode_t rc = f_create(&ctx, &cu.header, NULL);
    if (rc != FFX_API_RETURN_OK || !ctx) { ctx = NULL; return fail("the FSR 4 context could not be created", (int)rc); }
    ctx_rw = d->render[0], ctx_rh = d->render[1], ctx_ow = d->out[0], ctx_oh = d->out[1];
    LOG("[fsr4] %s context %ux%u -> %ux%u created in %.0f ms (non-linear colour; colour VkFormat %d, depth %d, motion vectors %d, "
        "reactive %d, output %d)\n", prov_name, ctx_rw, ctx_rh, ctx_ow, ctx_oh, (plat_now_ns() - t0) / 1e6, d->color.format,
        d->depth.format, d->motion.format, d->reactive.image ? d->reactive.format : 0, d->output.format);
    return true;
}

bool vk_fsr4_dispatch(const Fsr4Dispatch *d) {
    if (!vk_fsr4_ready() || !make_context(d)) return false;
    struct ffxDispatchDescUpscale du = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = d->cb};
    du.color = resource(&d->color, false);
    du.depth = resource(&d->depth, false);
    du.motionVectors = resource(&d->motion, false);
    du.reactive = resource(&d->reactive, false);
    du.output = resource(&d->output, true);
    du.jitterOffset = (struct FfxApiFloatCoords2D){d->jitter[0], d->jitter[1]};
    du.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    du.renderSize = (struct FfxApiDimensions2D){d->render[0], d->render[1]};
    du.upscaleSize = (struct FfxApiDimensions2D){d->out[0], d->out[1]};
    du.enableSharpening = d->sharpen;
    du.sharpness = d->sharpness;
    du.frameTimeDelta = d->frame_ms;
    du.preExposure = 1.0f;
    du.reset = d->reset;
    du.cameraNear = d->cam_near;
    du.cameraFar = d->cam_far;
    du.cameraFovAngleVertical = d->fovy;
    du.viewSpaceToMetersFactor = d->view_to_meters;
    du.flags = linear_in ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    static int fail_at = -2;  // (test: MHGU_FSR4_FAILTEST=n reports the n-th dispatch as failed, to see the fallback)
    if (fail_at == -2) fail_at = getenv("MHGU_FSR4_FAILTEST") ? atoi(getenv("MHGU_FSR4_FAILTEST")) : -1;
    ffxReturnCode_t rc = fail_at >= 0 && frames == (uint64_t)fail_at ? FFX_API_RETURN_ERROR : f_dispatch(&ctx, &du.header);
    if (rc != FFX_API_RETURN_OK) return fail("the FSR 4 dispatch failed", (int)rc);
    if (++frames == 1) LOG("[fsr4] first %s dispatch recorded\n", prov_name);
    return true;
}

#endif
