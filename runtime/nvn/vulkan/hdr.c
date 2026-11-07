// Native HDR output ([HDR] HDR = On).
//
// A port of the approach of RenoDX's Monster Hunter Generations Ultimate mod (souperman9; RenoDX framework by
// ShortFuse / Carlos Lopez, MIT licence: https://github.com/clshortfuse/renodx), done inside the renderer instead of
// through ReShade on an emulator:
//
// - The game's post chain from its colour grading on is rendered in RGBA16F instead of RGBA8 (B: the colour grading's
//   target, H: the full-screen image FXAA, the bloom add and the HUD are drawn into, P: the window texture the
//   brightness pass writes), and with [HDR] UnclampedScene (default) every other target the game renders into too (the
//   scene, depth of field, the bloom chain), so light above SDR white (the sun, fire, sparks, bloom) is kept.
// - The game's colour grading (drawn into B, the last full-screen draw into B before FXAA) and its bloom add (into H,
//   after FXAA, before the HUD) are drawn through HDR variants of their own translated shaders (ShaderKey.hdr: the
//   translated main() runs, then its output is decoded to linear light, graded with the user's settings - exposure,
//   highlights, shadows, contrast, flare, saturation, highlight saturation as RenoDX's ApplyUserColorGrading - tone
//   mapped to the display's peak (RenoDX's Neutwo per channel, the tone mapper its MHGU mod uses) and written in an
//   intermediate encoding in which 1.0 is UI white: the game's picture at GameBrightness, the HUD (drawn on top
//   unchanged, in [0, 1]) at UIBrightness.
// - FXAA, the HUD and the game's brightness pass (pow(c, cb3[0][0])) run unchanged on the wider values. At present the
//   window image is that picture (+ the settings overlay) decoded (gamma correction 2.2 / 2.4 as RenoDX's), scaled to
//   UI brightness, clamped to the peak in BT.2020 and encoded as HDR10 (PQ) or scRGB.
//
// The passes are recognised by the frame's structure (docs/FSR-PORT.md "Frame structure"), not by shader ids: B is the
// texture FXAA (its known code hash) samples; the colour grading is the last draw into B before FXAA (its fragment
// program's code hash is learnt from the previous frame); H is what the last draw before a present sampled.
// Logs: [hdr] lines when B, H or the grading's program are found. MHGU_HDR=0|1 overrides the setting.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel.h"
#include "hdr.h"
#include "shaders.h"

int g_hdr_out;

#define HDR_FXAA_FS_HASH 0x8530ded68f4407dfull  // the game's FXAA fragment shader (exec.c FXAA_FS_HASH)

// ---- frame tracking (render thread)
static NvnTexture *g_h, *g_b;                 // the full-screen image H, the colour grading's target B
static NvnTexture *last_target, *last_src;    // the frame's last draw: its target, the first texture it sampled
static uint64_t grade_hash, grade_hash_next;  // the colour grading's fragment program (learnt from the frame before)
static uint64_t last_b_hash;                  // this frame: the last full-screen draw into B before FXAA
static bool fxaa_seen, hud_seen;
static bool is_rt(NvnTexture *t) { return t && t->backend && ((TexBackend *)t->backend)->gpu_owned; }

