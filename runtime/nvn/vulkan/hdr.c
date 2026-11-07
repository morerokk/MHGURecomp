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

#include <SDL3/SDL.h>

#include "../../hdr_stats.h"
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

// ---- the HDR analysis ([HDR] Analysis, hdr_stats.h): the presented picture measured by a compute pass before it is
// drawn into the window (device.c backend_present), every other pixel in each direction, with the window image's own
// maths (vk_hdr_encode); read back a frame or two later, without waiting. Its buffer (uints): [0] max Y, [1] min Y,
// [2] max CLL (float bits), [3..4] sum Y, [5..6] sum CLL (quarter nits, low / high), [7] pixels, [8..11] gamut counts
// (BT.709, P3, BT.2020, outside), [12] centre Y, [13] centre CLL, [16..] the waveform (HDR_WF_H x HDR_WF_W).
#define AN_WORDS (16 + HDR_WF_W * HDR_WF_H)
#define AN_SLOTS 3
static const char *const analysis_comp =
    "#version 450\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(std430, set = 0, binding = 1) buffer Out { uint w[]; };\n"
    "layout(push_constant) uniform P { vec4 lum; vec4 opt; } pc;\n"  // (as encode_frag's)
    "shared uint s_max, s_min, s_cll, s_sum, s_csum, s_n, s_g[4];\n"
    "vec3 spow(vec3 c, float e) { return sign(c) * pow(abs(c), vec3(e)); }\n"
    "vec3 srgb_dec(vec3 c) {\n"
    "  vec3 a = abs(c);\n"
    "  return sign(c) * mix(a / 12.92, pow((a + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), a));\n"
    "}\n"
    "vec3 to_bt2020(vec3 c) {\n"
    "  return vec3(dot(vec3(0.6274040, 0.3292820, 0.0433136), c), dot(vec3(0.0690970, 0.9195400, 0.0113612), c),\n"
    "              dot(vec3(0.0163916, 0.0880132, 0.8955950), c));\n"
    "}\n"
    "vec3 to_p3(vec3 c) {\n"
    "  return vec3(dot(vec3(0.8224621, 0.1775380, 0.0), c), dot(vec3(0.0331941, 0.9668058, 0.0), c),\n"
    "              dot(vec3(0.0170827, 0.0723974, 0.9105199), c));\n"
    "}\n"
    "void add64(uint at, uint v) { uint old = atomicAdd(w[at], v); if (old + v < old) atomicAdd(w[at + 1], 1u); }\n"
    "void main() {\n"
    "  bool first = gl_LocalInvocationIndex == 0u;\n"
    "  if (first) { s_max = 0u; s_min = 0x7f7fffffu; s_cll = 0u; s_sum = 0u; s_csum = 0u; s_n = 0u;\n"
    "               s_g[0] = 0u; s_g[1] = 0u; s_g[2] = 0u; s_g[3] = 0u; }\n"
    "  barrier();\n"
    "  ivec2 size = textureSize(src, 0);\n"
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy) * 2;\n"
    "  if (p.x < size.x && p.y < size.y) {\n"
    "    vec3 c = texelFetch(src, p, 0).rgb;\n"
    "    c = vec3(isnan(c.x) ? 0.0 : c.x, isnan(c.y) ? 0.0 : c.y, isnan(c.z) ? 0.0 : c.z);\n"
    "    bool vanilla = pc.opt.x < 0.5;\n"
    "    if (vanilla) c = clamp(c, 0.0, 1.0);\n"
    "    vec3 lin = pc.opt.y > 0.0 ? spow(c, pc.opt.y) : srgb_dec(c);\n"
    "    vec3 n2020 = to_bt2020(lin);\n"
    "    const float e = -1e-4;\n"
    "    int g = all(greaterThanEqual(lin, vec3(e))) ? 0 : all(greaterThanEqual(to_p3(lin), vec3(e))) ? 1 :\n"
    "            all(greaterThanEqual(n2020, vec3(e))) ? 2 : 3;\n"
    "    vec3 nits = clamp(n2020 * (vanilla ? pc.lum.x : pc.lum.y), 0.0, pc.lum.z);\n"
    "    float y = dot(nits, vec3(0.2627, 0.6780, 0.0593));\n"
    "    float cll = max(nits.r, max(nits.g, nits.b));\n"
    "    atomicMax(s_max, floatBitsToUint(y));\n"
    "    atomicMin(s_min, floatBitsToUint(y));\n"
    "    atomicMax(s_cll, floatBitsToUint(cll));\n"
    "    atomicAdd(s_sum, uint(y * 4.0 + 0.5));\n"
    "    atomicAdd(s_csum, uint(cll * 4.0 + 0.5));\n"
    "    atomicAdd(s_n, 1u);\n"
    "    atomicAdd(s_g[g], 1u);\n"
    "    int col = min(p.x * " "%d" " / size.x, " "%d" ");\n"
    "    float v = (log2(max(y, 1e-6)) * 0.30103 - (" "%f" ")) / (" "%f" ");\n"
    "    int row = clamp(int(v * " "%d" ".0), 0, " "%d" ");\n"
    "    atomicAdd(w[16 + row * " "%d" " + col], 1u);\n"
    "    if (p == (size / 4) * 2) { w[12] = floatBitsToUint(y); w[13] = floatBitsToUint(cll); }\n"
    "  }\n"
    "  barrier();\n"
    "  if (first && s_n > 0u) {\n"
    "    atomicMax(w[0], s_max); atomicMin(w[1], s_min); atomicMax(w[2], s_cll);\n"
    "    add64(3u, s_sum); add64(5u, s_csum); atomicAdd(w[7], s_n);\n"
    "    for (int i = 0; i < 4; i++) atomicAdd(w[8 + i], s_g[i]);\n"
    "  }\n"
    "}\n";

