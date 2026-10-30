// Cheat codes in Atmosphere's format (the one Ryujinx and a modded Switch use): every .txt file in cheats/ (beside
// mods/; MHGU_CHEATS_DIR overrides), in name order; a file whose name starts with '_' is skipped. A file holds any
// number of cheats: "[Name]" starts one, "{Name}" a master code (run before the others), then its code words, 8 hex
// digits each. Lines starting with '#' or "//" are comments. Every cheat found is on.
// The codes run on Atmosphere's cheat virtual machine (every code type of its docs/features/cheats.md: written from
// that description), once before the game starts and then once per frame the game presents, from the window loop
// (as Atmosphere runs them beside the game). Memory regions: main is the main module (0x200000), heap, alias and aslr
// the regions GetInfo reports to the game; an address that is not mapped reads as 0 and is not written.
// [toggle patch] Cheats can be switched on and off while the game runs (cheats_set_enabled, for the F1 menu). Each cheat
// keeps a journal of every byte it wrote (the byte before, and the byte it wrote last); switching it off puts back the
// bytes that still hold what the cheat wrote and leaves the others alone (the game has reused that memory). A file whose
// name starts with '_' is loaded but off; cheats/cheats_state.ini remembers what the menu switched.
// A write into the game's code: the port runs code compiled ahead of time, which never sees it, so the function
// holding the changed word runs in the interpreter from then on (dispatch_interpret: that function alone, at the
// interpreter's speed); words written into the room after the code (padding up to the read-only data, where cheats
// put new code) are interpreted when the changed code jumps there. A call already running when the code changes
// finishes the old code.
#include <ctype.h>
#include <stdatomic.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cheats_ui.h"
#include "hle/input.h"
#include "interp.h"
#include "mods.h"
#include "rt.h"

#define NIB(w, i) (((w) >> (28 - 4 * (i))) & 15u)
#define MAX_OPS   1024
#define BUDGET    (1 << 20)  // instructions per cheat per run (a loop that never ends is cut off)

typedef struct {
    uint16_t type;  // 0x0-0xA: the top nybble; 0xC0-0xC4: the top byte; 0xFF0, 0xFF1, 0xFFF
    uint8_t n;      // words
    bool latched;   // (0xC4 without auto-repeat: its keys are still held since it ran)
    int jump;       // a block's begin or else: the op after its else / end; a loop's start: after its end, its end:
                    // after its start
    uint32_t w[5];
} Op;

typedef struct {  // one byte written by a cheat: the byte before it, and the byte it wrote last
    uint32_t a;
    uint8_t orig, last, used;
} JEnt;

typedef struct {
    char name[96], file[64];
    bool master;
    bool enabled;
    Op *ops;
    int nops;
    JEnt *jt;  // the journal: open-addressing table, jcap a power of two
    uint32_t jcap, jcount;
} Cheat;

static Cheat *cheats;
static int ncheats;
static uint64_t reg[16], saved[16], statics[128];
static uint64_t keys;
static Cheat *cur;  // the one running (for the log, and its journal)