int vk_hdr_draw(const FsrDraw *d, uint64_t fs_hash) {
    last_target = d->color0;
    last_src = d->ntex ? d->tex[0] : NULL;
    if (fs_hash == HDR_FXAA_FS_HASH) {  // FXAA: B -> H
        if (d->ntex && is_rt(d->tex[0]) && d->tex[0] != g_b) {
            g_b = d->tex[0];
            LOG("[hdr] colour grading target: %ux%u\n", g_b->width, g_b->height);
        }
        if (!fxaa_seen && last_b_hash) grade_hash_next = last_b_hash;
        fxaa_seen = true;
        return HDR_PASS_NONE;
    }
    if (!d->color0) return HDR_PASS_NONE;
    if (!fxaa_seen) {
        if (g_b && d->color0 == g_b && d->count <= 6) {
            last_b_hash = fs_hash;
            if (fs_hash == grade_hash) return HDR_PASS_GRADE;
        }
        return HDR_PASS_NONE;
    }
    if (hud_seen || !g_h || d->color0 != g_h) return HDR_PASS_NONE;
    // The bloom add: full-screen, sampling a smaller render target (the bloom chain is at half size and below). Other
    // full-screen draws into H sampling a render target (a copy of B, ...) carry values that are already in the HDR
    // encoding: drawn as they are (converting them again would scale the picture by GameBrightness / UIBrightness twice).
    bool reads_rt = false, reads_small = false;
    for (int i = 0; i < d->ntex; i++) {
        NvnTexture *t = d->tex[i];
        if (t == g_h || !is_rt(t)) continue;
        reads_rt = true;
        if (t != g_b && t->width < g_h->width && t->height < g_h->height) reads_small = true;
    }
    if (d->count <= 6 && reads_rt) {
        if (!reads_small) return HDR_PASS_NONE;
        static uint64_t logged[4];
        for (int i = 0; i < 4; i++) {
            if (logged[i] == fs_hash) break;
            if (!logged[i]) {
                logged[i] = fs_hash;
                LOG("[hdr] bloom add: fragment program %016llx (samples %ux%u)\n", (unsigned long long)fs_hash,
                    d->tex[0] ? d->tex[0]->width : 0, d->tex[0] ? d->tex[0]->height : 0);
                break;
            }
        }
        return HDR_PASS_BLOOM;
    }
    hud_seen = true;  // (the first other draw into H: the HUD)
    return HDR_PASS_NONE;
}

void vk_hdr_present(NvnTexture *t) {
    if (last_target == t && last_src && last_src != t && last_src->backend &&
        !(((TexBackend *)last_src->backend)->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && g_h != last_src) {
        g_h = last_src;
        LOG("[hdr] full-screen image: %ux%u\n", g_h->width, g_h->height);
    }
    if (grade_hash_next && grade_hash_next != grade_hash) {
        grade_hash = grade_hash_next;
        LOG("[hdr] colour grading: fragment program %016llx\n", (unsigned long long)grade_hash);
    }
    grade_hash_next = last_b_hash = 0;
    fxaa_seen = hud_seen = false;
    last_target = last_src = NULL;
}

// With UnclampedScene (the default) every RGBA8 2D target the game renders into - the scene, depth of field, the bloom
// chain at half size and below, the post chain - is RGBA16F, so nothing on the way clips light at SDR white (the bloom
// of the sun and fire included). Without it only the post chain from the colour grading on.
bool vk_hdr_rt_float(NvnTexture *t) {
    if (!vk_hdr_tracking() || !t) return false;
    if (t == g_b || t == g_h || (t->backend && ((TexBackend *)t->backend)->presented)) return true;
    return config_live_hdr_scene();
}

// (log) the passes' HDR variants: drawn through them or not, when that changes
static const char *const pass_name[] = {"", "colour grading", "bloom add"};
void vk_hdr_variant_used(int pass, bool ready, int state) {
    static int last[3] = {-1, -1, -1};
    if (pass < 1 || pass > 2 || last[pass] == (int)ready) return;
    last[pass] = ready;
    float p[HDR_NPARAMS];
    vk_hdr_params(p);
    LOG("[hdr] %s: %s (program state %d); tonemap %.0f gamma %.1f game/ui %.3f peak ratio %.3f exposure %.2f on %.0f\n",
        pass_name[pass], ready ? "drawn through its HDR variant" : "HDR variant not ready, the game's own shader", state,
        p[0], p[1], p[2], p[3], p[4], p[12]);
}


// ---- settings for the shaders
static float srgb_decode(float x) { return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); }

static float gamma_value(void) {
    int g = config_live_hdr_gamma();
    return g == HDR_GAMMA_22 ? 2.2f : g == HDR_GAMMA_24 ? 2.4f : 0.0f;
}

