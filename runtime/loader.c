// NSO loading (AArch32 modules), REL relocation and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "interp.h"
#include "mods.h"
#include "rt.h"

Module g_modules[MAX_MODULES];
int g_nmodules, g_nexefs;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_FINI = 13, DT_REL = 17, DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23,
       DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28 };
enum { R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_TLS_TPOFF32 = 19, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };
enum { DT_STRSZ = 10 };
#define TLS_MODULE_SIZE 0x1000u   // the block of one module's __thread data (an NRO says no more about its TLS segment)
#define TLS_TOTAL_MAX 0x10000u    // a thread's block, all modules
static uint32_t tls_next = 8;     // the TCB (two words), then the modules' blocks
uint32_t loader_tls_block_size(void) { return tls_next > 8 ? tls_next : 0; }

typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Elf32Sym;
typedef struct { int32_t tag; uint32_t val; } Elf32Dyn;

// MOD0 at mod0 (an offset in the image) -> the dynamic section's tables. Every offset must be inside the module's
// memory: a damaged NRO would otherwise read or write anywhere. false: they are not.
static bool in_module(const Module *m, uint32_t off, uint64_t n) { return off <= m->size && n <= m->size - off; }

static bool parse_dynamic(Module *m, uint32_t mod0) {
    if (!in_module(m, mod0, 8)) return false;
    uint32_t dyn_off = mod0 + LD32(m->base + mod0 + 4);
    if (!in_module(m, dyn_off, 8)) return false;
    m->dynamic = m->base + dyn_off;
    bool end = false;
    for (uint32_t off = dyn_off; in_module(m, off, 8); off += 8) {
        const Elf32Dyn *d = G2H(m->base + off);
        if (d->tag == DT_NULL) { end = true; break; }
        switch (d->tag) {  // (addresses: inside the image)
        case DT_SYMTAB: case DT_STRTAB: case DT_INIT: case DT_FINI: case DT_INIT_ARRAY: case DT_FINI_ARRAY:
            if (!in_module(m, d->val, 4)) return false;
            break;
        case DT_HASH:  // (nbucket, nchain)
            if (!in_module(m, d->val, 8)) return false;
        }
        switch (d->tag) {  // (an address of 0 is no address: DT_INIT 0 is no initializer)
        case DT_SYMTAB: m->symtab = m->base + d->val; break;
        case DT_STRTAB: m->strtab = m->base + d->val; break;
        case DT_STRSZ: m->strsz = d->val; break;
        case DT_HASH: m->nsyms = LD32(m->base + d->val + 4); break;
        case DT_INIT: if (d->val) m->init = m->base + d->val; break;
        case DT_FINI: if (d->val) m->fini = m->base + d->val; break;
        case DT_INIT_ARRAY: if (d->val) m->init_array = m->base + d->val; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d->val; break;
        case DT_FINI_ARRAY: if (d->val) m->fini_array = m->base + d->val; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d->val; break;
        }
    }
    // (DT_HASH's nchain is the symbol count: the symbol table has to hold that many entries)
    if (!end || (m->nsyms && !in_module(m, m->symtab - m->base, (uint64_t)m->nsyms * 16))) return false;
    // (the string table holds the names: a symbol's name offset is checked against its size when it is read)
    if (m->nsyms && (!m->strtab || !m->strsz || !in_module(m, m->strtab - m->base, m->strsz))) return false;
    if (m->init_array_sz && (!m->init_array || !in_module(m, m->init_array - m->base, m->init_array_sz))) return false;
    if (m->fini_array_sz && (!m->fini_array || !in_module(m, m->fini_array - m->base, m->fini_array_sz))) return false;
    return true;
}

static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    if (fread(p, 1, *len, f) != *len) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

// An NSO's size in memory (its segments and bss, page aligned); 0: not an NSO
static uint32_t nso_size(const char *path) {
    uint8_t h[0x40];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    bool ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "NSO0", 4);
    fclose(f);
    uint32_t data_off, data_size, bss;
    memcpy(&data_off, h + 0x34, 4), memcpy(&data_size, h + 0x38, 4), memcpy(&bss, h + 0x3C, 4);
    uint64_t size = ((uint64_t)data_off + data_size + bss + 0xFFF) & ~0xFFFull;
    return ok && size < 0x10000000u ? (uint32_t)size : 0;
}