// The menu (cheats_set_enabled...) may run on another thread than the window loop (cheats_frame): one lock around
// everything that runs or changes a cheat. The critical sections are short.
static atomic_flag lock = ATOMIC_FLAG_INIT;
static void lock_cheats(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void unlock_cheats(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

const char *cheats_dir(void) {
    const char *e = getenv("MHGU_CHEATS_DIR");
    return e && *e ? e : "cheats";
}

void cheats_make_dir(void) {
    struct stat st;
    if (stat(cheats_dir(), &st)) mkdir(cheats_dir(), 0755);
}

// ------------------------------------------------------------------ decoding
static bool width_ok(uint32_t t) { return t == 1 || t == 2 || t == 4 || t == 8; }

static uint16_t op_type(uint32_t w) {
    uint32_t t = w >> 28;
    if (t < 0xC) return (uint16_t)t;
    if (t == 0xF && (w >> 24) == 0xFF) return (uint16_t)(w >> 20);
    return (uint16_t)(w >> 24);
}

// The op's word count from its first word (0: not a valid op).
static int op_words(uint32_t w) {
    switch (op_type(w)) {
    case 0x0: case 0x1: return width_ok(NIB(w, 1)) && NIB(w, 2) <= 4 ? (NIB(w, 1) == 8 ? 4 : 3) : 0;
    case 0x2: return NIB(w, 1) <= 1;
    case 0x3: return NIB(w, 1) == 0 ? 2 : NIB(w, 1) == 1;
    case 0x4: return 3;
    case 0x5: return width_ok(NIB(w, 1)) && NIB(w, 2) <= 4 && NIB(w, 4) <= 3 ? 2 : 0;
    case 0x6: return width_ok(NIB(w, 1)) ? 3 : 0;
    case 0x7: return width_ok(NIB(w, 1)) && NIB(w, 4) <= 4 ? 2 : 0;
    case 0x8: return 1;
    case 0x9:
        if (!width_ok(NIB(w, 1)) || NIB(w, 2) > 13 || (NIB(w, 2) >= 10 && NIB(w, 1) < 4)) return 0;
        return NIB(w, 5) == 1 ? (NIB(w, 1) == 8 ? 3 : 2) : NIB(w, 5) == 0;
    case 0xA:
        if (!width_ok(NIB(w, 1)) || NIB(w, 5) > 5) return 0;
        return NIB(w, 5) == 2 || NIB(w, 5) == 4 || NIB(w, 5) == 5 ? 2 : 1;
    case 0xC0:
        if (!width_ok(NIB(w, 2)) || NIB(w, 3) < 1 || NIB(w, 3) > 6 || NIB(w, 5) > 5) return 0;
        if (NIB(w, 5) == 0 || NIB(w, 5) == 2) return 2;
        return NIB(w, 5) == 4 ? (NIB(w, 2) == 8 ? 3 : 2) : 1;
    case 0xC1: return NIB(w, 6) <= 3;
    case 0xC2: return NIB(w, 2) <= 3;
    case 0xC3: return 1;
    case 0xC4: return 3;
    case 0xFF0: case 0xFF1: return 1;
    case 0xFFF:
        if (!width_ok(NIB(w, 3)) || NIB(w, 5) > 4) return 0;
        return NIB(w, 5) == 0 || NIB(w, 5) == 2 ? 2 : 1;
    }
    return 0;
}

static bool cond_ok(const Op *o) {  // (the condition nybble of 0x1)
    return o->type != 0x1 || (NIB(o->w[0], 3) >= 1 && NIB(o->w[0], 3) <= 6 && NIB(o->w[0], 4) <= 1);
}

// Words -> ops, with the blocks and loops matched. NULL (why in *err) if it does not decode.
static const char *decode(Cheat *ch, const uint32_t *w, int n) {
    ch->ops = calloc(MAX_OPS, sizeof(Op));
    int stack[64], depth = 0;
    for (int i = 0; i < n;) {
        int k = op_words(w[i]);
        if (!k) return "an unknown or invalid code";
        if (i + k > n) return "a code is cut short";
        if (ch->nops == MAX_OPS) return "too many codes";
        Op *o = &ch->ops[ch->nops];
        o->type = op_type(w[i]);
        o->n = (uint8_t)k;
        memcpy(o->w, w + i, k * 4);
        o->jump = -1;
        if (!cond_ok(o)) return "an invalid condition";
        bool begin = o->type == 0x1 || o->type == 0x8 || o->type == 0xC0 || o->type == 0xC4 ||
                     (o->type == 0x3 && NIB(o->w[0], 1) == 0);
        if (begin) {
            if (depth == 64) return "blocks nested too deep";
            stack[depth++] = ch->nops;
        } else if (o->type == 0x2 || o->type == 0x3) {  // else / end / end of loop
            bool loop_end = o->type == 0x3;
            if (!depth) return loop_end ? "an end of loop without its start" : "an end of block without its start";
            Op *b = &ch->ops[stack[depth - 1]];
            if ((b->type == 0x3) != loop_end) return "a block and a loop overlap";
            if (loop_end && NIB(b->w[0], 3) != NIB(o->w[0], 3)) return "a loop ends on another register";
            b->jump = ch->nops + 1;
            if (loop_end) o->jump = stack[depth - 1] + 1;
            if (o->type == 0x2 && NIB(o->w[0], 1) == 1) {  // else: the block goes on to its end
                if (b->type == 0x2) return "two elses in one block";
                stack[depth - 1] = ch->nops;
            } else {
                depth--;
            }
        }
        ch->nops++;
        i += k;
    }
    while (depth) ch->ops[stack[--depth]].jump = ch->nops;  // (open at the end: they end with the cheat)
    return NULL;
}

// ------------------------------------------------------------------ loading
static bool load_off;  // the file being loaded starts with '_': its cheats are loaded but off

static void add_cheat(const char *file, const char *name, bool master, const uint32_t *w, int n, const char *bad) {
    Cheat ch = {0};
    ch.enabled = !load_off;
    snprintf(ch.name, sizeof ch.name, "%s", name);
    snprintf(ch.file, sizeof ch.file, "%s", file);
    ch.master = master;
    const char *err = bad ? bad : decode(&ch, w, n);
    if (err) {
        LOG("[cheats] %s: [%s]: %s, the cheat is left out\n", file, name, err);
        free(ch.ops);
        return;
    }
    cheats = realloc(cheats, (ncheats + 1) * sizeof(Cheat));
    cheats[ncheats++] = ch;
}

static void load_file(const char *path, const char *file) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(len + 1);
    len = (long)fread(s, 1, len, f);
    s[len] = 0;
    fclose(f);
    static uint32_t words[MAX_OPS * 4];
    int n = 0, before = ncheats;
    char name[96] = "";
    bool open = false, master = false, stray = false;
    const char *bad = NULL;
    for (char *p = s; *p;) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (*p == '#' || (p[0] == '/' && p[1] == '/')) { while (*p && *p != '\n') p++; continue; }
        if (*p == '[' || *p == '{') {
            char close = *p == '[' ? ']' : '}';
            char *e = strchr(p + 1, close);
            if (!e) { LOG("[cheats] %s: a name without its '%c'\n", file, close); break; }
            if (open) add_cheat(file, name, master, words, n, bad);
            snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
            master = *p == '{', open = true, n = 0, bad = NULL;
            p = e + 1;
            continue;
        }
        char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        bool hex = e - p == 8;
        for (char *q = p; hex && q < e; q++) hex = isxdigit((unsigned char)*q);
        if (!open) stray = true;
        else if (!hex) bad = bad ? bad : "something that is not a code word (8 hex digits)";
        else if (n < (int)(sizeof words / 4)) words[n++] = (uint32_t)strtoul(p, NULL, 16);
        else bad = "too many codes";
        p = e;
    }
    if (open) add_cheat(file, name, master, words, n, bad);
    if (stray) LOG("[cheats] %s: codes before the first [name] are left out\n", file);
    LOG("[cheats] %s: %d cheat(s)", file, ncheats - before);
    for (int i = before; i < ncheats; i++) LOG("%s%s%s%s", i == before ? ": " : ", ", cheats[i].master ? "{" : "",
                                             cheats[i].name, cheats[i].master ? "}" : "");
    LOG("\n");
    free(s);
}