// [0] x tone mapper (HDR_TONEMAP_*), y gamma correction (0 off, 2.2, 2.4), z game white / UI white, w the tone mapper's
//     peak (relative to game white, in the gamma-corrected space); [1] exposure, highlights, shadows, contrast;
// [2] saturation, blowout (from highlight saturation), flare, bloom; [3] x 1: on
void vk_hdr_params(float o[HDR_NPARAMS]) {
    memset(o, 0, HDR_NPARAMS * sizeof *o);
    int tm = config_live_hdr_tonemap();
    float g = gamma_value(), game = config_live_hdr_game(), ui = config_live_hdr_ui(), peak = config_live_hdr_peak();
    float ratio = fmaxf(peak / game, 1.0f);
    if (g > 0) ratio = srgb_decode(powf(ratio, 1.0f / g));  // (RenoDX: correct::Gamma(peak ratio, true, gamma))
    o[0] = (float)tm, o[1] = g, o[2] = tm == HDR_TONEMAP_VANILLA ? 1.0f : game / ui, o[3] = ratio;
    o[4] = config_live_hdr_exposure();
    o[5] = config_live_hdr_highlights() * 0.02f;
    o[6] = config_live_hdr_shadows() * 0.02f;
    o[7] = config_live_hdr_contrast() * 0.02f;
    o[8] = config_live_hdr_saturation() * 0.02f;
    o[9] = -(config_live_hdr_highlight_saturation() * 0.02f - 1.0f);
    o[10] = 0.10f * powf(config_live_hdr_flare() * 0.02f, 10.0f);
    o[11] = config_live_hdr_bloom() * 0.01f;
    o[12] = vk_hdr_tracking() ? 1.0f : 0.0f;
}

void vk_hdr_metadata_values(float *peak, float *avg) {
    *peak = config_live_hdr_peak();
    *avg = config_live_hdr_game();
}