// A mod's subsdk that is Forge's own module (its module name, at the start of rodata: u32 0, u32 length, the name):
// Forge's loader, which mods with Forge plugins bring along (on the console it loads the plugins). The port runs the
// plugins itself (runtime/forge.c), as before mods' subsdks were loaded: it is passed over.
static bool forge_module(const char *path, const char *mod) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    bool forge = false;
    if (raw && len >= 0x100 && !memcmp(raw, "NSO0", 4)) {
        uint32_t flags, file_off, size, csize;
        memcpy(&flags, raw + 0xC, 4), memcpy(&file_off, raw + 0x20, 4), memcpy(&size, raw + 0x28, 4);
        memcpy(&csize, raw + 0x64, 4);
        uint8_t *ro = size >= 0x10 && size < 0x10000000u ? malloc(size) : NULL;
        bool ok = ro && (uint64_t)file_off + csize <= len;
        if (ok && (flags & 2)) ok = lz4_decompress(raw + file_off, csize, ro, size) == size;
        else if (ok) ok = csize >= size && (memcpy(ro, raw + file_off, size), true);
        uint32_t n = 0;
        if (ok) memcpy(&n, ro + 4, 4);
        if (ok && n >= 5 && n <= size - 8) {
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)n, (const char *)ro + 8);
            forge = !strcasecmp(name, "mhguForge");
        }
        free(ro);
    }
    free(raw);
    if (forge) LOG("[mods] %s: exefs/%s is Forge's own module: not loaded (the port runs Forge plugins itself)\n", mod,
                   strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    return forge;
}

