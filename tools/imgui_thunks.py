"""Writes runtime/forge_imgui_thunks.inc: Forge plugins' ImGui calls as host functions.

Forge gives its plugins Dear ImGui through cimgui's C API (the ig* / Im* functions it exports); the port runs the
real Dear ImGui on the host (runtime/overlay.cpp) and binds those imports to the functions generated here, from
cimgui's own description of its API (third_party/cimgui/generator/output). Each one reads its arguments as an
AArch32 plugin passes them. Forge and its plugins are built with -mfloat-abi=softfp: the base AAPCS, floats and all
in r0-r3 and the stack (doubles and 64-bit values in an even register pair / 8-aligned slot), float results in r0
(r0:r1), structs larger than a word (ImVec2...) returned through a pointer the caller passes in r0. (VFP below:
the hard-float variant, kept for reference.)
converts them and calls cimgui:
  - guest pointers to plain data (strings, float* / bool* / int*, buffers): the host address of that guest memory
  - pointers to ImGui's objects (ImGuiIO*, ImDrawList*...): mirrors, runtime/forge_imgui.c - a copy of the host
    object in guest memory with the plugin's (32-bit) layout, its fields kept in step both ways; tables below
  - printf-style functions: formatted from the guest's arguments, then passed as "%s"
  - the callbacks plugins can pass (text input, size constraints, list getters) run through host trampolines
Functions this can't express (other callbacks, pointers to host data...) are left out: the call then reaches the
"unimplemented" path, which logs its name. The file's end lists them.

    tools/imgui_thunks.py [cimgui folder]       (default third_party/cimgui)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', 'cimgui')
OUT = os.path.join(ROOT, 'runtime', 'forge_imgui_thunks.inc')
J = lambda n: json.load(open(os.path.join(CIM, 'generator', 'output', n), encoding='utf-8'))
DEFS, SE, TD = J('definitions.json'), J('structs_and_enums.json'), J('typedefs_dict.json')
STRUCTS, ENUMS, ENUMTYPES, LOC = SE['structs'], SE['enums'], SE['enumtypes'], SE['locations']

# hand-written in runtime/forge_imgui.c (contexts, allocators, the frame itself, context hooks, files, texture pixels,
# glyph ranges)
OVERRIDE = {'igCreateContext', 'igDestroyContext', 'igSetAllocatorFunctions', 'igGetAllocatorFunctions', 'igMemAlloc',
            'igMemFree', 'igNewFrame', 'igEndFrame', 'igRender', 'igGetDrawData', 'igAddContextHook',
            'igImFileOpen', 'igImFileClose', 'igImFileGetSize', 'igImFileRead', 'igImFileWrite',
            'ImTextureData_GetPixels', 'ImTextureData_GetPixelsAt', 'igRegisterUserTexture', 'ImTextureData_destroy',
            'ImFont_CalcTextSizeA', 'ImFontAtlas_GetGlyphRangesDefault',
            'ImFontGlyphRangesBuilder_BuildRanges'}
# structs whose mirrors are refreshed around every call (the plugin reads and writes them at any time)
HOT = {'ImGuiIO', 'ImGuiStyle', 'ImGuiPlatformIO'}
# callbacks run through trampolines in runtime/forge_imgui.c (type as cimgui writes it -> trampoline)
TRAMPOLINES = {'ImGuiInputTextCallback': 'tr_input_text', 'ImGuiSizeCallback': 'tr_size',
               'const char*(*)(void* user_data,int idx)': 'tr_getter_str',
               'float(*)(void* data,int idx)': 'tr_getter_float'}

VFP = False  # plugins' float ABI: softfp (see above)

SCALAR = {'char': 1, 'signed char': 1, 'unsigned char': 1, 'bool': 1, 'short': 2, 'signed short': 2,
          'unsigned short': 2, 'int': 4, 'signed int': 4, 'unsigned int': 4, 'float': 4, 'double': 8,
          'long long': 8, 'signed long long': 8, 'unsigned long long': 8}
PTR = {'g': 4, 'h': 8}


class Unsupported(Exception):
    pass


# ---- types: ('scalar', name) ('size_t',) ('ptr', T) ('fn', text) ('struct', name) ('vector',) ('union', text)
#      ('va_list',) ('varargs',)
def parse(t):
    t = t.strip().rstrip(';').strip()
    if '(*)' in t:
        return ('fn', t)
    if t == '...':
        return ('varargs',)
    m = re.match(r'^(.*?)\s*\[\w*\]$', t)
    if m:  # (an array argument is a pointer)
        return ('ptr', parse(m.group(1)))
    if t.startswith('union'):
        return ('union', t)
    t = re.sub(r'\b(const|struct)\b', '', t).strip()
    t = re.sub(r'\s+', ' ', t)
    if t.endswith('*'):
        return ('ptr', parse(t[:-1]))
    if t == 'void':
        return ('void',)
    for _ in range(16):
        if t in SCALAR:
            return ('scalar', t)
        if t in ('size_t', 'va_list'):
            return (t,)
        if t in ENUMTYPES:
            t = ENUMTYPES[t]
            continue
        if t in ENUMS or t + '_' in ENUMS and t not in TD:
            return ('scalar', 'int')
        if t.startswith('ImVector_'):
            return ('vector',)
        if t in STRUCTS:
            return ('struct', t)
        if t.endswith('_c') and t[:-2] in STRUCTS:
            return ('struct', t[:-2])
        if t in TD:
            v = TD[t].strip().rstrip(';').strip()
            if '(*)' in v:
                return ('fn', t)
            v = re.sub(r'^struct\s+', '', v)
            if v == t:
                break
            return parse(v)
        break
    return ('unknown', t)


def align(x, a):
    return (x + a - 1) // a * a


# ---- struct layouts, per ABI ('g' AArch32, 'h' the host's 64-bit one): (size, align, leaves); a leaf is
# (offset, kind, size, arg): kinds raw (same bytes in both), size (size_t), str (char*), obj (pointer to a struct
# named arg), data (other pointer), fn (function pointer: not carried over)
_layouts = {}


def layout(name, abi):
    key = (name, abi)
    if key in _layouts:
        if _layouts[key] is None:
            raise Unsupported('recursive ' + name)
        return _layouts[key]
    _layouts[key] = None
    try:
        r = _layout(name, abi)
    except Unsupported:
        del _layouts[key]
        raise
    _layouts[key] = r
    return r


def _layout(name, abi):
    off, al, leaves, unit = 0, 1, [], None
    for f in STRUCTS[name]:
        if 'bitfield' in f:  # consecutive bit-fields share units of their type's size (as both compilers do here)
            ft = parse(f['type'])
            if ft[0] != 'scalar':
                raise Unsupported('bitfield type')
            sz, n = SCALAR[ft[1]], int(f['bitfield'])
            if unit and unit[2] == sz and unit[1] + n <= sz * 8:
                unit[1] += n
                continue
            off = align(off, sz)
            leaves.append((off, 'raw', sz, None))
            unit = [off, n, sz]
            off += sz
            al = max(al, sz)
            continue
        unit = None
        s, a, lv = type_layout(parse(f['type']), abi)
        off = align(off, a)
        for i in range(f.get('size', 1)):
            leaves += [(off + i * s + o, k, z, x) for o, k, z, x in lv]
        off += s * f.get('size', 1)
        al = max(al, a)
    return align(off, al), al, leaves


def type_layout(t, abi):
    k = t[0]
    if k == 'scalar':
        s = SCALAR[t[1]]
        return s, s, [(0, 'raw', s, None)]
    if k == 'size_t':
        return PTR[abi], PTR[abi], [(0, 'size', PTR[abi], None)]
    if k == 'ptr':
        p, tgt = PTR[abi], t[1]
        if tgt[0] == 'scalar' and tgt[1] == 'char':
            return p, p, [(0, 'str', p, None)]
        if tgt[0] == 'struct':
            return p, p, [(0, 'obj', p, tgt[1])]
        return p, p, [(0, 'data', p, None)]
    if k == 'fn':
        return PTR[abi], PTR[abi], [(0, 'fn', PTR[abi], None)]
    if k == 'struct':
        return layout(t[1], abi)
    if k == 'vector':  # ImVector<T>: int Size, Capacity; T *Data
        p = PTR[abi]
        return align(8 + p, p), p, [(0, 'raw', 8, None), (8, 'data', p, None)]
    if k == 'union':  # the members' first 4 bytes (all of them here are int / float / pointer arrays)
        mem = [m.strip() for m in t[1][t[1].index('{') + 1:t[1].rindex('}')].split(';') if m.strip()]
        size = al = 1
        for m in mem:
            mm = re.match(r'^(.*?)\s*(\w+)(?:\[(\d+)\])?$', m)
            s, a, _ = type_layout(parse(mm.group(1)), abi)
            n = int(mm.group(3) or 1)
            size, al = max(size, s * n), max(al, a)
        return align(size, al), al, [(0, 'raw', 4, None)]
    raise Unsupported('type %s' % (t,))


def hfa(t):
    """Number of floats if t is a homogeneous float aggregate of 1-4 floats (AAPCS-VFP passes those in s regs)."""
    if t[0] == 'scalar':
        return 1 if t[1] == 'float' else 0
    if t[0] != 'struct':
        return 0
    n = 0
    for f in STRUCTS[t[1]]:
        if 'bitfield' in f:
            return 0
        k = hfa(parse(f['type']))
        if not k:
            return 0
        n += k * f.get('size', 1)
    return n if n <= 4 else 0


# ---- the struct tables (mirrors)
TYPES = sorted(STRUCTS)
TID = {n: i for i, n in enumerate(TYPES)}


def synced(name):
    """Mirrors with their fields carried over: the public API's structs (imgui.h) whose layout is known."""
    if not LOC.get(name, '').startswith('imgui:'):
        return False
    try:
        layout(name, 'g'), layout(name, 'h')
        return True
    except Unsupported:
        return False