// ---- the HDR variants of the game's passes: its translated main() renamed and called first
// GLSL ports of RenoDX functions (MIT): srgb Encode/DecodeSafe, color::grade ApplyUserColorGrading, tonemap Neutwo,
// correct::GammaSafe, oklab, AP1 clamp.
static const char *const hdr_lib =
    "layout(std140, set = 1, binding = 23) uniform HdrParams { vec4 hdr_p[6]; };\n"
    "vec3 hdr_clean(vec3 c) {\n"
    "  return vec3(isnan(c.x) || isinf(c.x) ? 0.0 : c.x, isnan(c.y) || isinf(c.y) ? 0.0 : c.y,\n"
    "              isnan(c.z) || isinf(c.z) ? 0.0 : c.z);\n"
    "}\n"
    "vec3 hdr_spow(vec3 c, float e) { return sign(c) * pow(abs(c), vec3(e)); }\n"
    "vec3 hdr_srgb_dec(vec3 c) {\n"
    "  vec3 a = abs(c);\n"
    "  return sign(c) * mix(a / 12.92, pow((a + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), a));\n"
    "}\n"
    "vec3 hdr_srgb_enc(vec3 l) {\n"
    "  vec3 a = abs(l);\n"
    "  return sign(l) * mix(a * 12.92, 1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), a));\n"
    "}\n"
    "float hdr_y(vec3 c) { return dot(c, vec3(0.2126390059, 0.7151686788, 0.0721923154)); }\n"
    "vec3 hdr_cbrt(vec3 x) { return sign(x) * pow(abs(x), vec3(1.0 / 3.0)); }\n"
    "vec3 hdr_oklab(vec3 c) {\n"
    "  vec3 lms = vec3(dot(c, vec3(0.4122214708, 0.5363325363, 0.0514459929)),\n"
    "                  dot(c, vec3(0.2119034982, 0.6806995451, 0.1073969566)),\n"
    "                  dot(c, vec3(0.0883024619, 0.2817188376, 0.6299787005)));\n"
    "  lms = hdr_cbrt(lms);\n"
    "  return vec3(dot(lms, vec3(0.2104542553, 0.7936177850, -0.0040720468)),\n"
    "              dot(lms, vec3(1.9779984951, -2.4285922050, 0.4505937099)),\n"
    "              dot(lms, vec3(0.0259040371, 0.7827717662, -0.8086757660)));\n"
    "}\n"
    "vec3 hdr_from_oklab(vec3 lab) {\n"
    "  vec3 lms = vec3(dot(lab, vec3(1.0, 0.3963377774, 0.2158037573)), dot(lab, vec3(1.0, -0.1055613458, -0.0638541728)),\n"
    "                  dot(lab, vec3(1.0, -0.0894841775, -1.2914855480)));\n"
    "  lms = lms * lms * lms;\n"
    "  return vec3(dot(lms, vec3(4.0767416621, -3.3077115913, 0.2309699292)),\n"
    "              dot(lms, vec3(-1.2684380046, 2.6097574011, -0.3413193965)),\n"
    "              dot(lms, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));\n"
    "}\n"
    "vec3 hdr_clamp_ap1(vec3 c) {\n"  // (BT.709 -> AP1, >= 0, back)
    "  vec3 ap1 = vec3(dot(c, vec3(0.6130974, 0.3395231, 0.0473795)), dot(c, vec3(0.0701937, 0.9163539, 0.0134524)),\n"
    "                  dot(c, vec3(0.0206156, 0.1095698, 0.8698146)));\n"
    "  ap1 = max(ap1, vec3(0.0));\n"
    "  return vec3(dot(ap1, vec3(1.7050510, -0.6217921, -0.0832589)), dot(ap1, vec3(-0.1302564, 1.1408048, -0.0105484)),\n"
    "              dot(ap1, vec3(-0.0240034, -0.1289690, 1.1529724)));\n"
    "}\n"
    // RenoDX color::grade ApplyUserColorGrading (exposure, contrast + flare, highlights, shadows, blowout, saturation)
    "vec3 hdr_grade(vec3 color) {\n"
    "  float exposure = hdr_p[1].x, highlights = hdr_p[1].y, shadows = hdr_p[1].z, contrast = hdr_p[1].w;\n"
    "  float saturation = hdr_p[2].x, blowout = hdr_p[2].y, flare = hdr_p[2].z;\n"
    "  if (exposure == 1.0 && highlights == 1.0 && shadows == 1.0 && contrast == 1.0 && flare == 0.0 &&\n"
    "      saturation == 1.0 && blowout == 0.0) return color;\n"
    "  color *= exposure;\n"
    "  float y = hdr_y(color);\n"
    "  float yn = max(y, 0.0) / 0.18;\n"
    "  float fl = yn != 0.0 ? (yn + flare) / yn : 1.0;\n"
    "  float yc = yn > 0.0 ? pow(yn, contrast * fl) : 0.0;\n"
    "  float yh = yc > 0.0 ? pow(yc, highlights) : 0.0;\n"
    "  yh = mix(yc, yh, clamp(yc, 0.0, 1.0));\n"
    "  float ys = yh > 0.0 ? pow(yh, -(shadows - 2.0)) : 0.0;\n"
    "  ys = mix(ys, yh, clamp(yh, 0.0, 1.0));\n"
    "  color *= y > 0.0 ? ys * 0.18 / y : 0.0;\n"
    "  if (saturation != 1.0 || blowout != 0.0) {\n"
    "    vec3 lab = hdr_oklab(color);\n"
    "    if (blowout != 0.0) {\n"
    "      float change = pow(1.0 - clamp(y * 100.0 / 10000.0, 0.0, 1.0), 100.0 * abs(blowout));\n"
    "      lab.yz *= blowout < 0.0 ? 2.0 - change : change;\n"
    "    }\n"
    "    lab.yz *= saturation;\n"
    "    color = hdr_clamp_ap1(hdr_from_oklab(lab));\n"
    "  }\n"
    "  return color;\n"
    "}\n"
    // the tone mapper: 0 vanilla (SDR, clipped), 1 none (clipped at the peak at the end), 2 RenoDRT (Neutwo per channel:
    // p x / sqrt(x x + p p), RenoDX tonemap::neutwo::PerChannel, what its MHGU mod's DisplayMap uses)
    "vec3 hdr_tonemap(vec3 c) {\n"
    "  if (hdr_p[0].x < 0.5) return clamp(c, 0.0, 1.0);\n"
    "  if (hdr_p[0].x < 1.5) return c;\n"
    "  float p = hdr_p[0].w;\n"
    "  return p * c * inversesqrt(c * c + p * p);\n"
    "}\n"
    // -> the intermediate encoding (RenoDX draw::RenderIntermediatePass: gamma correction, scaled to UI white, back)
    "vec3 hdr_intermediate(vec3 lin) {\n"
    "  float g = hdr_p[0].y;\n"
    "  if (g > 0.0) return hdr_spow(hdr_spow(hdr_srgb_enc(lin), g) * hdr_p[0].z, 1.0 / g);\n"
    "  return hdr_srgb_enc(lin * hdr_p[0].z);\n"
    "}\n";