static void load_nso(Module *m, const char *path, uint32_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || len < 0x100 || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint8_t build_id[32];
    memcpy(build_id, raw + 0x40, 32);
    uint32_t size = (seg[2][1] + seg[2][2] + bss + 0xFFF) & ~0xFFFu;
    plat_commit(GUEST_BASE + base, size);
    memset(G2H(base), 0, size);
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        if ((uint64_t)seg[i][0] + csize > len || (uint64_t)seg[i][1] + seg[i][2] > size)
            FATAL("%s: segment %d is outside the file or the image\n", path, i);
        uint8_t *dst = (uint8_t *)G2H(base + seg[i][1]);
        if (flags & (1 << i)) {
            size_t got = lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;
    m->bss_size = bss;

    if (!parse_dynamic(m, LD32(base + 4))) FATAL("%s: MOD0 or the dynamic section is outside the image\n", path);
    LOG("loaded %-5s at %08x size %08x syms %u\n", m->name, base, size, m->nsyms);
    mods_patch_module(m, build_id);  // (before relocation, as on the console)
}

static const Elf32Sym *sym(const Module *m, uint32_t i) { return (const Elf32Sym *)G2H(m->symtab) + i; }
// a symbol's name, "" when the name is not inside the string table or not terminated there (a damaged module)
static const char *symname(const Module *m, const Elf32Sym *s) {
    if (s->name >= m->strsz) return "";
    const char *p = G2H(m->strtab + s->name);
    return memchr(p, 0, m->strsz - s->name) ? p : "";
}

uint32_t loader_find_symbol(const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint32_t i = 1; i < m->nsyms; i++) {
            const Elf32Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint32_t addr, uint32_t *offset) {
    const char *best = NULL;
    uint32_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint32_t i = 1; i < m->nsyms; i++) {
            const Elf32Sym *s = sym(m, i);
            uint32_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

// plugin: a module loaded at run time, whose imports the log names when nothing provides them (calls return 0)
static uint32_t resolve(Module *m, uint32_t symidx, bool plugin) {
    const Elf32Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) return m->base + s->value;
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // a host implementation wins over the guest SDK's
    uint32_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    if (plugin) LOG("[forge] [WARN] %s: import %s is not provided (calls return 0)\n", m->name, name);
    return hle_register(name, NULL);
}

// false: a plugin this port can't relocate (logged); the game's own modules can't fail (FATAL)
static bool relocate(Module *m, bool plugin) {
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltsz = 0, pltrel = DT_REL;
    for (const Elf32Dyn *d = G2H(m->dynamic); d->tag != DT_NULL; d++) {
        if (d->tag == DT_REL) rel = m->base + d->val;
        if (d->tag == DT_RELSZ) relsz = d->val;
        if (d->tag == DT_JMPREL) jmprel = m->base + d->val;
        if (d->tag == DT_PLTRELSZ) pltsz = d->val;
        if (d->tag == DT_PLTREL) pltrel = d->val;
        if (d->tag == DT_RELA) {
            if (!plugin) FATAL("%s: RELA relocations in an AArch32 module\n", m->name);
            LOG("[loader] %s: RELA relocations in an AArch32 module: not loaded\n", m->name);
            return false;
        }
    }
    if (pltrel != DT_REL) {
        if (!plugin) FATAL("%s: PLT uses RELA\n", m->name);
        LOG("[loader] %s: PLT uses RELA: not loaded\n", m->name);
        return false;
    }
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t tab = pass ? jmprel : rel, n = (pass ? pltsz : relsz) / 8;
        if (n && !in_module(m, tab - m->base, (uint64_t)n * 8)) {
            if (!plugin) FATAL("%s: relocation table outside the module\n", m->name);
            LOG("[loader] %s: relocation table outside the module: not loaded\n", m->name);
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t off = LD32(tab + 8 * i), info = LD32(tab + 8 * i + 4);
            uint32_t where = m->base + off, type = info & 0xFF, si = info >> 8;
            if (type != R_ARM_NONE && !in_module(m, off, 4)) {
                if (!plugin) FATAL("%s: relocation at %08x outside the module\n", m->name, off);
                LOG("[loader] %s: relocation at %08x is outside the module: not loaded\n", m->name, off);
                return false;
            }
            bool needs_sym = type == R_ARM_ABS32 || type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT ||
                             (type == R_ARM_TLS_TPOFF32 && si);
            if (needs_sym && si >= m->nsyms) {
                if (!plugin) FATAL("%s: relocation of symbol %u of %u\n", m->name, si, m->nsyms);
                LOG("[loader] %s: relocation of symbol %u, there are %u: not loaded\n", m->name, si, m->nsyms);
                return false;
            }
            switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: {  // (a mod's subsdk elsewhere than expected: an address outside its own image is
                // one of main's, from where the game's layout puts the subsdk: see loader_load)
                uint32_t v = LD32(where);
                ST32(where, v + (m->expected && v > m->size ? m->expected : m->base));
                break;
            }
            case R_ARM_ABS32: {
                uint32_t s = resolve(m, si, plugin);
                ST32(where, s ? s + LD32(where) : 0);
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                uint32_t s = resolve(m, si, plugin);
                ST32(where, s);
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            case R_ARM_TLS_TPOFF32: {  // initial-exec __thread data: the variable's offset from the thread pointer
                // (the module's block is after the TCB; the variable's offset in it is the symbol's value, or the
                // addend for a local one. A variable of another module is not ours to place.)
                const Elf32Sym *s = si ? sym(m, si) : NULL;
                if (!plugin || (s && !s->shndx)) {
                    if (!plugin) FATAL("%s: TLS relocation\n", m->name);
                    LOG("[loader] %s: __thread data of another module (offset %08x) is not supported: not loaded\n", m->name, off);
                    return false;
                }
                if (!m->tls_size) {
                    if (tls_next + TLS_MODULE_SIZE > TLS_TOTAL_MAX) {
                        LOG("[loader] %s: no room left for its __thread data: not loaded\n", m->name);
                        return false;
                    }
                    m->tls_off = tls_next, m->tls_size = TLS_MODULE_SIZE;
                    tls_next += TLS_MODULE_SIZE;
                }
                uint32_t v = (s ? s->value : 0) + LD32(where);
                if (v >= m->tls_size) {
                    LOG("[loader] %s: a __thread variable at %x is past the %x bytes kept for them: not loaded\n", m->name, v, m->tls_size);
                    return false;
                }
                ST32(where, m->tls_off + v);
                break;
            }
            default:
                if (!plugin) FATAL("%s: reloc type %u\n", m->name, type);
                LOG("[loader] %s: relocation type %u (offset %08x) is not supported: not loaded\n", m->name, type, off);
                return false;
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host functions)\n", m->name, unresolved);
    return true;
}

bool loader_moved(uint32_t addr, uint32_t slack, int32_t *shift) {
    for (int i = 2; i < g_nexefs; i++) {
        const Module *m = &g_modules[i];
        if (m->expected && addr + slack >= m->expected && addr < m->expected + m->size + slack) {
            *shift = (int32_t)(m->base - m->expected);
            return true;
        }
    }
    return false;
}

// Where a mod's subsdk goes. On the console and in emulators the modules sit one after another: main, the subsdks
// in order, sdk. A subsdk built for that finds main from its own address (MH4U Port: its base - a fixed gap), and
// the mod's patches of main jump into it there: that address is the expected one (after main and every subsdk
// before it, the game's own included, though this port runs none of those). Here sdk is at SDK_BASE, which can be
// on it: then the subsdk goes in the room between sdk and HLE_BASE instead, at a whole multiple of 256 KiB from the
// expected address (so the mod's `add pc, pc, #imm` jumps into it, multiples of that, still encode: runtime/mods.c),
// and its addresses outside its own image are relocated as if it were where expected (main's), mods.c moves the
// patches' jumps and offsets into it by the difference.
static uint32_t place_subsdk(uint32_t expected, uint32_t size, uint32_t lo) {
    uint32_t b = expected;
    while (b < lo) b += 0x40000;
    return (uint64_t)b + size <= HLE_BASE ? b : 0;
}

void loader_load(const char *exefs) {
    char path[1100];
    static const struct { const char *name; uint32_t base; } mods[] = {{"main", MAIN_BASE}, {"sdk", SDK_BASE}};
    for (unsigned i = 0; i < 2; i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, mods[i].base);
    }
    // the mods' subsdks (runtime/mods.h), in their order: the game's own (subsdk0) only take their room in the layout
    uint32_t expected = MAIN_BASE + g_modules[0].size, lo = SDK_BASE + g_modules[1].size;
    for (int k = 0; k < 10; k++) {
        char name[16];
        snprintf(name, sizeof name, "subsdk%d", k);
        const char *mod = mods_exefs_file(name, path, sizeof path, forge_module);
        if (!mod) {
            snprintf(path, sizeof path, "%s/%s", exefs, name);
            expected += nso_size(path);
            continue;
        }
        uint32_t size = nso_size(path), base = size ? place_subsdk(expected, size, lo) : 0;
        if (!size) FATAL("mod %s: exefs/%s is not an NSO (remove the mod, or turn it off in the Mods tab)\n", mod, name);
        if (!base || g_nmodules == MAX_MODULES)
            FATAL("mod %s: no room for exefs/%s (%#x bytes) in the code region\n", mod, name, size);
        Module *m = &g_modules[g_nmodules++];
        m->name = strdup(name);
        m->mod = mod;
        if (base != expected) m->expected = expected;
        if (base != expected)
            LOG("[mods] %s: %s at %08x (the game's layout puts it at %08x, where this port has sdk: its addresses "
                "outside itself are relocated for %08x)\n", mod, name, base, expected, expected);
        else
            LOG("[mods] %s: %s at %08x\n", mod, name, base);
        load_nso(m, path, base);
        expected += size;
        lo = base + size;
    }
    g_nexefs = g_nmodules;
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i], false);
    for (int i = 2; i < g_nexefs; i++) {  // (a subsdk's code runs in the interpreter, as Forge plugins' does)
        Module *m = &g_modules[i];
        interp_add_range(m->base + m->text_off, m->base + m->text_off + m->text_size);
    }
}