def fields_table(name):
    gs, ga, gl = layout(name, 'g')
    hs, ha, hl = layout(name, 'h')
    assert len(gl) == len(hl), name
    out = []
    for (go, gk, gz, gx), (ho, hk, hz, hx) in zip(gl, hl):
        assert gk == hk and gx == hx, name
        kind = gk
        if kind == 'obj' and gx not in TID:
            kind = 'data'
        if kind == 'raw' and out and out[-1][3] == 'raw' and out[-1][0] + out[-1][2] == go and out[-1][1] + out[-1][2] == ho:
            out[-1] = (out[-1][0], out[-1][1], out[-1][2] + gz, 'raw', None)
            continue
        out.append((go, ho, gz if kind == 'raw' else 0, kind, gx if kind == 'obj' else None))
    return gs, hs, out


# ---- prototypes as cimgui.h declares them (for struct argument types and the va_list wrappers)
PROTO = {}
for line in open(os.path.join(CIM, 'cimgui.h'), encoding='utf-8'):
    m = re.match(r'^CIMGUI_API (.+?)\s*\b(\w+)\((.*)\);\s*$', line)
    if m:
        PROTO[m.group(2)] = (m.group(1).strip(), m.group(3))


def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip() and cur.strip() != 'void':
        out.append(cur)
    return [p.strip() for p in out]