static VkDescriptorSetLayout an_set_layout;
static VkPipelineLayout an_layout;
static VkPipeline an_pipe;
static bool an_broken;
static struct { VkBuffer buf; VmaAllocation mem; uint32_t *ptr; uint64_t seq; bool pending; float set[5]; } an_slot[AN_SLOTS];
static int an_next;
static SDL_Mutex *an_lock;
static HdrStats an_out;     // (an_lock) the latest
static bool an_have;        // (an_lock)
static uint64_t an_frame;   // (an_lock) the frame it was measured in

static bool analysis_init(void) {
    if (an_pipe) return true;
    if (an_broken) return false;
    an_broken = true;  // (until it all worked)
    char src[8192];
    snprintf(src, sizeof src, analysis_comp, HDR_WF_W, HDR_WF_W - 1, (double)HDR_WF_LO, (double)(HDR_WF_HI - HDR_WF_LO),
             HDR_WF_H, HDR_WF_H - 1, HDR_WF_W);
    VkShaderModule m = vk_compile_glsl(src, STAGE_COMPUTE, "hdr_analysis.comp");
    if (!m) { LOG("[hdr] analysis: its shader did not compile: off\n"); return false; }
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &an_set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &an_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &an_layout));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = an_layout,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
    if (vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &an_pipe) != VK_SUCCESS) {
        LOG("[hdr] analysis: no pipeline: off\n");
        an_pipe = VK_NULL_HANDLE;
        return false;
    }
    for (int i = 0; i < AN_SLOTS; i++) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = AN_WORDS * 4,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &an_slot[i].buf, &an_slot[i].mem, &info) != VK_SUCCESS) {
            LOG("[hdr] analysis: no memory for it: off\n");
            return false;
        }
        an_slot[i].ptr = info.pMappedData;
    }
    an_lock = SDL_CreateMutex();
    an_broken = false;
    LOG("[hdr] analysis on\n");
    return true;
}