static int cmp_name(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

// ------------------------------------------------------------------ memory
static uint64_t region(uint32_t m) {
    switch (m) {
    case 0: return MAIN_BASE;
    case 1: return HEAP_BASE;
    case 2: return ALIAS_BASE;
    case 3: return MAIN_BASE;  // (the address space: GetInfo's ASLR region)
    default: return 0;         // 4: not relative
    }
}

static uint64_t mask(uint32_t t, uint64_t v) { return t == 8 ? v : v & ((1ull << (8 * t)) - 1); }

static uint64_t rd(uint64_t a, uint32_t t) {
    uint64_t v = 0;
    if (kernel_mapped(a, t)) memcpy(&v, G2H((uint32_t)a), t);
    return v;
}

// A changed word of the game's code: the function holding it runs in the interpreter from now on.
static void code_changed(uint32_t a) {
    const Module *m = &g_modules[a >= SDK_BASE];
    if (a >= m->base + m->text_off + m->text_size) {  // after the code (padding up to the read-only data): new code
        interp_add_range(a, a + 4);
        return;
    }
    uint32_t next, entry = dispatch_function_at(a, &next);
    if (!entry) return;
    uint32_t end = next ? next : m->base + m->text_off + m->text_size;
    int r = dispatch_interpret(entry, end);
    if (r > 0) LOG("[cheats] %s: changes the code at %s+%#x: the function at %s+%#x runs in the interpreter now\n",
                   cur->name, m->name, a - m->base, m->name, entry - m->base);
    else if (r < 0) LOG("[cheats] %s: changes the code at %s+%#x, which cannot take effect\n", cur->name, m->name,
                        a - m->base);
}

static bool in_code(uint64_t a, uint32_t t) {  // does [a, a+t) touch the game's code?
    for (int i = 0; i < 2 && i < g_nmodules; i++) {
        const Module *m = &g_modules[i];
        if (a < m->base + m->ro_off && a + t > m->base + m->text_off) return true;
    }
    return false;
}

// ---- the journal of a cheat (what it needs to be switched off)
static uint32_t jhash(uint32_t a) { return a * 2654435761u; }

static void jgrow(Cheat *c) {
    uint32_t cap = c->jcap ? c->jcap * 2 : 64;
    JEnt *t = calloc(cap, sizeof(JEnt));
    for (uint32_t i = 0; i < c->jcap; i++) {
        if (!c->jt[i].used) continue;
        uint32_t k = jhash(c->jt[i].a) & (cap - 1);
        while (t[k].used) k = (k + 1) & (cap - 1);
        t[k] = c->jt[i];
    }
    free(c->jt);
    c->jt = t;
    c->jcap = cap;
}

static void jnote(Cheat *c, uint32_t a, uint8_t nb, uint8_t before) {
    if ((c->jcount + 1) * 10 >= c->jcap * 7) jgrow(c);
    uint32_t k = jhash(a) & (c->jcap - 1);
    while (c->jt[k].used && c->jt[k].a != a) k = (k + 1) & (c->jcap - 1);
    JEnt *e = &c->jt[k];
    if (!e->used) *e = (JEnt){a, before, nb, 1}, c->jcount++;  // the first write: the byte as the game had it
    else e->last = nb;
}

// Bytes the game has already rewritten (an object that is gone...) are not worth keeping.
static void jprune(Cheat *c) {
    JEnt *old = c->jt;
    uint32_t n = c->jcap;
    c->jt = NULL;
    c->jcap = c->jcount = 0;
    for (uint32_t i = 0; i < n; i++)
        if (old[i].used && kernel_mapped(old[i].a, 1) && *(uint8_t *)G2H(old[i].a) == old[i].last)
            jnote(c, old[i].a, old[i].last, old[i].orig);
    free(old);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// Switch a cheat off: every byte that still holds what the cheat wrote gets its old value back.
static void revert(Cheat *c) {
    uint32_t *words = NULL;
    int nw = 0, cap = 0;
    cur = c;
    for (uint32_t i = 0; i < c->jcap; i++) {
        const JEnt *e = &c->jt[i];
        if (!e->used || !kernel_mapped(e->a, 1)) continue;
        uint8_t *p = G2H(e->a);
        if (*p != e->last || e->last == e->orig) continue;  // changed by the game since, or nothing to undo
        *p = e->orig;
        if (in_code(e->a, 1)) {
            if (nw == cap) words = realloc(words, (cap = cap ? cap * 2 : 16) * sizeof *words);
            words[nw++] = e->a & ~3u;
        }
    }
    free(c->jt);
    c->jt = NULL;
    c->jcap = c->jcount = 0;
    if (nw) {  // the interpreter must see the code as it is again
        qsort(words, nw, sizeof *words, cmp_u32);
        for (int i = 0; i < nw; i++)
            if (!i || words[i] != words[i - 1]) code_changed(words[i]);
    }
    free(words);
}

static void wr(uint64_t a, uint32_t t, uint64_t v) {
    if (!kernel_mapped(a, t)) return;
    uint8_t *p = G2H((uint32_t)a);
    bool code = in_code(a, t);
    for (uint32_t i = 0; i < t; i++) jnote(cur, (uint32_t)a + i, ((uint8_t *)&v)[i], p[i]);
    if (code && !memcmp(p, &v, t)) return;
    memcpy(p, &v, t);
    if (code)
        for (uint32_t w = (uint32_t)a & ~3u; w < a + t; w += 4) code_changed(w);
}

// ------------------------------------------------------------------ running
static bool compare(uint32_t c, uint64_t a, uint64_t b) {
    switch (c) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    default: return a != b;
    }
}

static uint64_t arith(uint32_t op, uint32_t t, uint64_t a, uint64_t b) {
    if (op >= 10) {  // floats: T 4 single, 8 double
        if (t == 4) {
            float x, y, r;
            uint32_t xa = (uint32_t)a, xb = (uint32_t)b, out;
            memcpy(&x, &xa, 4), memcpy(&y, &xb, 4);
            r = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            memcpy(&out, &r, 4);
            return out;
        }
        double x, y, r;
        memcpy(&x, &a, 8), memcpy(&y, &b, 8);
        r = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
        uint64_t out;
        memcpy(&out, &r, 8);
        return out;
    }
    uint64_t r;
    switch (op) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = a * b; break;
    case 3: r = b < 64 ? a << b : 0; break;
    case 4: r = b < 64 ? a >> b : 0; break;
    case 5: r = a & b; break;
    case 6: r = a | b; break;
    case 7: r = ~a; break;
    case 8: r = a ^ b; break;
    default: r = a; break;  // 9: move
    }
    return mask(t, r);
}