def param_ctype(p):
    """'const ImVec2_c size' -> 'ImVec2_c'"""
    p = re.sub(r'\bconst\b', '', p).strip()
    return re.sub(r'\s*\w+$', '', p).strip()


def by_value_ok(gl, hl):
    """A struct passed or returned by value: plain data, and pointers to mirrored objects (ImTextureRef)."""
    return len(gl) == len(hl) and all(
        gk == hk and (gk == 'raw' and gz == hz or gk == 'obj' and gx in TID and gx == hx)
        for (_, gk, gz, gx), (_, hk, hz, hx) in zip(gl, hl))


# ---- functions
class Alloc:
    """AAPCS argument allocation (with VFP unless variadic)."""
    def __init__(self, vfp, first=0):
        self.ncrn, self.nsaa, self.vfp = first, 0, vfp
        self.sfree = [True] * 16
        self.vfp_stacked = False

    def word(self):
        if self.ncrn < 4:
            self.ncrn += 1
            return 'c->r[%d]' % (self.ncrn - 1)
        self.nsaa += 4
        return 'STK(%d)' % (self.nsaa // 4 - 1)

    def words(self, n, al):  # a composite or 64-bit value in core registers / the stack
        if al == 8 and self.ncrn % 2:
            self.ncrn += 1
        if self.ncrn + n <= 4 or (self.ncrn < 4 and self.nsaa == 0):  # (one that doesn't fit is split: C.5)
            return [self.word() for _ in range(n)]
        self.ncrn = 4
        if al == 8:
            self.nsaa = align(self.nsaa, 8)
        return [self.word() for _ in range(n)]

    def floats(self, n, dbl=False):  # n floats (or one double) in VFP registers, else the stack
        if self.vfp and not self.vfp_stacked:
            step = 2 if dbl else 1
            cnt = 2 if dbl else n
            for i in range(0, 16 - cnt + 1, step):
                if all(self.sfree[i:i + cnt]):
                    for j in range(i, i + cnt):
                        self.sfree[j] = False
                    return ['c->v.df[%d]' % (i // 2)] if dbl else ['c->v.f[%d]' % (i + j) for j in range(n)]
            self.vfp_stacked = True
            self.sfree = [False] * 16
        if not self.vfp:
            if dbl:
                lo, hi = self.words(2, 8)
                return ['ig_d(%s, %s)' % (lo, hi)]
            return ['ig_f(%s)' % w for w in (self.words(n, 4) if n > 1 else [self.word()])]
        if dbl:
            self.nsaa = align(self.nsaa, 8)
            self.nsaa += 8
            return ['LDD(c->r[13] + %d)' % (self.nsaa - 8)]
        self.nsaa += 4 * n
        return ['LDF(c->r[13] + %d)' % (self.nsaa - 4 * (n - i)) for i in range(n)]


def gen(o):
    sym = o['ov_cimguiname']
    if sym not in PROTO:
        raise Unsupported('not in cimgui.h')
    pret, pparams = PROTO[sym]
    params = split_params(pparams)
    variadic = 'isvararg' in o
    args = o['argsT']
    if len(params) != len(args):
        raise Unsupported('parameters')
    # the return value
    if o.get('constructor'):
        rt = ('ptr', ('struct', o['stname']))
    elif o.get('destructor'):
        rt = ('void',)
    else:
        rt = ('void',) if o['ret'] == 'void' else parse(o['ret'])
    memret = False
    if rt[0] == 'struct' and (not hfa(rt) or not VFP):
        gs, _, gl = layout(rt[1], 'g')
        hs, _, hl = layout(rt[1], 'h')
        if not by_value_ok(gl, hl) or (gs != hs or any(k != 'raw' for _, k, _, _ in gl)) and gs <= 4:
            raise Unsupported('returns %s' % rt[1])
        memret = gs > 4
    if rt[0] == 'struct' and hfa(rt) and variadic and VFP:
        raise Unsupported('variadic float aggregate return')
    if rt[0] in ('unknown', 'fn', 'vector', 'union', 'va_list'):
        raise Unsupported('returns %s' % o['ret'])
    if rt[0] == 'ptr' and rt[1][0] in ('ptr', 'fn'):
        raise Unsupported('returns %s' % o['ret'])

    vfp = VFP and not variadic
    al = Alloc(vfp, 1 if memret else 0)
    pre, call, post, conv = [], [], [], []
    raw = {}
    fmt_at = None
    for i, (a, p) in enumerate(zip(args, params)):
        t = parse(a['type'])
        nm = 'a%d' % i
        k = t[0]
        if k == 'varargs':
            fmt_at = (al.ncrn, al.nsaa)
            call.append('"%s", s_')
            continue
        if k == 'scalar':
            if t[1] == 'float':
                pre.append('float %s = %s;' % (nm, al.floats(1)[0]))
            elif t[1] == 'double':
                pre.append('double %s = %s;' % (nm, al.floats(1, True)[0]))
            elif SCALAR[t[1]] == 8:
                lo, hi = al.words(2, 8)
                pre.append('uint64_t %s = (uint64_t)%s | (uint64_t)%s << 32;' % (nm, lo, hi))
            else:
                pre.append('uint32_t %s = %s;' % (nm, al.word()))
            call.append(nm if t[1] not in ('bool',) else '(%s != 0)' % nm)
        elif k == 'size_t':
            pre.append('uint32_t %s = %s;' % (nm, al.word()))
            call.append('(size_t)' + nm)
        elif k == 'va_list':
            pre.append('uint32_t %s = %s;' % (nm, al.word()))
            raw['va'] = nm
            call.append(None)  # (the wrapper's "%s", s_)
        elif k == 'ptr':
            pre.append('uint32_t %s = %s;' % (nm, al.word()))
            tgt = t[1]
            if tgt[0] == 'struct':
                conv.append('void *h%s = m_in(%s, %d);' % (nm, nm, TID[tgt[1]]))
                post.append('m_after(%s);' % nm)
                call.append('h' + nm)
            elif tgt[0] == 'ptr' and tgt[1][0] == 'scalar' and tgt[1][1] == 'char' and 'const char* const' in a['type']:
                cnt = next((x['name'] for x in args if x['name'] == 'items_count'), None)
                if not cnt:
                    raise Unsupported('string array without a count')
                conv.append('const char **h%s = ig_strarr(%s, a%d);' % (nm, nm, [x['name'] for x in args].index(cnt)))
                call.append('h' + nm)
            elif tgt[0] in ('ptr', 'fn', 'unknown', 'size_t', 'vector'):
                raise Unsupported('argument %s' % a['type'])
            elif a['name'].endswith('filename') and tgt == ('scalar', 'char'):
                call.append('ig_path(%s)' % nm)  # (a console path, "app:/...": the host file behind it)
            else:
                call.append('GP(%s)' % nm)
        elif k == 'fn':
            tr = TRAMPOLINES.get(a['type'])
            if not tr:
                raise Unsupported('callback %s' % a['type'])
            pre.append('uint32_t %s = %s;' % (nm, al.word()))
            conv.append('uint32_t cb_prev_ = t_cb; t_cb = %s;' % nm)
            post.append('t_cb = cb_prev_;')
            call.append('(%s ? %s : NULL)' % (nm, tr))
        elif k == 'struct':
            n = hfa(t)
            ct = param_ctype(p)
            if n:
                fl = al.floats(n)
                call.append('(%s){%s}' % (ct, ', '.join(fl)))
            else:
                gs, ga, gl = layout(t[1], 'g')
                hs, ha, hl = layout(t[1], 'h')
                if not by_value_ok(gl, hl):
                    raise Unsupported('argument %s' % a['type'])
                ws = al.words(align(gs, 4) // 4, ga)
                pre.append('uint32_t %s[%d] = {%s};' % (nm, len(ws), ', '.join(ws)))
                if gs == hs and all(k2 == 'raw' for _, k2, _, _ in gl):
                    conv.append('%s h%s; memcpy(&h%s, %s, sizeof h%s);' % (ct, nm, nm, nm, nm))
                else:  # (with pointers to mirrored objects in it: field by field, as the layouts differ)
                    conv.append('%s h%s; memset(&h%s, 0, sizeof h%s);' % (ct, nm, nm, nm))
                    for (go, gk, gz, gx), (ho, _, hz, _) in zip(gl, hl):
                        if gk == 'raw':
                            conv.append('memcpy((char *)&h%s + %d, (char *)%s + %d, %d);' % (nm, ho, nm, go, gz))
                        else:
                            conv.append('{ void *p_ = m_in(%s[%d], %d); memcpy((char *)&h%s + %d, &p_, sizeof p_); }' % (
                                nm, go // 4, TID[gx], nm, ho))
                call.append('h' + nm)
        else:
            raise Unsupported('argument %s' % a['type'])
    if fmt_at is not None:
        fi = max(i for i, x in enumerate(args[:-1]) if x['type'].strip() == 'const char*')
        conv.append('const char *s_ = ig_fmt(c, a%d, %d, %d);' % (fi, fmt_at[0], fmt_at[1]))
        call[fi] = None  # ("%s" takes its place)
        call = [x for x in call if x is not None]
    wrapper = ''
    fn = sym
    if 'va' in raw:
        fi = len(args) - 2
        conv.append('const char *s_ = ig_vfmt(c, a%d, %s);' % (fi, raw['va']))
        call = call[:fi] + ['"%s", s_']
        fn = sym + '_h'
        ps = ', '.join(params[:-1])
        names = [re.search(r'(\w+)$', x).group(1) for x in params[:-1]]
        ret = pret
        body = ('%s r_ = %s(%s, ap); va_end(ap); return r_;' % (ret, sym, ', '.join(names)) if ret != 'void'
                else '%s(%s, ap); va_end(ap);' % (sym, ', '.join(names)))
        wrapper = 'static %s %s(%s, ...) { va_list ap; va_start(ap, %s); %s }\n' % (ret, fn, ps, names[-1], body)
    # the call and its result
    callx = '%s(%s)' % (fn, ', '.join(call))
    lines = ['IG_ENTER(c);'] if not memret else ['uint32_t ret_ = c->r[0];', 'IG_ENTER(c);']
    lines = pre + lines + conv
    k = rt[0]
    if k == 'void':
        lines.append(callx + ';')
        res = []
    else:
        lines.append('__auto_type r_ = %s;' % callx)
        if k == 'scalar':
            if rt[1] == 'float':
                res = ['c->v.f[0] = r_;'] if vfp else ['memcpy(&c->r[0], &r_, 4);']
            elif rt[1] == 'double':
                res = ['c->v.df[0] = r_;'] if vfp else ['memcpy(&c->r[0], &r_, 8);']
            elif rt[1] == 'bool':
                res = ['c->r[0] = r_ != 0;']
            elif SCALAR[rt[1]] == 8:
                res = ['c->r[0] = (uint32_t)r_; c->r[1] = (uint32_t)((uint64_t)r_ >> 32);']
            else:
                res = ['c->r[0] = (uint32_t)r_;']
        elif k == 'size_t':
            res = ['c->r[0] = (uint32_t)r_;']
        elif k == 'ptr':
            tgt = rt[1]
            if o.get('constructor'):
                res = ['c->r[0] = m_out_new(r_, %d);' % TID[tgt[1]]]
            elif tgt[0] == 'struct':
                res = ['c->r[0] = m_out(r_, %d);' % TID[tgt[1]]]
            elif tgt[0] == 'scalar' and tgt[1] == 'char':
                res = ['c->r[0] = ig_str_out(r_);']
            else:
                res = ['c->r[0] = h2g(r_);']
        elif k == 'struct':
            if memret:
                gs, _, gl = layout(rt[1], 'g')
                hs, _, hl = layout(rt[1], 'h')
                if gs == hs and all(k2 == 'raw' for _, k2, _, _ in gl):
                    res = ['memcpy(G2H(ret_), &r_, sizeof r_);', 'c->r[0] = ret_;']
                else:  # (field by field, as above)
                    res = ['memset(G2H(ret_), 0, %d);' % gs]
                    for (go, gk, gz, gx), (ho, _, hz, _) in zip(gl, hl):
                        if gk == 'raw':
                            res.append('memcpy((char *)G2H(ret_) + %d, (char *)&r_ + %d, %d);' % (go, ho, gz))
                        else:
                            res.append('{ void *p_; memcpy(&p_, (char *)&r_ + %d, sizeof p_); uint32_t v_ = m_out(p_, %d); '
                                       'memcpy((char *)G2H(ret_) + %d, &v_, 4); }' % (ho, TID[gx], go))
                    res.append('c->r[0] = ret_;')
            elif hfa(rt):
                res = ['memcpy(c->v.f, &r_, sizeof r_);']
            else:
                res = ['memcpy(&c->r[0], &r_, sizeof r_);']
        else:
            raise Unsupported('result')
    if o.get('destructor'):  # (the host object is gone: its mirror goes, nothing to copy back)
        post = [x for x in post if x != 'm_after(a0);'] + ['m_destroy(a0);']
    lines += post + ['IG_LEAVE(c);'] + res
    body = '\n'.join('    ' + x for x in lines)
    return wrapper + 'HLE_FUNC("%s", ig_%s) {\n%s\n}\n' % (sym, sym, body)


def main():
    out = ['// Generated by tools/imgui_thunks.py from cimgui (third_party/cimgui): do not edit. Included twice by',
           '// runtime/forge_imgui.c: the mirror tables (FORGE_IMGUI_TABLES), then the functions, which use its helpers',
           '// (IG_ENTER / IG_LEAVE, GP, m_in / m_out...).', '#ifdef FORGE_IMGUI_TABLES']
    # mirror tables
    tables, asserts = [], []
    for n in TYPES:
        if not synced(n):
            tables.append('    {"%s", 0, 0, NULL, 0, %d},' % (n, n in HOT))
            continue
        gs, hs, fl = fields_table(n)
        out.append('static const MField mf_%s[] = {' % n)
        kinds = {'raw': 'MF_RAW', 'size': 'MF_SIZE', 'str': 'MF_STR', 'obj': 'MF_OBJ', 'data': 'MF_DATA', 'fn': 'MF_FN'}
        for go, ho, sz, k, x in fl:
            out.append('    {%d, %d, %d, %s, %d},' % (go, ho, sz, kinds[k], TID[x] if x else 0))
        out.append('};')
        tables.append('    {"%s", %d, %d, mf_%s, %d, %d},' % (n, gs, hs, n, len(fl), n in HOT))
        # the layout model checked against the compiler's (the guest side has only the model)
        asserts.append('_Static_assert(sizeof(%s) == %d, "%s: host layout");' % (n, hs, n))
        for fname, o in _field_offsets(n, 'h'):
            asserts.append('_Static_assert(offsetof(%s, %s) == %d, "%s.%s");' % (n, fname, o, n, fname))
    out.append('static const MType mtypes[] = {')
    out += tables
    out.append('};')
    out.append('enum { MTYPES = %d };' % len(TYPES))
    for n in TYPES:
        out.append('#define T_%s %d' % (n, TID[n]))
    out += asserts
    out += ['#else', '#pragma clang diagnostic ignored "-Wmissing-braces"', '']
    done, skipped = 0, []
    for name in sorted(DEFS):
        for o in DEFS[name]:
            sym = o['ov_cimguiname']
            if o['location'].startswith('imgui_freetype') or o.get('templated') or sym in OVERRIDE:
                continue
            try:
                out.append(gen(o))
                done += 1
            except Unsupported as e:
                skipped.append('%s (%s)' % (sym, e))
            except (KeyError, ValueError) as e:
                skipped.append('%s (%s)' % (sym, e))
    out.append('// %d functions; not provided (%d):' % (done, len(skipped)))
    out += ['//   ' + s for s in skipped]
    out.append('#endif')
    with open(OUT, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    print('%d functions, %d left out, %d mirror types (%d with fields)' % (
        done, len(skipped), len(TYPES), sum(1 for n in TYPES if synced(n))))


def _field_offsets(name, abi):
    off, unit, res = 0, None, []
    for f in STRUCTS[name]:
        if 'bitfield' in f:
            sz, n = SCALAR[parse(f['type'])[1]], int(f['bitfield'])
            if unit and unit[2] == sz and unit[1] + n <= sz * 8:
                unit[1] += n
                continue
            off = align(off, sz)
            unit = [off, n, sz]
            off += sz
            continue
        unit = None
        s, a, _ = type_layout(parse(f['type']), abi)
        off = align(off, a)
        fname = re.sub(r'\[.*$', '', f['name'])
        if fname:
            res.append((fname, off))
        off += s * f.get('size', 1)
    return res


main()