static void analysis_collect(void) {  // (render thread) the slots the GPU is done with -> an_out
    for (int k = 1; k <= AN_SLOTS; k++) {
        int i = (an_next - k + AN_SLOTS) % AN_SLOTS;  // (the newest first)
        if (!an_slot[i].pending || vk_completed_seq() < an_slot[i].seq) continue;
        vmaInvalidateAllocation(g_vk.vma, an_slot[i].mem, 0, VK_WHOLE_SIZE);
        const uint32_t *w = an_slot[i].ptr;
        for (int j = 0; j < AN_SLOTS; j++)
            if (an_slot[j].pending && an_slot[j].seq <= an_slot[i].seq) an_slot[j].pending = false;  // (older: dropped)
        if (!w[7]) return;
        SDL_LockMutex(an_lock);
        HdrStats *o = &an_out;
        double n = w[7];
        o->max_nits = u2f(w[0]), o->min_nits = u2f(w[1]), o->max_cll = u2f(w[2]);
        o->avg_nits = (float)(((double)w[4] * 4294967296.0 + w[3]) / 4.0 / n);
        o->avg_cll = (float)(((double)w[6] * 4294967296.0 + w[5]) / 4.0 / n);
        o->center_nits = u2f(w[12]), o->center_cll = u2f(w[13]);
        o->bt709 = (float)(100.0 * w[8] / n), o->p3 = (float)(100.0 * w[9] / n);
        o->bt2020 = (float)(100.0 * w[10] / n), o->outside = (float)(100.0 * w[11] / n);
        o->peak = an_slot[i].set[0], o->game = an_slot[i].set[1], o->ui = an_slot[i].set[2], o->pq = (int)an_slot[i].set[3];
        uint32_t mx = 0;
        for (int r = 0; r < HDR_WF_H; r++)
            for (int c = 0; c < HDR_WF_W; c++) {
                uint32_t v = w[16 + r * HDR_WF_W + c];
                o->wf[r][c] = v;
                if (v > mx) mx = v;
            }
        o->wf_max = mx;
        an_have = true;
        an_frame = g_present_count;
        SDL_UnlockMutex(an_lock);
        return;
    }
}

void vk_hdr_analyze(VkCommandBuffer cb, TexBackend *src) {
    if (!g_hdr_out || !config_live_hdr_analysis() || !src || !src->image || src->samples != 1) return;
    if (!analysis_init()) return;
    analysis_collect();
    int i = an_next;
    if (an_slot[i].pending) return;  // (the GPU is still on it: this frame not measured)
    VkDescriptorSet ds = vk_alloc_set(an_set_layout);
    static const VkComponentMapping id = {0};
    VkDescriptorImageInfo ii = {vk_default_sampler(false),
                                vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {an_slot[i].buf, 0, AN_WORDS * 4};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    vkCmdFillBuffer(cb, an_slot[i].buf, 0, AN_WORDS * 4, 0);
    vkCmdFillBuffer(cb, an_slot[i].buf, 4, 4, 0x7f7fffffu);  // (the minimum: from the largest float down)
    vk_full_barrier(cb);  // (the picture finished, the buffer cleared)
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, an_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, an_layout, 0, 1, &ds, 0, NULL);
    float pc[8] = {config_live_hdr_game(), config_live_hdr_ui(), config_live_hdr_peak(), (float)g_hdr_out,
                   (float)config_live_hdr_tonemap(), gamma_value(), 0, 0};
    vkCmdPushConstants(cb, an_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    uint32_t gx = (src->width / 2 + 15) / 16, gy = (src->height / 2 + 15) / 16;
    vkCmdDispatch(cb, gx ? gx : 1, gy ? gy : 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    an_slot[i].seq = vk_pending_seq();
    an_slot[i].pending = true;
    an_slot[i].set[0] = pc[2], an_slot[i].set[1] = pc[0], an_slot[i].set[2] = pc[1], an_slot[i].set[3] = g_hdr_out == HDR_OUT_PQ;
    an_next = (i + 1) % AN_SLOTS;
}

bool vk_hdr_stats(HdrStats *out) {
    if (!an_lock || !g_hdr_out || !config_live_hdr_analysis()) return false;
    SDL_LockMutex(an_lock);
    bool ok = an_have && an_frame + 30 >= g_present_count;  // (not a stale one: HDR went off and on, a menu, ...)
    if (ok) *out = an_out;
    SDL_UnlockMutex(an_lock);
    return ok;
}