static const char *const grade_main =
    "void main() {\n"
    "  hdr_game_main();\n"
    "  if (hdr_p[3].x < 0.5) return;\n"
    "  c0.rgb = hdr_intermediate(hdr_tonemap(hdr_grade(hdr_srgb_dec(hdr_clean(c0.rgb)))));\n"
    "}\n";

static const char *const bloom_main =
    "void main() {\n"
    "  hdr_game_main();\n"
    "  if (hdr_p[3].x < 0.5) return;\n"
    "  vec3 lin = hdr_srgb_dec(hdr_clean(c0.rgb));\n"
    "  if (hdr_p[0].x < 0.5) lin = clamp(lin, 0.0, 1.0);\n"
    "  c0.rgb = hdr_intermediate(hdr_tonemap(hdr_grade(lin * hdr_p[2].w)));\n"
    "}\n";

// Every other fragment shader (exec.c compile_program): its colour outputs as an RGBA8 target took them - no NaN,
// nothing below 0, alpha at most 1 - but colour above 1 kept (up to 256, far above any peak). In 16-bit float (HDR) a
// NaN or infinity (an overflow; 0 / 0 in a particle's shader, which RGBA8 turned into 0) survived into the blend, where
// even alpha 0 keeps it (NaN x 0 = NaN): the whole sprite's square came out black in the grading. Always, HDR on or
// off: programs are built once, and HDR can be turned on while playing; into RGBA8 (HDR off) it changes nothing.
char *vk_hdr_guard_fs(const char *src) {
    if (!src) return NULL;
    const char *m = strstr(src, "void main()");
    if (!m) return NULL;
    char body[1024] = "void main() {\n  hdr_guarded_main();\n";
    int n = 0;
    for (int rt = 0; rt < 8; rt++) {
        char decl[48];
        snprintf(decl, sizeof decl, "out vec4 c%d;", rt);
        if (!strstr(src, decl)) continue;
        char line[96];
        snprintf(line, sizeof line, "  c%d = hdr_guard(c%d);\n", rt, rt);
        strcat(body, line);
        n++;
    }
    if (!n) return NULL;
    strcat(body, "}\n");
    static const char guard[] =
        "vec4 hdr_guard(vec4 c) {\n"
        "  bvec4 bad = bvec4(isnan(c.x) || isinf(c.x), isnan(c.y) || isinf(c.y), isnan(c.z) || isinf(c.z),\n"
        "                    isnan(c.w) || isinf(c.w));\n"
        "  c = mix(c, vec4(0.0), bad);\n"
        "  return vec4(clamp(c.rgb, 0.0, 256.0), clamp(c.w, 0.0, 1.0));\n"
        "}\n";
    static const char renamed[] = "void hdr_guarded_main()";
    size_t head = (size_t)(m - src), tail = strlen(m + 11);
    char *out = malloc(head + sizeof renamed + tail + sizeof guard + strlen(body) + 2);
    if (!out) return NULL;
    memcpy(out, src, head);
    char *p = out + head;
    memcpy(p, renamed, sizeof renamed - 1), p += sizeof renamed - 1;
    memcpy(p, m + 11, tail), p += tail;
    *p++ = '\n';
    strcpy(p, guard);
    strcat(p, body);
    return out;
}

char *vk_hdr_wrap_fs(const char *src, int pass) {
    const char *m = src ? strstr(src, "void main()") : NULL;
    if (!m || !strstr(src, " c0;")) {
        LOG("[hdr] the game's pass %d has no main() / c0 to wrap: drawn as it is\n", pass);
        return NULL;
    }
    const char *body = pass == HDR_PASS_GRADE ? grade_main : bloom_main;
    static const char renamed[] = "void hdr_game_main()";
    size_t head = (size_t)(m - src), tail = strlen(m + 11);
    size_t n = head + sizeof renamed - 1 + tail + strlen(hdr_lib) + strlen(body) + 2;
    char *out = malloc(n);
    if (!out) return NULL;
    memcpy(out, src, head);
    char *p = out + head;
    memcpy(p, renamed, sizeof renamed - 1), p += sizeof renamed - 1;
    memcpy(p, m + 11, tail), p += tail;
    *p++ = '\n';
    strcpy(p, hdr_lib);
    strcat(p, body);
    return out;
}