static uint64_t imm(const Op *o, int i, uint32_t t) {  // a value word (two for width 8: high, low)
    return t == 8 ? (uint64_t)o->w[i] << 32 | o->w[i + 1] : o->w[i];
}

static uint64_t addr40(const Op *o) { return (uint64_t)(o->w[0] & 0xFF) << 32 | o->w[1]; }
static uint64_t addr36(const Op *o) { return (uint64_t)(o->w[0] & 0xF) << 32 | o->w[1]; }

// Each cheat starts with its registers and saved registers at 0 (as Ryujinx runs them, each on its own): a cheat
// that leaves a register set must not move the next one's writes, which add a register to their address (0TMR...:
// R, mostly 0). Static registers are shared: they are how cheats pass values on.
static void run(Cheat *ch) {
    cur = ch;
    memset(reg, 0, sizeof reg);
    memset(saved, 0, sizeof saved);
    int budget = BUDGET;
    for (int i = 0; i < ch->nops && budget--;) {
        Op *o = &ch->ops[i];
        uint32_t w = o->w[0];
        int next = i + 1;
        switch (o->type) {
        case 0x0: {  // store a value: 0TMR00AA AAAAAAAA V
            uint32_t t = NIB(w, 1);
            wr(region(NIB(w, 2)) + addr40(o) + reg[NIB(w, 3)], t, mask(t, imm(o, 2, t)));
            break;
        }
        case 0x1: {  // memory condition: 1TMCXrAA AAAAAAAA V
            uint32_t t = NIB(w, 1);
            uint64_t a = region(NIB(w, 2)) + addr40(o) + (NIB(w, 4) ? reg[NIB(w, 5)] : 0);
            if (!compare(NIB(w, 3), rd(a, t), mask(t, imm(o, 2, t)))) next = o->jump;
            break;
        }
        case 0x2:  // end, or else reached at the end of the taken part: on past the block
            if (NIB(w, 1) == 1) next = o->jump;
            break;
        case 0x3:
            if (NIB(w, 1) == 0) {  // loop start: 300R0000 V
                reg[NIB(w, 3)] = o->w[1];
                if (!o->w[1]) next = o->jump;
            } else if (reg[NIB(w, 3)] && --reg[NIB(w, 3)]) {  // loop end: 310R0000
                next = o->jump;
            }
            break;
        case 0x4:  // load a value: 400R0000 V V
            reg[NIB(w, 3)] = (uint64_t)o->w[1] << 32 | o->w[2];
            break;
        case 0x5: {  // load from memory: 5TMRIS AA AAAAAAAA
            uint32_t t = NIB(w, 1), r = NIB(w, 3), s = NIB(w, 5);
            uint64_t a = addr40(o);
            switch (NIB(w, 4)) {
            case 0: a += region(NIB(w, 2)); break;
            case 1: a += reg[r]; break;
            case 2: a += reg[s]; break;
            default: a += region(NIB(w, 2)) + reg[s]; break;
            }
            reg[r] = rd(a, t);
            break;
        }
        case 0x6: {  // store a value at a register's address: 6T0RIor0 V V
            uint32_t t = NIB(w, 1), r = NIB(w, 3);
            wr(reg[r] + (NIB(w, 5) ? reg[NIB(w, 6)] : 0), t, mask(t, (uint64_t)o->w[1] << 32 | o->w[2]));
            if (NIB(w, 4)) reg[r] += t;
            break;
        }
        case 0x7: {  // legacy arithmetic: 7T0RC000 V
            uint32_t t = NIB(w, 1), r = NIB(w, 3);
            reg[r] = arith(NIB(w, 4), t, reg[r], o->w[1]);
            break;
        }
        case 0x8: {  // keys held: 8kkkkkkk
            uint64_t k = w & 0x0FFFFFFFu;
            if ((keys & k) != k) next = o->jump;
            break;
        }
        case 0x9: {  // arithmetic: 9TCRS0s0 / 9TCRS100 V
            uint32_t t = NIB(w, 1);
            uint64_t b = NIB(w, 5) ? imm(o, 1, t) : reg[NIB(w, 6)];
            reg[NIB(w, 3)] = arith(NIB(w, 2), t, mask(t, reg[NIB(w, 4)]), mask(t, b));
            break;
        }
        case 0xA: {  // store a register: ATSRIOxa (aaaaaaaa)
            uint32_t t = NIB(w, 1), r = NIB(w, 3), x = NIB(w, 6);
            uint64_t a;
            switch (NIB(w, 5)) {
            case 0: a = reg[r]; break;
            case 1: a = reg[r] + reg[x]; break;
            case 2: a = reg[r] + addr36(o); break;
            case 3: a = region(x) + reg[r]; break;
            case 4: a = region(x) + addr36(o); break;
            default: a = region(x) + addr36(o) + reg[r]; break;
            }
            wr(a, t, mask(t, reg[NIB(w, 2)]));
            if (NIB(w, 4)) reg[r] += t;
            break;
        }
        case 0xC0: {  // register condition: C0TcSX..
            uint32_t t = NIB(w, 2), x6 = NIB(w, 6), x7 = NIB(w, 7);
            uint64_t b;
            switch (NIB(w, 5)) {
            case 0: b = rd(region(x6) + addr36(o), t); break;
            case 1: b = rd(region(x6) + reg[x7], t); break;
            case 2: b = rd(reg[x6] + addr36(o), t); break;
            case 3: b = rd(reg[x6] + reg[x7], t); break;
            case 4: b = imm(o, 1, t); break;
            default: b = reg[x6]; break;
            }
            if (!compare(NIB(w, 3), mask(t, reg[NIB(w, 4)]), mask(t, b))) next = o->jump;
            break;
        }
        case 0xC1: {  // save / restore a register: C10D0Sx0
            uint32_t d = NIB(w, 3), s = NIB(w, 5);
            switch (NIB(w, 6)) {
            case 0: reg[d] = saved[s]; break;
            case 1: saved[d] = reg[s]; break;
            case 2: saved[d] = 0; break;
            default: reg[d] = 0; break;
            }
            break;
        }
        case 0xC2:  // the same by a mask: C2x0XXXX
            for (int k = 0; k < 16; k++) {
                if (!(w >> k & 1)) continue;
                switch (NIB(w, 2)) {
                case 0: reg[k] = saved[k]; break;
                case 1: saved[k] = reg[k]; break;
                case 2: saved[k] = 0; break;
                default: reg[k] = 0; break;
                }
            }
            break;
        case 0xC3: {  // static register: C3000XXx (XX 0x00-0x7F read, 0x80-0xFF write)
            uint32_t s = (w >> 4) & 0xFF;
            if (s < 0x80) reg[w & 15] = statics[s];
            else statics[s & 0x7F] = reg[w & 15];
            break;
        }
        case 0xC4: {  // keys held, extended: C4r00000 K K (r 0: once per press)
            uint64_t k = (uint64_t)o->w[1] << 32 | o->w[2];
            bool held = (keys & k) == k, fire = held && (NIB(w, 2) || !o->latched);
            o->latched = held;
            if (!fire) next = o->jump;
            break;
        }
        case 0xFF0: case 0xFF1: {  // pause / resume the game: not done here
            static bool said;
            if (!said) said = true, LOG("[cheats] %s: pausing / resuming the game is not supported\n", ch->name);
            break;
        }
        case 0xFFF: {  // debug log: FFFTIX..
            uint32_t t = NIB(w, 3), x6 = NIB(w, 6), x7 = NIB(w, 7);
            uint64_t v;
            switch (NIB(w, 5)) {
            case 0: v = rd(region(x6) + addr36(o), t); break;
            case 1: v = rd(region(x6) + reg[x7], t); break;
            case 2: v = rd(reg[x6] + addr36(o), t); break;
            case 3: v = rd(reg[x6] + reg[x7], t); break;
            default: v = mask(t, reg[x6]); break;
            }
            LOG("[cheats] %s: log %x: %#llx\n", ch->name, NIB(w, 4), (unsigned long long)v);
            break;
        }
        }
        i = next;
    }
    if (budget < 0) LOG("[cheats] %s: ran too long (a loop that never ends?): stopped for this frame\n", ch->name);
    if (ch->jcount > 4096) jprune(ch);
}