uint32_t loader_module_symbol(const Module *m, const char *name) {
    for (uint32_t i = 1; i < m->nsyms; i++) {
        const Elf32Sym *s = sym(m, i);
        if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
    }
    return 0;
}

// NRO: a flat image (file offsets are memory offsets) with the segment table at 0x20 and the bss size at 0x38
Module *loader_load_nro(const char *name, const uint8_t *data, size_t len, uint32_t base) {
    if (g_nmodules == MAX_MODULES) return NULL;
    if (len < 0x80 || memcmp(data + 0x10, "NRO0", 4)) return NULL;
    uint32_t seg[3][2], bss, mod0;
    memcpy(seg, data + 0x20, sizeof seg);
    memcpy(&bss, data + 0x38, 4);
    memcpy(&mod0, data + 4, 4);
    // the segments are text, rodata, data in order, page aligned, inside the file's image, with the bss after it
    uint64_t image64 = (uint64_t)seg[2][0] + seg[2][1];
    bool ok = image64 <= len && image64 + bss + 0xFFF < 0x10000000u && (uint64_t)mod0 + 8 <= image64;
    for (int i = 0; ok && i < 3; i++)
        ok = !(seg[i][0] & 0xFFF) && (uint64_t)seg[i][0] + seg[i][1] <= image64 && (i == 0 || seg[i][0] >= seg[i - 1][0] + seg[i - 1][1]);
    if (!ok || memcmp(data + mod0, "MOD0", 4)) {
        LOG("[loader] %s: segment table or MOD0 is not valid: not loaded\n", name);
        return NULL;
    }
    uint32_t image = (uint32_t)image64;
    Module *m = &g_modules[g_nmodules];
    memset(m, 0, sizeof *m);
    m->name = strdup(name);
    m->base = base;
    m->size = (image + bss + 0xFFF) & ~0xFFFu;
    plat_commit(GUEST_BASE + base, m->size);
    memset(G2H(base), 0, m->size);
    memcpy(G2H(base), data, image);
    m->text_off = seg[0][0]; m->text_size = seg[0][1];
    m->ro_off = seg[1][0];   m->ro_size = seg[1][1];
    m->data_off = seg[2][0]; m->data_size = seg[2][1] + bss;
    m->bss_size = bss;
    if (!parse_dynamic(m, mod0)) {
        LOG("[loader] %s: MOD0 or the dynamic section is outside the image: not loaded\n", name);
        free((char *)m->name);
        return NULL;
    }
    g_nmodules++;
    LOG("loaded %s at %08x size %08x syms %u\n", m->name, base, m->size, m->nsyms);
    uint32_t tls_saved = tls_next;
    if (!relocate(m, true)) {  // (nothing is bound to it yet: it just goes)
        tls_next = tls_saved;
        g_nmodules--;
        free((char *)m->name);
        return NULL;
    }
    return m;
}