// ---- the window image
// pc.lum: x game white, y UI white, z peak (nits), w output (1 PQ, 2 scRGB); pc.opt: x tone mapper, y gamma (0 sRGB)
static const char *const encode_vert =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const encode_frag =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D comp;\n"
    "layout(push_constant) uniform P { vec4 lum; vec4 opt; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "vec3 spow(vec3 c, float e) { return sign(c) * pow(abs(c), vec3(e)); }\n"
    "vec3 srgb_dec(vec3 c) {\n"
    "  vec3 a = abs(c);\n"
    "  return sign(c) * mix(a / 12.92, pow((a + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), a));\n"
    "}\n"
    "vec3 to_bt2020(vec3 c) {\n"
    "  return vec3(dot(vec3(0.6274040, 0.3292820, 0.0433136), c), dot(vec3(0.0690970, 0.9195400, 0.0113612), c),\n"
    "              dot(vec3(0.0163916, 0.0880132, 0.8955950), c));\n"
    "}\n"
    "vec3 from_bt2020(vec3 c) {\n"
    "  return vec3(dot(vec3(1.6604910, -0.5876411, -0.0728499), c), dot(vec3(-0.1245505, 1.1328999, -0.0083494), c),\n"
    "              dot(vec3(-0.0181508, -0.1005789, 1.1187297), c));\n"
    "}\n"
    "vec3 pq(vec3 nits) {\n"
    "  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));\n"
    "  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));\n"
    "}\n"
    "void main() {\n"
    "  vec3 c = texelFetch(comp, ivec2(gl_FragCoord.xy), 0).rgb;\n"
    "  c = vec3(isnan(c.x) ? 0.0 : c.x, isnan(c.y) ? 0.0 : c.y, isnan(c.z) ? 0.0 : c.z);\n"
    "  bool vanilla = pc.opt.x < 0.5;\n"
    "  if (vanilla) c = clamp(c, 0.0, 1.0);\n"
    "  vec3 lin = pc.opt.y > 0.0 ? spow(c, pc.opt.y) : srgb_dec(c);\n"
    "  vec3 nits = to_bt2020(lin * (vanilla ? pc.lum.x : pc.lum.y));\n"
    "  nits = clamp(nits, 0.0, pc.lum.z);\n"
    "  if (pc.lum.w > 1.5) o = vec4(from_bt2020(nits) / 80.0, 1.0);\n"
    "  else o = vec4(pq(nits), 1.0);\n"
    "}\n";

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;

static VkPipeline encode_pipeline(VkRenderPass rp) {
    static VkPipeline p;
    static VkRenderPass made_for;
    if (p && made_for == rp) return p;
    if (!set_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
    }
    static VkShaderModule vsm, fs;
    if (!vsm) vsm = vk_compile_glsl(encode_vert, STAGE_VERTEX, "hdr_encode.vert");
    if (!fs) fs = vk_compile_glsl(encode_frag, STAGE_FRAGMENT, "hdr_encode.frag");
    if (!vsm || !fs) FATAL("the HDR output shaders failed to compile\n");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout, .renderPass = rp};
    if (p) vkDestroyPipeline(g_vk.dev, p, NULL);  // (a new window format: device.c waited for the GPU to be idle)
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    made_for = rp;
    return p;
}

void vk_hdr_encode(VkCommandBuffer cb, VkRenderPass rp, TexBackend *compose) {
    VkPipeline p = encode_pipeline(rp);
    VkDescriptorSet ds = vk_alloc_set(set_layout);
    static const VkComponentMapping id = {0};
    VkDescriptorImageInfo ii = {vk_default_sampler(false),
                                vk_image_view(compose, VK_IMAGE_VIEW_TYPE_2D, compose->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)compose->width, (float)compose->height, 0, 1};
    VkRect2D scissor = {{0, 0}, {compose->width, compose->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    float pc[8] = {config_live_hdr_game(), config_live_hdr_ui(), config_live_hdr_peak(), (float)g_hdr_out,
                   (float)config_live_hdr_tonemap(), gamma_value(), 0, 0};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