static void run_all(void) {
    for (int pass = 0; pass < 2; pass++)  // master codes first
        for (int i = 0; i < ncheats; i++)
            if (cheats[i].enabled && cheats[i].master == !pass) run(&cheats[i]);
}

// ------------------------------------------------------------------ what the menu switched
static void state_path(char *out, size_t n) { snprintf(out, n, "%s/cheats_state.ini", cheats_dir()); }

static void state_save(void) {  // one line per cheat: 1|0 TAB file TAB name
    char path[1100];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < ncheats; i++) fprintf(f, "%d\t%s\t%s\n", cheats[i].enabled, cheats[i].file, cheats[i].name);
    fclose(f);
}

static void state_load(void) {
    char path[1100], line[512];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *file = strchr(line, '\t');
        if (!file) continue;
        *file++ = 0;
        char *name = strchr(file, '\t');
        if (!name) continue;
        *name++ = 0;
        for (int i = 0; i < ncheats; i++)
            if (!strcmp(cheats[i].file, file) && !strcmp(cheats[i].name, name)) cheats[i].enabled = line[0] == '1';
    }
    fclose(f);
}

// ------------------------------------------------------------------ for the F1 menu
int cheats_count(void) { return ncheats; }
const char *cheats_name(int i) { return i >= 0 && i < ncheats ? cheats[i].name : ""; }
const char *cheats_file(int i) { return i >= 0 && i < ncheats ? cheats[i].file : ""; }
bool cheats_master(int i) { return i >= 0 && i < ncheats && cheats[i].master; }
bool cheats_enabled(int i) { return i >= 0 && i < ncheats && cheats[i].enabled; }

// Does it only write fixed values (a patch, mostly of the game's code: it runs in the interpreter)?
bool cheats_is_patch(int i) {
    if (i < 0 || i >= ncheats || !cheats[i].nops) return false;
    for (int k = 0; k < cheats[i].nops; k++)
        if (cheats[i].ops[k].type != 0x0) return false;
    return true;
}

// Switch a cheat on or off now. Returns its state afterwards.
bool cheats_set_enabled(int i, bool on) {
    if (i < 0 || i >= ncheats) return false;
    Cheat *c = &cheats[i];
    lock_cheats();
    if (c->enabled != on) {
        if (on) {
            c->enabled = true;
            run(c);  // at once: its changes to the game's code are in place before the next frame
        } else {
            revert(c);
            c->enabled = false;
        }
        state_save();
    }
    bool now = c->enabled;
    unlock_cheats();
    return now;
}

void cheats_disable_all(void) {
    lock_cheats();
    for (int i = 0; i < ncheats; i++)
        if (cheats[i].enabled) {
            revert(&cheats[i]);
            cheats[i].enabled = false;
        }
    state_save();
    unlock_cheats();
}

// ------------------------------------------------------------------ entry points
void cheats_init(void) {
    DIR *d = opendir(cheats_dir());
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] != '.' && l > 4 && !strcasecmp(e->d_name + l - 4, ".txt"))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_name);
    for (int i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", cheats_dir(), names[i]);
        load_off = names[i][0] == '_';
        load_file(path, names[i]);
        free(names[i]);
    }
    load_off = false;
    state_load();
    if (ncheats) run_all();  // (before the game starts: changes to its code are in place from the start)
}

void cheats_frame(void) {
    static uint64_t last;
    if (!ncheats || g_present_count == last) return;
    last = g_present_count;
    uint64_t b;
    int32_t s[4];
    input_get(&b, s);
    enum { T = 16384 };  // (stick directions: half way)
    b &= 0xFFFF;
    if (s[0] < -T) b |= 1u << 16;
    if (s[1] > T) b |= 1u << 17;
    if (s[0] > T) b |= 1u << 18;
    if (s[1] < -T) b |= 1u << 19;
    if (s[2] < -T) b |= 1u << 20;
    if (s[3] > T) b |= 1u << 21;
    if (s[2] > T) b |= 1u << 22;
    if (s[3] < -T) b |= 1u << 23;
    keys = b;
    lock_cheats();
    run_all();
    unlock_cheats();
}
