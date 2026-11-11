// The visual binding view (runtime/binding_view.h): a controller as line art, or a keyboard and a mouse, with the
// bound inputs lit, an arrow from each to a chip naming its actions, a glow on the selected input.
//
// How it works: the drawing is a set of polygons in screen space (Shape), built for a width and a font size and kept
// until those or the bindings change. Chips (the labels) are placed in the margins, side chips for the controller,
// tiers above and below for the keyboard, and every arrow is routed: a handful of candidate paths from a handful of
// anchor points on the input, scored by what they cross (other inputs, chips, other arrows), the best kept.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "binding_view.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"  // KEY_MOUSE, KEY_WHEEL_*
#include "keymap.h"  // key_reserved

#define PAD_BASE 0x2000  // runtime/actions.h ACT_PAD (kept as a constant here: the module needs no link to the runtime)
#define PAD_BTN(b) (PAD_BASE + (b))
#define PAD_LT (PAD_BASE + 0x40)
#define PAD_RT (PAD_BASE + 0x41)

namespace {
typedef std::vector<ImVec2> Poly;
const float PI = 3.14159265f;

// ---------------------------------------------------------------------------------------------- colours
ImU32 with_alpha(ImU32 c, float a) {
    int al = (int)(((c >> 24) & 255) * std::clamp(a, 0.f, 1.f) + .5f);
    return (c & 0x00FFFFFFu) | ((ImU32)al << 24);
}
float luminance(ImU32 c) {
    float r = (c & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = ((c >> 16) & 255) / 255.f;
    return .2126f * r + .7152f * g + .0722f * b;
}
ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) { return (int)(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t + .5f); };
    return (ImU32)(ch(0) | ch(8) << 8 | ch(16) << 16 | ch(24) << 24);
}

// ---------------------------------------------------------------------------------------------- geometry
float len(ImVec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }
ImVec2 norm(ImVec2 v) {
    float l = len(v);
    return l > 1e-6f ? ImVec2(v.x / l, v.y / l) : ImVec2(0, 0);
}
Poly circle_poly(ImVec2 c, float r) {
    int n = std::clamp((int)(r * 1.4f) + 20, 28, 140);
    Poly o;
    for (int i = 0; i < n; i++) {
        float a = 2 * PI * i / n;
        o.push_back(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r));
    }
    return o;
}
void add_bezier(Poly &o, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, int n) {
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        o.push_back(ImVec2(u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                           u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y));
    }
}
// A polygon with its corners rounded by radius r (a quadratic through each corner)
Poly round_poly(const Poly &p, float r) {
    Poly o;
    int n = (int)p.size();
    for (int i = 0; i < n; i++) {
        ImVec2 prev = p[(i + n - 1) % n], cur = p[i], next = p[(i + 1) % n];
        float l1 = len(prev - cur), l2 = len(next - cur);
        float t = std::min(r, std::min(l1, l2) * .5f);
        ImVec2 a = cur + norm(prev - cur) * t, b = cur + norm(next - cur) * t;
        int steps = std::clamp((int)(t * .9f) + 3, 4, 14);
        for (int k = 0; k <= steps; k++) {
            float s = (float)k / steps, u = 1 - s;
            o.push_back(ImVec2(u * u * a.x + 2 * u * s * cur.x + s * s * b.x, u * u * a.y + 2 * u * s * cur.y + s * s * b.y));
        }
    }
    return o;
}
Poly rrect_poly(ImVec2 lo, ImVec2 hi, float r) {
    return round_poly(Poly{lo, ImVec2(hi.x, lo.y), hi, ImVec2(lo.x, hi.y)}, r);
}
Poly rot_rect_poly(ImVec2 c, float length, float width, float deg, float r) {
    float a = deg * PI / 180.f;
    ImVec2 d(cosf(a), sinf(a)), n(-sinf(a), cosf(a));
    ImVec2 h = d * (length * .5f), w = n * (width * .5f);
    return round_poly(Poly{c - h - w, c + h - w, c + h + w, c - h + w}, r);
}
Poly wedge_poly(ImVec2 c, float r0, float r1, float a0, float a1) {
    Poly o;
    int n = std::clamp((int)(r1 * (a1 - a0) * .5f) + 4, 6, 40);
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * i / n;
        o.push_back(ImVec2(c.x + cosf(a) * r1, c.y + sinf(a) * r1));
    }
    for (int i = n; i >= 0; i--) {
        float a = a0 + (a1 - a0) * i / n;
        o.push_back(ImVec2(c.x + cosf(a) * r0, c.y + sinf(a) * r0));
    }
    return o;
}
// Points closer together than a pixel make the thick strokes (the glow) spike: drop them
void dedupe(Poly &p, float min_dist) {
    Poly o;
    for (const ImVec2 &q : p)
        if (o.empty() || len(q - o.back()) >= min_dist) o.push_back(q);
    while (o.size() > 2 && len(o.front() - o.back()) < min_dist) o.pop_back();
    p = o;
}
bool point_in_poly(ImVec2 p, const Poly &poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const ImVec2 &a = poly[i], &b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}
bool seg_seg(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
    auto cross = [](ImVec2 o, ImVec2 p, ImVec2 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    float d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

// ---------------------------------------------------------------------------------------------- the drawing
enum { SIDE_L, SIDE_R, SIDE_T, SIDE_B };
enum { ICON_NONE, ICON_UP, ICON_DOWN, ICON_LEFT, ICON_RIGHT, ICON_START, ICON_BACK };
struct Shape {
    int code = 0;
    Poly pts;
    bool convex = true, circle = false;
    float r = 0;
    ImVec2 c = {0, 0}, lo = {0, 0}, hi = {0, 0};
    std::vector<ImVec2> anchors;
    char label[12] = "";
    float tsize = 1.f;  // label size relative to the font
    int side = SIDE_L;
    int icon = ICON_NONE;
    bool reserved = false;
    const char *reserved_use = nullptr;
    bool decor = false;  // drawn, not bindable
    int group = 0;       // inputs of one group (a stick and its ring) do not hide each other's arrows
};
struct Decor {
    Poly pts;
    bool closed = true, fill = false;
    float alpha = 1.f, width = 1.f;
};
struct Chip {
    int code = 0, shape = -1, side = SIDE_L;
    std::string title;
    std::vector<std::string> lines;
    std::vector<int> ids;  // the ids of the lines' actions (the first two)
    int extra = 0, count = 0, tier = 0;
    bool conflict = false;
    ImVec2 pos = {0, 0}, size = {0, 0};
    Poly route;
    ImVec2 dot = {0, 0}, tip = {0, 0}, dir = {1, 0};
};
struct Layout {
    uint64_t key = 0;
    float W = 0, fs = 0, need_h = 0, diag_w = 0;
    int scheme = 0;
    std::vector<Shape> shapes;
    std::vector<Decor> decor;
    std::vector<Chip> chips;
    std::string extras;
};

void finish_shape(Shape &s) {
    ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (const ImVec2 &p : s.pts) {
        lo = ImVec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = ImVec2(std::max(hi.x, p.x), std::max(hi.y, p.y));
    }
    s.lo = lo, s.hi = hi;
    s.c = (lo + hi) * .5f;
}
// The anchors of a shape: where rays from its centre leave it, in `n` directions (the first is straight right, so
// the four edge middles of a rectangle are among them when n is a multiple of 4)
void anchors_dirs(Shape &s, int n) {
    s.anchors.clear();
    ImVec2 c(0, 0);
    for (const ImVec2 &p : s.pts) c = c + p;
    c = c * (1.f / (float)s.pts.size());
    size_t cnt = s.pts.size();
    for (int k = 0; k < n; k++) {
        float a = 2 * PI * k / n;
        ImVec2 d(cosf(a), sinf(a));
        float best = -1;
        ImVec2 hit = c;
        for (size_t i = 0; i < cnt; i++) {
            ImVec2 p = s.pts[i], q = s.pts[(i + 1) % cnt], e = q - p;
            float den = d.x * e.y - d.y * e.x;
            if (fabsf(den) < 1e-6f) continue;
            ImVec2 w = p - c;
            float t = (w.x * e.y - w.y * e.x) / den, u = (w.x * d.y - w.y * d.x) / den;
            if (t > 0 && u >= 0 && u <= 1 && t > best) best = t, hit = c + d * t;
        }
        if (best > 0) s.anchors.push_back(hit);
    }
    if (s.anchors.empty()) s.anchors.push_back(s.c);
}
void anchors_rect(Shape &s) { anchors_dirs(s, 16); }
void anchors_ring(Shape &s, int) { anchors_dirs(s, 16); }
void anchors_poly(Shape &s, int) { anchors_dirs(s, 16); }

// ---------------------------------------------------------------------------------------------- names
const char *pad_name(int code) {
    switch (code) {
    case PAD_BTN(SDL_GAMEPAD_BUTTON_SOUTH): return "South button";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_EAST): return "East button";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_WEST): return "West button";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_NORTH): return "North button";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER): return "L1 (bumper)";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER): return "R1 (bumper)";
    case PAD_LT: return "L2 (trigger)";
    case PAD_RT: return "R2 (trigger)";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_LEFT_STICK): return "L3 (stick click)";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_RIGHT_STICK): return "R3 (stick click)";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_UP): return "D-pad up";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_DOWN): return "D-pad down";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_LEFT): return "D-pad left";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_RIGHT): return "D-pad right";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_START): return "Start";
    case PAD_BTN(SDL_GAMEPAD_BUTTON_BACK): return "Back";
    case BV_STICK_L_UP: return "Left stick up";
    case BV_STICK_L_DOWN: return "Left stick down";
    case BV_STICK_L_LEFT: return "Left stick left";
    case BV_STICK_L_RIGHT: return "Left stick right";
    case BV_STICK_R_UP: return "Right stick up";
    case BV_STICK_R_DOWN: return "Right stick down";
    case BV_STICK_R_LEFT: return "Right stick left";
    case BV_STICK_R_RIGHT: return "Right stick right";
    }
    return nullptr;
}
}  // namespace

void bv_input_name(int code, char *buf, int size) {
    if (code >= PAD_BASE && code < PAD_BASE + 0x80) {
        const char *n = pad_name(code);
        snprintf(buf, size, "%s", n ? n : "Pad button");
    } else if (code >= KEY_MOUSE) {
        switch (code) {
        case KEY_MOUSE + SDL_BUTTON_LEFT: snprintf(buf, size, "Mouse left"); break;
        case KEY_MOUSE + SDL_BUTTON_RIGHT: snprintf(buf, size, "Mouse right"); break;
        case KEY_MOUSE + SDL_BUTTON_MIDDLE: snprintf(buf, size, "Mouse middle"); break;
        case KEY_MOUSE + SDL_BUTTON_X1: snprintf(buf, size, "Mouse button 4"); break;
        case KEY_MOUSE + SDL_BUTTON_X2: snprintf(buf, size, "Mouse button 5"); break;
        case KEY_WHEEL_UP: snprintf(buf, size, "Wheel up"); break;
        case KEY_WHEEL_DOWN: snprintf(buf, size, "Wheel down"); break;
        default: snprintf(buf, size, "Mouse %d", code - KEY_MOUSE);
        }
    } else {
        const char *n = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(buf, size, "%s", n && *n ? n : "Key");
    }
}

BVColors bv_colors_from(ImU32 panel, ImU32 line, ImU32 text, ImU32 text_dim, ImU32 accent, ImU32 accent_bright, ImU32 warn,
                        ImU32 live) {
    return BVColors{panel, line, text, text_dim, accent, accent_bright, warn, live};
}

namespace {
// ---------------------------------------------------------------------------------------------- the controller
const float CW = 1000;  // the controller's own units

struct Builder {
    Layout &L;
    ImVec2 O;
    float S;
    ImVec2 P(float x, float y) const { return ImVec2(O.x + x * S, O.y + y * S); }
    float R(float v) const { return v * S; }
    Shape &add(int code, Poly pts, const char *label, int side, bool convex = true) {
        L.shapes.emplace_back();
        Shape &s = L.shapes.back();
        s.code = code, s.pts = std::move(pts), s.convex = convex, s.side = side;
        snprintf(s.label, sizeof s.label, "%s", label);
        finish_shape(s);
        return s;
    }
    void circle(int code, float x, float y, float r, const char *label, int side, float tsize = 1.f, int icon = ICON_NONE) {
        Shape &s = add(code, circle_poly(P(x, y), R(r)), label, side);
        s.circle = true, s.r = R(r), s.c = P(x, y), s.tsize = tsize, s.icon = icon;
        anchors_ring(s, 12);
    }
};

void build_controller(Layout &L, float fs, float W, float chip_w, float gutter, float pad, bool columns) {
    float dw = W - 2 * (chip_w + gutter);
    float S = std::min(dw / CW, .085f * fs);
    S = std::max(S, .12f);
    Builder b{L, ImVec2((W - CW * S) * .5f, pad), S};
    auto mirror = [](float x) { return CW - x; };

    // the body: the right half from the top middle to the bottom middle, then the left half back
    Poly right{b.P(500, 112)};
    add_bezier(right, b.P(500, 112), b.P(650, 112), b.P(770, 126), b.P(842, 172), 28);
    add_bezier(right, b.P(842, 172), b.P(952, 214), b.P(1004, 420), b.P(958, 528), 34);
    add_bezier(right, b.P(958, 528), b.P(944, 594), b.P(868, 598), b.P(840, 572), 16);
    add_bezier(right, b.P(840, 572), b.P(790, 545), b.P(760, 497), b.P(690, 492), 22);
    add_bezier(right, b.P(690, 492), b.P(620, 488), b.P(560, 502), b.P(500, 502), 20);
    Poly body = right;
    for (int i = (int)right.size() - 2; i >= 1; i--) body.push_back(ImVec2(2 * b.P(500, 0).x - right[i].x, right[i].y));
    Decor d;
    d.pts = body, d.fill = true, d.alpha = .55f, d.width = 1.4f;
    L.decor.push_back(d);
    // the wells of the sticks and the hub
    for (ImVec2 c : {ImVec2(235, 255), ImVec2(645, 385)}) {
        Decor w;
        w.pts = circle_poly(b.P(c.x, c.y), b.R(92)), w.alpha = .35f, w.width = 1.f;
        L.decor.push_back(w);
    }
    {
        Decor w;
        w.pts = circle_poly(b.P(500, 214), b.R(25)), w.alpha = .45f, w.width = 1.2f;
        L.decor.push_back(w);
        Decor w2;
        w2.pts = circle_poly(b.P(500, 214), b.R(13)), w2.alpha = .35f, w2.width = 1.f;
        L.decor.push_back(w2);
    }

    // triggers and bumpers
    auto top = [&](int code, Poly p, const char *label, int side) { Shape &s = b.add(code, std::move(p), label, side); s.tsize = .9f; anchors_poly(s, 10); anchors_rect(s); };
    top(PAD_LT, rot_rect_poly(b.P(262, 50), b.R(214), b.R(56), -11, b.R(16)), "L2", SIDE_L);
    top(PAD_RT, rot_rect_poly(b.P(mirror(262), 50), b.R(214), b.R(56), 11, b.R(16)), "R2", SIDE_R);
    top(PAD_BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), rot_rect_poly(b.P(292, 120), b.R(236), b.R(34), -15, b.R(13)), "L1", SIDE_L);
    top(PAD_BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), rot_rect_poly(b.P(mirror(292), 120), b.R(236), b.R(34), 15, b.R(13)), "R1", SIDE_R);

    // sticks: the cap is the click, the ring around it the four directions
    int next_group = 1;
    auto stick = [&](float x, float y, int side, int up, int down, int left, int right, int click, const char *lab) {
        int grp = next_group++;
        b.circle(click, x, y, 50, lab, side, .95f);
        L.shapes.back().group = grp;
        struct { int code; float a0, a1; } w[4] = {{right, -PI / 4, PI / 4}, {down, PI / 4, 3 * PI / 4}, {left, 3 * PI / 4, 5 * PI / 4}, {up, 5 * PI / 4, 7 * PI / 4}};
        for (auto &q : w) {
            float gap = .07f;
            Shape &s = b.add(q.code, wedge_poly(b.P(x, y), b.R(60), b.R(82), q.a0 + gap, q.a1 - gap), "", side, false);
            anchors_poly(s, 6);
            s.group = grp;
        }
    };
    stick(235, 255, SIDE_L, BV_STICK_L_UP, BV_STICK_L_DOWN, BV_STICK_L_LEFT, BV_STICK_L_RIGHT, PAD_BTN(SDL_GAMEPAD_BUTTON_LEFT_STICK), "L3");
    stick(645, 385, SIDE_R, BV_STICK_R_UP, BV_STICK_R_DOWN, BV_STICK_R_LEFT, BV_STICK_R_RIGHT, PAD_BTN(SDL_GAMEPAD_BUTTON_RIGHT_STICK), "R3");

    // d-pad: four arms and the square between them
    {
        float x = 355, y = 385;
        Decor c;
        c.pts = rrect_poly(b.P(x - 20, y - 20), b.P(x + 20, y + 20), b.R(4)), c.fill = true, c.alpha = .35f;
        L.decor.push_back(c);
        struct { int code; ImVec2 lo, hi; int icon; } arms[4] = {
            {PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_UP), ImVec2(x - 19, y - 78), ImVec2(x + 19, y - 24), ICON_UP},
            {PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_DOWN), ImVec2(x - 19, y + 24), ImVec2(x + 19, y + 78), ICON_DOWN},
            {PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_LEFT), ImVec2(x - 78, y - 19), ImVec2(x - 24, y + 19), ICON_LEFT},
            {PAD_BTN(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), ImVec2(x + 24, y - 19), ImVec2(x + 78, y + 19), ICON_RIGHT}};
        for (auto &a : arms) {
            bool below = a.icon == ICON_DOWN || a.icon == ICON_RIGHT;  // (their chips go under the drawing: nothing in the way)
            Shape &s = b.add(a.code, rrect_poly(b.P(a.lo.x, a.lo.y), b.P(a.hi.x, a.hi.y), b.R(8)), "", below ? SIDE_B : SIDE_L);
            s.icon = a.icon;
            anchors_rect(s);
        }
    }
    // face buttons, named by position
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_NORTH), 765, 188, 33, "N", SIDE_R, 1.15f);
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_WEST), 700, 253, 33, "W", SIDE_R, 1.15f);
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_EAST), 830, 253, 33, "E", SIDE_R, 1.15f);
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_SOUTH), 765, 318, 33, "S", SIDE_R, 1.15f);
    // centre
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_BACK), 432, 272, 17, "", SIDE_B, 1.f, ICON_BACK);
    b.circle(PAD_BTN(SDL_GAMEPAD_BUTTON_START), 568, 272, 17, "", SIDE_B, 1.f, ICON_START);
    if (!columns)  // (a narrow drawing: chips above and below instead of beside: the upper half of the controller above)
        for (Shape &sh : L.shapes) sh.side = sh.c.y < b.O.y + 262 * S ? SIDE_T : SIDE_B;
}

// ---------------------------------------------------------------------------------------------- the keyboard
struct KeyDef {
    int sc;
    float x, y, w, h;
    const char *label;
};
const char *LETTERS[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                           "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
const char *DIGITS[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char *FKEYS[12] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
const float KBW = 22.9f, KBH = 6.5f;  // the keyboard's units (the numpad included)

void keyboard_keys(std::vector<KeyDef> &o) {
    auto add = [&](int sc, float x, float y, float w, const char *l, float h = 1) { o.push_back({sc, x, y, w, h, l}); };
    add(SDL_SCANCODE_ESCAPE, 0, 0, 1, "Esc");
    for (int i = 0; i < 12; i++) add(SDL_SCANCODE_F1 + i, 2 + i + (i / 4) * .5f, 0, 1, FKEYS[i]);
    add(SDL_SCANCODE_PRINTSCREEN, 15.5f, 0, 1, "Prt");
    add(SDL_SCANCODE_SCROLLLOCK, 16.5f, 0, 1, "Scr");
    add(SDL_SCANCODE_PAUSE, 17.5f, 0, 1, "Pau");
    // numbers
    add(SDL_SCANCODE_GRAVE, 0, 1.5f, 1, "`");
    for (int i = 0; i < 10; i++) add(i < 9 ? SDL_SCANCODE_1 + i : SDL_SCANCODE_0, 1 + i, 1.5f, 1, DIGITS[i]);
    add(SDL_SCANCODE_MINUS, 11, 1.5f, 1, "-");
    add(SDL_SCANCODE_EQUALS, 12, 1.5f, 1, "=");
    add(SDL_SCANCODE_BACKSPACE, 13, 1.5f, 2, "Back");
    // qwerty
    add(SDL_SCANCODE_TAB, 0, 2.5f, 1.5f, "Tab");
    static const int row2[] = {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T,
                               SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P};
    for (int i = 0; i < 10; i++) add(row2[i], 1.5f + i, 2.5f, 1, LETTERS[row2[i] - SDL_SCANCODE_A]);
    add(SDL_SCANCODE_LEFTBRACKET, 11.5f, 2.5f, 1, "[");
    add(SDL_SCANCODE_RIGHTBRACKET, 12.5f, 2.5f, 1, "]");
    add(SDL_SCANCODE_BACKSLASH, 13.5f, 2.5f, 1.5f, "\\");
    // home row
    add(SDL_SCANCODE_CAPSLOCK, 0, 3.5f, 1.75f, "Caps");
    static const int row3[] = {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G,
                               SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L};
    for (int i = 0; i < 9; i++) add(row3[i], 1.75f + i, 3.5f, 1, LETTERS[row3[i] - SDL_SCANCODE_A]);
    add(SDL_SCANCODE_SEMICOLON, 10.75f, 3.5f, 1, ";");
    add(SDL_SCANCODE_APOSTROPHE, 11.75f, 3.5f, 1, "'");
    add(SDL_SCANCODE_RETURN, 12.75f, 3.5f, 2.25f, "Enter");
    // bottom letters
    add(SDL_SCANCODE_LSHIFT, 0, 4.5f, 2.25f, "Shift");
    static const int row4[] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B,
                               SDL_SCANCODE_N, SDL_SCANCODE_M};
    for (int i = 0; i < 7; i++) add(row4[i], 2.25f + i, 4.5f, 1, LETTERS[row4[i] - SDL_SCANCODE_A]);
    add(SDL_SCANCODE_COMMA, 9.25f, 4.5f, 1, ",");
    add(SDL_SCANCODE_PERIOD, 10.25f, 4.5f, 1, ".");
    add(SDL_SCANCODE_SLASH, 11.25f, 4.5f, 1, "/");
    add(SDL_SCANCODE_RSHIFT, 12.25f, 4.5f, 2.75f, "Shift");
    // space row
    add(SDL_SCANCODE_LCTRL, 0, 5.5f, 1.25f, "Ctrl");
    add(SDL_SCANCODE_LGUI, 1.25f, 5.5f, 1.25f, "Win");
    add(SDL_SCANCODE_LALT, 2.5f, 5.5f, 1.25f, "Alt");
    add(SDL_SCANCODE_SPACE, 3.75f, 5.5f, 6.25f, "Space");
    add(SDL_SCANCODE_RALT, 10, 5.5f, 1.25f, "Alt");
    add(SDL_SCANCODE_RGUI, 11.25f, 5.5f, 1.25f, "Win");
    add(SDL_SCANCODE_APPLICATION, 12.5f, 5.5f, 1.25f, "Menu");
    add(SDL_SCANCODE_RCTRL, 13.75f, 5.5f, 1.25f, "Ctrl");
    // navigation
    add(SDL_SCANCODE_INSERT, 15.5f, 1.5f, 1, "Ins");
    add(SDL_SCANCODE_HOME, 16.5f, 1.5f, 1, "Home");
    add(SDL_SCANCODE_PAGEUP, 17.5f, 1.5f, 1, "PgUp");
    add(SDL_SCANCODE_DELETE, 15.5f, 2.5f, 1, "Del");
    add(SDL_SCANCODE_END, 16.5f, 2.5f, 1, "End");
    add(SDL_SCANCODE_PAGEDOWN, 17.5f, 2.5f, 1, "PgDn");
    add(SDL_SCANCODE_UP, 16.5f, 4.5f, 1, "");
    add(SDL_SCANCODE_LEFT, 15.5f, 5.5f, 1, "");
    add(SDL_SCANCODE_DOWN, 16.5f, 5.5f, 1, "");
    add(SDL_SCANCODE_RIGHT, 17.5f, 5.5f, 1, "");
    // numpad
    const float nx = 19.f;
    add(SDL_SCANCODE_NUMLOCKCLEAR, nx, 1.5f, 1, "Num");
    add(SDL_SCANCODE_KP_DIVIDE, nx + 1, 1.5f, 1, "/");
    add(SDL_SCANCODE_KP_MULTIPLY, nx + 2, 1.5f, 1, "*");
    add(SDL_SCANCODE_KP_MINUS, nx + 3, 1.5f, 1, "-");
    add(SDL_SCANCODE_KP_7, nx, 2.5f, 1, "7");
    add(SDL_SCANCODE_KP_8, nx + 1, 2.5f, 1, "8");
    add(SDL_SCANCODE_KP_9, nx + 2, 2.5f, 1, "9");
    add(SDL_SCANCODE_KP_PLUS, nx + 3, 2.5f, 1, "+", 2);
    add(SDL_SCANCODE_KP_4, nx, 3.5f, 1, "4");
    add(SDL_SCANCODE_KP_5, nx + 1, 3.5f, 1, "5");
    add(SDL_SCANCODE_KP_6, nx + 2, 3.5f, 1, "6");
    add(SDL_SCANCODE_KP_1, nx, 4.5f, 1, "1");
    add(SDL_SCANCODE_KP_2, nx + 1, 4.5f, 1, "2");
    add(SDL_SCANCODE_KP_3, nx + 2, 4.5f, 1, "3");
    add(SDL_SCANCODE_KP_ENTER, nx + 3, 4.5f, 1, "Ent", 2);
    add(SDL_SCANCODE_KP_0, nx, 5.5f, 2, "0");
    add(SDL_SCANCODE_KP_PERIOD, nx + 2, 5.5f, 1, ".");
}

// ---------------------------------------------------------------------------------------------- text
ImVec2 text_size(float size, const char *s) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, s); }
std::string fit(const std::string &s, float size, float maxw) {
    if (text_size(size, s.c_str()).x <= maxw) return s;
    std::string t = s;
    while (t.size() > 1) {
        t.pop_back();
        std::string u = t + "..";
        if (text_size(size, u.c_str()).x <= maxw) return u;
    }
    return t;
}
uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
uint64_t layout_key(const BVParams &p, float W) {
    uint64_t h = 1469598103934665603ull;
    int w = (int)W, f = (int)(p.font_px * 10);
    int mh = (int)p.max_height;
    h = fnv(h, &p.scheme, sizeof p.scheme), h = fnv(h, &w, sizeof w), h = fnv(h, &f, sizeof f), h = fnv(h, &mh, sizeof mh);
    for (int i = 0; i < p.n_inputs; i++) {
        const BVInput &in = p.inputs[i];
        h = fnv(h, &in.code, sizeof in.code), h = fnv(h, &in.n, sizeof in.n), h = fnv(h, &in.conflict, sizeof in.conflict);
        for (int k = 0; k < in.n; k++) {
            h = fnv(h, &in.actions[k].id, sizeof(int));
            h = fnv(h, in.actions[k].label, strlen(in.actions[k].label));
        }
    }
    return h;
}

// ---------------------------------------------------------------------------------------------- chips and routes
float chip_line_h(float fs) { return fs * 1.2f; }
float chip_pad(float fs) { return fs * .42f; }

void make_chip(Chip &c, const BVInput &in, float fs, float max_w, int max_lines) {
    char nm[64];
    bv_input_name(in.code, nm, sizeof nm);
    c.code = in.code, c.title = nm, c.count = in.n, c.conflict = in.conflict;
    float ts = fs * .78f, as = fs * .95f, pad = chip_pad(fs);
    float w = text_size(ts, c.title.c_str()).x;
    int show = std::min(in.n, max_lines);
    for (int i = 0; i < show; i++) {
        c.lines.push_back(in.actions[i].label);
        c.ids.push_back(in.actions[i].id);
        w = std::max(w, text_size(as, in.actions[i].label).x + fs * .5f);
    }
    c.extra = in.n - show;
    if (c.extra > 0) {  // (the count sits at the end of the last line)
        char more[24];
        snprintf(more, sizeof more, "+%d", c.extra);
        w = std::max(w, text_size(as, in.actions[show - 1].label).x + fs * .5f + text_size(ts, more).x + fs * .5f);
    }
    float badge = in.conflict ? fs * 1.3f : 0;
    w = std::max(w, text_size(ts, c.title.c_str()).x + badge);
    c.size.x = std::min(max_w, std::max(w + 2 * pad, fs * 6.f));
    int lines = 1 + show;
    c.size.y = 2 * pad + lines * chip_line_h(fs) - (lines > 1 ? fs * .12f : 0);
    float room = c.size.x - 2 * pad;
    for (size_t i = 0; i < c.lines.size(); i++) {
        float r = room;
        if (c.extra > 0 && i + 1 == c.lines.size()) r -= text_size(ts, "+99").x + fs * .4f;
        c.lines[i] = fit(c.lines[i], as, r);
    }
    c.title = fit(c.title, ts, c.size.x - 2 * pad - badge);
}

// two segments on one horizontal or vertical line, sharing some length (arrows must not run over each other)
bool runs_along(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
    if (fabsf(a.y - b.y) < .5f && fabsf(c.y - d.y) < .5f && fabsf(a.y - c.y) < 3.f)
        return std::min(std::max(a.x, b.x), std::max(c.x, d.x)) - std::max(std::min(a.x, b.x), std::min(c.x, d.x)) > 1.f;
    if (fabsf(a.x - b.x) < .5f && fabsf(c.x - d.x) < .5f && fabsf(a.x - c.x) < 3.f)
        return std::min(std::max(a.y, b.y), std::max(c.y, d.y)) - std::max(std::min(a.y, b.y), std::min(c.y, d.y)) > 1.f;
    return false;
}
struct Router {
    Layout &L;
    float fs;
    std::vector<Poly> done;
    bool hits_rect(ImVec2 a, ImVec2 b, ImVec2 lo, ImVec2 hi) const {
        if (std::max(a.x, b.x) < lo.x || std::min(a.x, b.x) > hi.x || std::max(a.y, b.y) < lo.y || std::min(a.y, b.y) > hi.y)
            return false;
        auto in = [&](ImVec2 p) { return p.x > lo.x && p.x < hi.x && p.y > lo.y && p.y < hi.y; };
        if (in(a) || in(b)) return true;
        ImVec2 c[4] = {lo, ImVec2(hi.x, lo.y), hi, ImVec2(lo.x, hi.y)};
        for (int i = 0; i < 4; i++)
            if (seg_seg(a, b, c[i], c[(i + 1) % 4])) return true;
        return false;
    }
    bool hits_shape(ImVec2 a, ImVec2 b, const Shape &s) const {
        if (std::max(a.x, b.x) < s.lo.x || std::min(a.x, b.x) > s.hi.x || std::max(a.y, b.y) < s.lo.y || std::min(a.y, b.y) > s.hi.y)
            return false;
        if (point_in_poly((a + b) * .5f, s.pts)) return true;
        size_t n = s.pts.size();
        for (size_t i = 0; i < n; i++)
            if (seg_seg(a, b, s.pts[i], s.pts[(i + 1) % n])) return true;
        return false;
    }
    float cost(const Poly &r, int own_shape, int own_chip) const {
        float c = 0, length = 0;
        for (size_t i = 0; i + 1 < r.size(); i++) {
            ImVec2 a = r[i], b = r[i + 1];
            length += len(b - a);
            for (size_t k = 0; k < L.shapes.size(); k++) {
                if ((int)k == own_shape || L.shapes[k].decor) continue;
                if (own_shape >= 0 && L.shapes[own_shape].group && L.shapes[k].group == L.shapes[own_shape].group) continue;
                if (hits_shape(a, b, L.shapes[k])) c += 1000;
            }
            if (own_shape >= 0 && i > 0) {  // (past the first segment the arrow does not go back over its own input)
                if (hits_shape(a, b, L.shapes[own_shape])) c += 600;
            }
            for (size_t k = 0; k < L.chips.size(); k++) {
                if ((int)k == own_chip) continue;
                const Chip &o = L.chips[k];
                if (hits_rect(a, b, o.pos - ImVec2(2, 2), o.pos + o.size + ImVec2(2, 2))) c += 1000;
            }
            for (const Poly &d : done)
                for (size_t j = 0; j + 1 < d.size(); j++) {
                    if (seg_seg(a, b, d[j], d[j + 1])) c += 90;
                    if (runs_along(a, b, d[j], d[j + 1])) c += 500;
                }
        }
        return c + length * .02f + (float)(r.size() - 2) * 3.f;
    }
    // the corners of an orthogonal path cut at 45 degrees
    Poly chamfer(const Poly &r) const {
        if (r.size() < 3) return r;
        Poly o{r[0]};
        float q = fs * .7f;
        for (size_t i = 1; i + 1 < r.size(); i++) {
            ImVec2 p = r[i], a = r[i - 1], n = r[i + 1];
            float t = std::min(q, std::min(len(a - p), len(n - p)) * .5f);
            o.push_back(p + norm(a - p) * t);
            o.push_back(p + norm(n - p) * t);
        }
        o.push_back(r.back());
        return o;
    }
    void route(Chip &c, int chip_index) {
        const Shape &sh = L.shapes[c.shape];
        float rr = fs * .5f;
        Poly best;
        float best_cost = FLT_MAX;
        ImVec2 best_dot, best_tip;
        for (const ImVec2 &a : sh.anchors) {
            ImVec2 t;
            switch (c.side) {
            case SIDE_L: t = ImVec2(c.pos.x + c.size.x, std::clamp(a.y, c.pos.y + rr, c.pos.y + c.size.y - rr)); break;
            case SIDE_R: t = ImVec2(c.pos.x, std::clamp(a.y, c.pos.y + rr, c.pos.y + c.size.y - rr)); break;
            case SIDE_T: t = ImVec2(std::clamp(a.x, c.pos.x + rr, c.pos.x + c.size.x - rr), c.pos.y + c.size.y); break;
            default: t = ImVec2(std::clamp(a.x, c.pos.x + rr, c.pos.x + c.size.x - rr), c.pos.y); break;
            }
            std::vector<Poly> cand;
            cand.push_back({a, t});
            bool horiz = c.side == SIDE_L || c.side == SIDE_R;
            if (horiz) {
                cand.push_back({a, ImVec2(a.x, t.y), t});
                for (float f : {.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f}) {
                    float lx = a.x + (t.x - a.x) * f;
                    cand.push_back({a, ImVec2(lx, a.y), ImVec2(lx, t.y), t});
                }
            } else {
                cand.push_back({a, ImVec2(t.x, a.y), t});
                for (float f : {.12f, .22f, .32f, .42f, .52f, .62f, .72f, .82f, .9f}) {
                    float ly = a.y + (t.y - a.y) * f;
                    cand.push_back({a, ImVec2(a.x, ly), ImVec2(t.x, ly), t});
                }
            }
            for (Poly &r : cand) {
                // (drop zero-length steps)
                Poly q;
                for (const ImVec2 &p : r)
                    if (q.empty() || len(p - q.back()) > .5f) q.push_back(p);
                if (q.size() < 2) continue;
                float cst = cost(q, c.shape, chip_index);
                bool straight = q.size() == 2;
                if (straight && fabsf(q[0].x - q[1].x) > 1.f && fabsf(q[0].y - q[1].y) > 1.f) cst += 250;  // (a slanted line)
                if (cst < best_cost) best_cost = cst, best = q, best_dot = a, best_tip = t;
            }
        }
        c.dot = best_dot, c.tip = best_tip;
        c.route = chamfer(best);
        if (c.route.size() >= 2) c.dir = norm(c.route.back() - c.route[c.route.size() - 2]);
        done.push_back(c.route);
    }
};

// ---------------------------------------------------------------------------------------------- layout
Layout g_cache[2];

void relax_column(std::vector<Chip *> &col, float gap, float lo) {
    std::sort(col.begin(), col.end(), [](Chip *a, Chip *b) { return a->pos.y + a->size.y * .5f < b->pos.y + b->size.y * .5f; });
    float y = lo;
    for (Chip *c : col) {
        c->pos.y = std::max(c->pos.y, y);
        y = c->pos.y + c->size.y + gap;
    }
}
void shift_all(Layout &L, float dy) {
    for (Shape &s : L.shapes) {
        for (ImVec2 &p : s.pts) p.y += dy;
        for (ImVec2 &p : s.anchors) p.y += dy;
        s.c.y += dy, s.lo.y += dy, s.hi.y += dy;
    }
    for (Decor &d : L.decor)
        for (ImVec2 &p : d.pts) p.y += dy;
    for (Chip &c : L.chips) c.pos.y += dy;
}

// Packs the chips of one side of a drawing (above or below it) into tiers: each chip as near to the middle of its input
// as it can be, tiers that fill up are stacked, and an arrow up a tier is not asked to pass through the chips of the
// tiers below. Returns the height of each tier.
std::vector<float> pack_row(Layout &L, std::vector<Chip *> &grp_g, float W, float pad, float gap, float fs) {
    std::vector<float> tier_h;
            std::sort(grp_g.begin(), grp_g.end(), [&](Chip *a, Chip *b) { return L.shapes[a->shape].c.x < L.shapes[b->shape].c.x; });
            std::vector<float> right;  // where each tier is filled to
            for (Chip *c : grp_g) {
                float want = std::clamp(L.shapes[c->shape].c.x - c->size.x * .5f, pad * .5f, std::max(pad * .5f, W - pad * .5f - c->size.x));
                int bt = -1;
                float bx = want, bd = FLT_MAX;
                float ax = L.shapes[c->shape].c.x;
                for (size_t t = 0; t < right.size() + 1 && t < 8; t++) {
                    float x = t < right.size() ? std::max(want, right[t] + gap) : want;
                    if (x + c->size.x > W - pad * .5f) continue;
                    float d = fabsf(x - want) + (float)t * fs * 12.f;
                    for (Chip *o : grp_g) {  // (an arrow goes up past the chips of the tiers below its own)
                        if (o == c) break;
                        float oax = L.shapes[o->shape].c.x;
                        if (o->tier < (int)t && ax >= o->pos.x && ax <= o->pos.x + o->size.x) d += 400;
                        if (o->tier > (int)t && oax >= x && oax <= x + c->size.x) d += 400;
                    }
                    if (d < bd) bd = d, bt = (int)t, bx = x;
                }
                if (bt < 0) {  // (no room anywhere: the last tier, pushed left)
                    bt = (int)std::min<size_t>(right.size(), 7);
                    bx = std::max(pad * .5f, W - pad * .5f - c->size.x);
                }
                if (bt >= (int)right.size()) right.resize(bt + 1, -1e9f);
                if (bt >= (int)tier_h.size()) tier_h.resize(bt + 1, 0.f);
                c->tier = bt, c->pos.x = bx;
                right[bt] = bx + c->size.x;
                tier_h[bt] = std::max(tier_h[bt], c->size.y);
            }
            // a tier that ran past the right edge is pulled back, then pushed off the left edge again
            for (size_t t = 0; t < right.size(); t++) {
                std::vector<Chip *> row;
                for (Chip *c : grp_g)
                    if (c->tier == (int)t) row.push_back(c);
                for (int i = (int)row.size() - 1; i >= 0; i--) {
                    float lim = i + 1 < (int)row.size() ? row[i + 1]->pos.x - gap : W - pad * .5f;
                    row[i]->pos.x = std::min(row[i]->pos.x, lim - row[i]->size.x);
                }
                for (size_t i = 0; i < row.size(); i++) {
                    float lim = i ? row[i - 1]->pos.x + row[i - 1]->size.x + gap : pad * .5f;
                    row[i]->pos.x = std::max(row[i]->pos.x, lim);
                }
            }
    // a tier that was pushed to one side as a whole is slid back toward the middle of its inputs
    for (size_t t = 0; t < tier_h.size(); t++) {
        std::vector<Chip *> row;
        for (Chip *c : grp_g)
            if (c->tier == (int)t) row.push_back(c);
        if (row.empty()) continue;
        std::sort(row.begin(), row.end(), [](Chip *a, Chip *b) { return a->pos.x < b->pos.x; });
        float sum = 0;
        for (Chip *c : row) sum += L.shapes[c->shape].c.x - (c->pos.x + c->size.x * .5f);
        float delta = sum / (float)row.size();
        delta = std::max(delta, pad * .5f - row.front()->pos.x);
        delta = std::min(delta, W - pad * .5f - (row.back()->pos.x + row.back()->size.x));
        for (Chip *c : row) c->pos.x += delta;
    }
    return tier_h;
}

// mode 0: the controller's chips beside it when the width allows, 1 beside it, 2 above and below
void build_layout(Layout &L, const BVParams &p, float W, int mode) {
    float fs = p.font_px;
    L = Layout();
    L.W = W, L.fs = fs, L.scheme = p.scheme;
    float pad = fs * .8f, gutter = fs * 2.6f;
    float gap = fs * .5f;
    bool columns = p.scheme == BV_CONTROLLER && (mode == 1 || (mode == 0 && W >= fs * 64));  // chips beside the controller, or above and below

    if (p.scheme == BV_CONTROLLER) {
        float chip_w = columns ? std::clamp(W * .19f, fs * 8.f, fs * 13.f) : 0.f;
        gutter = columns ? fs * 2.2f : pad;
        build_controller(L, fs, W, chip_w, gutter, pad, columns);
    } else {
        // keyboard and mouse
        std::vector<KeyDef> keys;
        keyboard_keys(keys);
        float mouse_w = 3.6f, kbw = KBW, total = kbw + 1.4f + mouse_w;
        float u = std::min((W - 2 * pad) / total, fs * 3.6f);
        if (u < fs * 2.5f) {  // (too narrow for the numpad: without it the keys are a fifth bigger)
            kbw = 18.6f, total = kbw + 1.4f + mouse_w;
            u = std::min((W - 2 * pad) / total, fs * 3.6f);
            keys.erase(std::remove_if(keys.begin(), keys.end(), [](const KeyDef &k) { return k.x >= 19.f; }), keys.end());
        }
        u = std::max(u, 6.f);
        float ox = (W - total * u) * .5f, oy = 0;
        {
            Decor plate;
            float m = u * .35f;
            plate.pts = rrect_poly(ImVec2(ox - m, oy - m), ImVec2(ox + kbw * u + m, oy + KBH * u + m), u * .4f);
            plate.fill = true, plate.alpha = .32f, plate.width = 1.2f;
            L.decor.push_back(plate);
        }
        float kg = u * .06f;
        for (const KeyDef &k : keys) {
            L.shapes.emplace_back();
            Shape &s = L.shapes.back();
            s.code = k.sc;
            s.pts = rrect_poly(ImVec2(ox + k.x * u + kg, oy + k.y * u + kg), ImVec2(ox + (k.x + k.w) * u - kg, oy + (k.y + k.h) * u - kg), u * .18f);
            snprintf(s.label, sizeof s.label, "%s", k.label);
            s.tsize = 1.f;
            s.side = k.y < 3.0f ? SIDE_T : SIDE_B;
            if (k.sc == SDL_SCANCODE_UP) s.icon = ICON_UP;
            if (k.sc == SDL_SCANCODE_DOWN) s.icon = ICON_DOWN;
            if (k.sc == SDL_SCANCODE_LEFT) s.icon = ICON_LEFT;
            if (k.sc == SDL_SCANCODE_RIGHT) s.icon = ICON_RIGHT;
            if (const char *use = key_reserved(k.sc)) s.reserved = true, s.reserved_use = use;
            finish_shape(s);
            anchors_rect(s);
        }
        // the mouse
        float mx = ox + (kbw + 1.4f) * u, my = oy + .4f * u, mw = mouse_w * u, mh = 5.6f * u;
        {
            Decor body;
            body.pts = rrect_poly(ImVec2(mx, my), ImVec2(mx + mw, my + mh), mw * .42f);
            body.fill = true, body.alpha = .4f, body.width = 1.4f;
            L.decor.push_back(body);
        }
        auto mshape = [&](int code, Poly pts, const char *lab, int side) {
            L.shapes.emplace_back();
            Shape &s = L.shapes.back();
            s.code = code, s.pts = std::move(pts), s.side = side;
            snprintf(s.label, sizeof s.label, "%s", lab);
            finish_shape(s);
            anchors_rect(s);
            return &s;
        };
        float cx = mx + mw * .5f, R = mw * .42f, g = u * .3f, ip = u * .07f, split = my + mh * .4f;
        Poly lb, rb;
        lb.push_back(ImVec2(mx + ip, split));
        for (int i = 0; i <= 10; i++) {
            float a = PI + (PI * .5f) * i / 10;
            lb.push_back(ImVec2(mx + R + cosf(a) * (R - ip), my + R + sinf(a) * (R - ip)));
        }
        lb.push_back(ImVec2(cx - g, my + ip)), lb.push_back(ImVec2(cx - g, split));
        rb.push_back(ImVec2(mx + mw - ip, split));
        for (int i = 0; i <= 10; i++) {
            float a = 2 * PI - (PI * .5f) * i / 10;
            rb.push_back(ImVec2(mx + mw - R + cosf(a) * (R - ip), my + R + sinf(a) * (R - ip)));
        }
        rb.push_back(ImVec2(cx + g, my + ip)), rb.push_back(ImVec2(cx + g, split));
        mshape(KEY_MOUSE + SDL_BUTTON_LEFT, round_poly(lb, u * .1f), "L", SIDE_T);
        mshape(KEY_MOUSE + SDL_BUTTON_RIGHT, round_poly(rb, u * .1f), "R", SIDE_T);
        {
            Decor seam;  // the palm line under the buttons
            seam.pts = {ImVec2(mx + ip, split + u * .08f), ImVec2(mx + mw - ip, split + u * .08f)};
            seam.closed = false, seam.alpha = .3f;
            L.decor.push_back(seam);
        }
        float ww = u * .46f, wy = my + u * .42f;
        mshape(KEY_WHEEL_UP, rrect_poly(ImVec2(cx - ww * .5f, wy), ImVec2(cx + ww * .5f, wy + u * .6f), u * .12f), "", SIDE_T);
        mshape(KEY_MOUSE + SDL_BUTTON_MIDDLE, rrect_poly(ImVec2(cx - ww * .5f, wy + u * .66f), ImVec2(cx + ww * .5f, wy + u * 1.2f), u * .1f), "", SIDE_T);
        mshape(KEY_WHEEL_DOWN, rrect_poly(ImVec2(cx - ww * .5f, wy + u * 1.26f), ImVec2(cx + ww * .5f, wy + u * 1.86f), u * .12f), "", SIDE_B);
        mshape(KEY_MOUSE + SDL_BUTTON_X1, rrect_poly(ImVec2(mx - u * .12f, my + u * 2.3f), ImVec2(mx + u * .34f, my + u * 3.0f), u * .14f), "", SIDE_B);
        mshape(KEY_MOUSE + SDL_BUTTON_X2, rrect_poly(ImVec2(mx - u * .12f, my + u * 3.15f), ImVec2(mx + u * .34f, my + u * 3.85f), u * .14f), "", SIDE_B);
    }

    for (Shape &sh : L.shapes) dedupe(sh.pts, .9f);
    for (Decor &d : L.decor) dedupe(d.pts, .9f);
    // chips for what is bound
    std::unordered_map<int, int> by_code;
    for (size_t i = 0; i < L.shapes.size(); i++) by_code[L.shapes[i].code] = (int)i;
    std::string extras;
    for (int i = 0; i < p.n_inputs; i++) {
        const BVInput &in = p.inputs[i];
        if (in.n <= 0) continue;
        auto it = by_code.find(in.code);
        if (it == by_code.end()) {
            char nm[64];
            bv_input_name(in.code, nm, sizeof nm);
            if (!extras.empty()) extras += "   ";
            extras += std::string(nm) + ": " + in.actions[0].label + (in.n > 1 ? "..." : "");
            continue;
        }
        L.chips.emplace_back();
        Chip &c = L.chips.back();
        c.shape = it->second;
        c.side = L.shapes[c.shape].side;
        float max_w = columns ? std::clamp(W * .19f, fs * 8.f, fs * 13.f) : fs * 13.5f;
        make_chip(c, in, fs, max_w, columns ? 2 : 1);
    }
    L.extras = extras;
    if (columns) {  // (the chips of a column are as wide as the widest, so their edges line up)
        float cw[2] = {0, 0};
        for (const Chip &c : L.chips)
            if (c.side != SIDE_B) cw[c.side == SIDE_L ? 0 : 1] = std::max(cw[c.side == SIDE_L ? 0 : 1], c.size.x);
        for (Chip &c : L.chips)
            if (c.side != SIDE_B) c.size.x = cw[c.side == SIDE_L ? 0 : 1];
    }

    float diag_lo_x = FLT_MAX, diag_hi_x = -FLT_MAX, diag_lo_y = FLT_MAX, diag_hi_y = -FLT_MAX;
    for (const Shape &s : L.shapes) {
        diag_lo_x = std::min(diag_lo_x, s.lo.x), diag_hi_x = std::max(diag_hi_x, s.hi.x);
        diag_lo_y = std::min(diag_lo_y, s.lo.y), diag_hi_y = std::max(diag_hi_y, s.hi.y);
    }
    for (const Decor &d : L.decor)
        for (const ImVec2 &q : d.pts) {
            diag_lo_x = std::min(diag_lo_x, q.x), diag_hi_x = std::max(diag_hi_x, q.x);
            diag_lo_y = std::min(diag_lo_y, q.y), diag_hi_y = std::max(diag_hi_y, q.y);
        }

    L.diag_w = diag_hi_x - diag_lo_x;
    float need = diag_hi_y + pad;
    if (columns) {
        // the columns of chips beside the drawing, in the order of the inputs they name
        for (int pass = 0; pass < 2; pass++) {
            std::vector<Chip *> col[2];
            for (Chip &c : L.chips) {
                if (c.side == SIDE_B) continue;
                const Shape &s = L.shapes[c.shape];
                c.pos.x = c.side == SIDE_L ? diag_lo_x - gutter - c.size.x : diag_hi_x + gutter;
                c.pos.y = s.c.y - c.size.y * .5f;
                col[c.side == SIDE_L ? 0 : 1].push_back(&c);
            }
            float top = FLT_MAX, bot = -FLT_MAX;
            for (auto &cc : col) {
                relax_column(cc, gap, pad);
                for (Chip *c : cc) top = std::min(top, c->pos.y), bot = std::max(bot, c->pos.y + c->size.y);
            }
            if (pass == 0) {  // centre the drawing on the taller of the two
                float dy = 0;
                if (bot > top) {
                    float mid_chips = (top + bot) * .5f, mid_diag = (diag_lo_y + diag_hi_y) * .5f;
                    if (bot - top > diag_hi_y - diag_lo_y) dy = mid_chips - mid_diag;
                }
                if (fabsf(dy) > .5f) {
                    shift_all(L, dy);
                    diag_lo_y += dy, diag_hi_y += dy;
                }
            } else {
                need = std::max(diag_hi_y + pad, bot + pad);
                if (top < pad) {  // (the top column starts above the margin)
                    shift_all(L, pad - top);
                    diag_lo_y += pad - top, diag_hi_y += pad - top;
                    need += pad - top;
                }
            }
        }
        // the chips of the buttons under the drawing (the centre ones and two of the d-pad's), in tiers
        {
            float lane = fs * 1.9f, bottom_y = diag_hi_y + lane;
            std::vector<Chip *> row;
            for (Chip &c : L.chips)
                if (c.side == SIDE_B) row.push_back(&c);
            std::vector<float> th = pack_row(L, row, W, pad, gap, fs);
            for (Chip *c : row) {
                float above = 0;
                for (int k = 0; k < c->tier; k++) above += th[k] + gap;
                c->pos.y = bottom_y + above;
                need = std::max(need, c->pos.y + c->size.y + pad);
            }
        }
    } else {
        // tiers of chips above and below the keyboard
        std::vector<Chip *> grp[2];
        for (Chip &c : L.chips) grp[c.side == SIDE_T ? 0 : 1].push_back(&c);
        std::vector<float> tier_h[2];
        for (int g = 0; g < 2; g++) tier_h[g] = pack_row(L, grp[g], W, pad, gap, fs);
        float lane = fs * 2.6f;  // between the keyboard and the nearest chips: where the arrows turn
        auto stack_h = [&](int g) {
            float h = 0;
            for (float t : tier_h[g]) h += t + gap;
            return h > 0 ? h - gap : 0.f;
        };
        float top_h = stack_h(0), bot_h = stack_h(1);
        float y0 = pad + (top_h > 0 ? top_h + lane : 0) - diag_lo_y;
        shift_all(L, y0);
        diag_lo_y += y0, diag_hi_y += y0;
        for (Chip *c : grp[0]) {
            float below = 0;
            for (int k = 0; k < c->tier; k++) below += tier_h[0][k] + gap;
            c->pos.y = diag_lo_y - lane - below - c->size.y;
        }
        for (Chip *c : grp[1]) {
            float above = 0;
            for (int k = 0; k < c->tier; k++) above += tier_h[1][k] + gap;
            c->pos.y = diag_hi_y + lane + above;
        }
        need = diag_hi_y + (bot_h > 0 ? lane + bot_h : 0) + pad;
    }
    if (!L.extras.empty()) need += fs * 1.4f;

    // the arrows
    Router r{L, fs, {}};
    std::vector<int> order(L.chips.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    for (int i : order) r.route(L.chips[i], i);
    L.need_h = need;
}

Layout &layout_for(const BVParams &p, float W) {
    Layout &L = g_cache[p.scheme == BV_KEYBOARD ? 1 : 0];
    uint64_t key = layout_key(p, W);
    if (L.key != key || L.shapes.empty()) {
        if (p.scheme == BV_CONTROLLER && p.max_height > 0 && W >= p.font_px * 40) {
            // a limit on the height: the chips beside the controller or above and below it, whichever lets the drawing be larger
            Layout a, b;
            build_layout(a, p, W, 1);
            build_layout(b, p, W, 2);
            bool fa = a.need_h <= p.max_height, fb = b.need_h <= p.max_height;
            bool pick_a = fa && fb ? a.diag_w >= b.diag_w : fa ? true : fb ? false : a.need_h <= b.need_h;
            L = pick_a ? std::move(a) : std::move(b);
        } else
            build_layout(L, p, W, 0);
        L.key = key;
    }
    return L;
}

// ---------------------------------------------------------------------------------------------- drawing
void stroke_poly(ImDrawList *dl, const Poly &pts, ImU32 col, float w, bool closed = true) {
    if (pts.size() < 2) return;
    dl->AddPolyline(pts.data(), (int)pts.size(), col, w, closed ? ImDrawFlags_Closed : 0);
}
void fill_poly(ImDrawList *dl, const Shape &s, ImU32 col) {
    if (s.convex) dl->AddConvexPolyFilled(s.pts.data(), (int)s.pts.size(), col);
    else dl->AddConcavePolyFilled(s.pts.data(), (int)s.pts.size(), col);
}
void glow(ImDrawList *dl, const Shape &s, ImU32 col, float spread, float strength) {
    const int layers = 8;
    for (int k = layers; k >= 1; k--) {
        float t = (float)k / layers;
        float a = strength * (.07f + .55f * (1 - t) * (1 - t));
        dl->AddPolyline(s.pts.data(), (int)s.pts.size(), with_alpha(col, a), 2 * spread * t + 1.f, ImDrawFlags_Closed);
    }
}
void draw_icon(ImDrawList *dl, const Shape &s, ImU32 col, float lw) {
    ImVec2 c = s.c;
    float sz = std::min(s.hi.x - s.lo.x, s.hi.y - s.lo.y) * .22f;
    switch (s.icon) {
    case ICON_UP: dl->AddTriangleFilled(ImVec2(c.x, c.y - sz), ImVec2(c.x - sz, c.y + sz * .7f), ImVec2(c.x + sz, c.y + sz * .7f), col); break;
    case ICON_DOWN: dl->AddTriangleFilled(ImVec2(c.x, c.y + sz), ImVec2(c.x + sz, c.y - sz * .7f), ImVec2(c.x - sz, c.y - sz * .7f), col); break;
    case ICON_LEFT: dl->AddTriangleFilled(ImVec2(c.x - sz, c.y), ImVec2(c.x + sz * .7f, c.y + sz), ImVec2(c.x + sz * .7f, c.y - sz), col); break;
    case ICON_RIGHT: dl->AddTriangleFilled(ImVec2(c.x + sz, c.y), ImVec2(c.x - sz * .7f, c.y - sz), ImVec2(c.x - sz * .7f, c.y + sz), col); break;
    case ICON_START:
        for (int i = -1; i <= 1; i++) dl->AddLine(ImVec2(c.x - s.r * .42f, c.y + i * s.r * .3f), ImVec2(c.x + s.r * .42f, c.y + i * s.r * .3f), col, lw);
        break;
    case ICON_BACK: {
        float q = s.r * .32f;
        dl->AddRect(ImVec2(c.x - q * 1.5f, c.y - q * 1.1f), ImVec2(c.x + q * .3f, c.y + q * .7f), col, 1.f, 0, lw);
        dl->AddRect(ImVec2(c.x - q * .3f, c.y - q * .7f), ImVec2(c.x + q * 1.5f, c.y + q * 1.1f), col, 1.f, 0, lw);
        break;
    }
    }
}
void arrow_head(ImDrawList *dl, ImVec2 tip, ImVec2 dir, float size, ImU32 col) {
    ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(tip, tip - dir * size + n * (size * .5f), tip - dir * size - n * (size * .5f), col);
}
}  // namespace

float bv_fit_width(const BVParams &p, float width) {
    // the widest drawing, up to `width`, that is not taller than p.max_height (the layout is not simply in proportion
    // to the width: chips keep their size). Remembered while nothing changes, so a frame costs nothing.
    static struct { uint64_t key; float w; } memo;
    uint64_t key = layout_key(p, width);
    if (memo.key == key && memo.w > 0) return memo.w;
    float min_w = p.font_px * 24, w = width;
    if (p.max_height > 0) {
        // the candidates, each with the layout it gets (the controller's chips beside it or above and below): the one that fits
        // and has the largest drawing; if none fits, the smallest height
        static const float steps[] = {1.f, .9f, .8f, .7f, .6f, .5f};
        float best_w = -1, best_d = -1, low_w = width, low_h = 1e9f;
        for (float k : steps) {
            float cw = std::max(min_w, width * k);
            const Layout &L = layout_for(p, cw);
            if (L.need_h < low_h) low_h = L.need_h, low_w = cw;
            if (L.need_h <= p.max_height && L.diag_w > best_d) best_d = L.diag_w, best_w = cw;
        }
        w = best_w > 0 ? best_w : low_w;
        layout_for(p, w);  // (the layout of the one chosen is the one kept)
    }
    memo.key = key, memo.w = w;
    return w;
}

float bv_height(const BVParams &p, float width) {
    Layout &L = layout_for(p, width);
    return L.need_h;
}

bool bv_input_center(const BVParams &p, float width, int code, ImVec2 *out) {
    Layout &L = layout_for(p, width);
    for (const Shape &sh : L.shapes)
        if (sh.code == code) {
            if (out) *out = sh.c;
            return true;
        }
    return false;
}

BVResult bv_draw(const char *id, float width, float height, const BVParams &p) {
    BVResult res;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 org = ImGui::GetCursorScreenPos();
    Layout &L = layout_for(p, width);
    float fs = p.font_px;
    ImVec2 off = org + ImVec2(0, std::max(0.f, (height - L.need_h) * .5f));
    const BVColors &C = p.colors;

    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton(id, ImVec2(width, height));
    ImGui::PopItemFlag();
    bool region_hover = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemDeactivated() && ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos - off;

    // what the mouse is over (the topmost shape)
    int hover = -1;
    if (region_hover)
        for (int i = (int)L.shapes.size() - 1; i >= 0; i--) {
            const Shape &s = L.shapes[i];
            if (mouse.x < s.lo.x || mouse.x > s.hi.x || mouse.y < s.lo.y || mouse.y > s.hi.y) continue;
            if (point_in_poly(mouse, s.pts)) {
                hover = i;
                break;
            }
        }
    if (hover < 0 && region_hover)  // (a chip stands for its input: pointing at it lights the input, clicking it picks it)
        for (const Chip &c : L.chips)
            if (mouse.x >= c.pos.x && mouse.x <= c.pos.x + c.size.x && mouse.y >= c.pos.y && mouse.y <= c.pos.y + c.size.y) {
                hover = c.shape;
                break;
            }
    if (hover >= 0) {
        res.hovered = L.shapes[hover].code;
        res.reserved = L.shapes[hover].reserved;
        if (pressed && !res.reserved) res.clicked = res.hovered;
        ImGui::SetMouseCursor(res.reserved ? ImGuiMouseCursor_NotAllowed : ImGuiMouseCursor_Hand);
    }
    auto is_held = [&](int code) {
        for (int i = 0; i < p.n_held; i++)
            if (p.held[i] == code) return true;
        return false;
    };
    auto is_sel = [&](int code) {
        if (code && code == p.selected_input) return true;
        for (int i = 0; i < p.n_selected; i++)
            if (p.selected_list[i] == code) return true;
        return false;
    };
    auto is_foc = [&](int code) {
        if (code && code == p.focus_input) return true;
        for (int i = 0; i < p.n_focus; i++)
            if (p.focus_list[i] == code) return true;
        return false;
    };
    std::unordered_map<int, bool> action_selected;
    for (int i = 0; i < p.n_inputs; i++)
        for (int k = 0; k < p.inputs[i].n; k++)
            if (p.inputs[i].actions[k].selected) action_selected[p.inputs[i].actions[k].id] = true;
    std::unordered_map<int, int> chip_of;
    for (size_t i = 0; i < L.chips.size(); i++) chip_of[L.chips[i].code] = (int)i;

    float lw = std::max(1.25f, fs * .105f);
    float pulse = p.capturing ? .72f + .28f * sinf(p.time_s * 6.5f) : 1.f;
    bool dark_accent = luminance(C.accent) < .55f;
    ImU32 on_accent = dark_accent ? IM_COL32(255, 255, 255, 255) : (C.panel | 0xFF000000u);
    if (!dark_accent) on_accent = mix(C.panel | 0xFF000000u, IM_COL32(0, 0, 0, 255), .5f);

    // the body, wells and other decoration
    for (const Decor &d : L.decor) {
        Poly pts = d.pts;
        for (ImVec2 &q : pts) q = q + off;
        if (d.fill) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), with_alpha(mix(C.panel, C.line, .12f), d.alpha * .9f));
        stroke_poly(dl, pts, with_alpha(C.line, d.alpha), d.width * lw, d.closed);
    }
    auto shifted = [&](const Shape &s) {
        Shape t = s;
        for (ImVec2 &q : t.pts) q = q + off;
        t.c = t.c + off, t.lo = t.lo + off, t.hi = t.hi + off;
        return t;
    };

    // the arrows, under the inputs
    auto emphasised = [&](const Chip &c) {
        return is_sel(c.code) || is_foc(c.code) || (hover >= 0 && L.shapes[hover].code == c.code) || is_held(c.code);
    };
    for (int pass = 0; pass < 2; pass++)
        for (const Chip &c : L.chips) {
            bool em = emphasised(c);
            if ((pass == 1) != em) continue;
            bool sel = is_sel(c.code);
            ImU32 col = sel ? C.accent_bright : c.conflict ? C.warn : em ? C.accent : C.text_dim;
            float a = em ? 1.f : .62f;
            Poly r = c.route;
            for (ImVec2 &q : r) q = q + off;
            if (em) stroke_poly(dl, r, with_alpha(col, .22f * pulse), lw * 4.2f, false);
            stroke_poly(dl, r, with_alpha(col, a), lw * (em ? 1.6f : 1.05f), false);
            arrow_head(dl, r.back(), c.dir, fs * (em ? .62f : .5f), with_alpha(col, a));
        }

    // the inputs
    for (size_t i = 0; i < L.shapes.size(); i++) {
        const Shape &s0 = L.shapes[i];
        Shape s = shifted(s0);
        auto cit = chip_of.find(s0.code);
        const Chip *chip = cit != chip_of.end() ? &L.chips[cit->second] : nullptr;
        bool bound = chip != nullptr, sel = is_sel(s0.code), foc = is_foc(s0.code);
        bool hov = (int)i == hover, held = is_held(s0.code), conflict = chip && chip->conflict;
        float spread = fs * (sel ? 1.7f : .8f);
        ImU32 outline = bound ? C.text : C.line;
        float outline_a = s0.reserved ? .28f : bound ? .88f : .55f;
        if (conflict) outline = C.warn, outline_a = 1.f;
        if (held) glow(dl, s, C.live, fs * .7f, .8f);
        if (sel) glow(dl, s, C.accent, spread, pulse);
        else if (foc) glow(dl, s, C.accent_bright, fs * .7f, .55f + .25f * sinf(p.time_s * 5.f));
        else if (hov && !s0.reserved) glow(dl, s, C.accent, fs * .5f, .55f);
        // the face
        ImU32 base = mix(C.panel | 0xFF000000u, C.line | 0xFF000000u, .13f);
        fill_poly(dl, s, base);
        if (bound) fill_poly(dl, s, with_alpha(conflict ? C.warn : C.text, conflict ? .2f : .09f));
        if (held) fill_poly(dl, s, with_alpha(C.live, .62f));
        if (hov && !s0.reserved) fill_poly(dl, s, with_alpha(C.accent, .26f));
        if (foc) fill_poly(dl, s, with_alpha(C.accent_bright, .22f));
        if (sel) fill_poly(dl, s, with_alpha(C.accent, std::min(1.f, .93f * (.88f + .12f * pulse))));
        float ow = lw * (sel || hov || foc ? 1.5f : 1.f);
        ImU32 oc = with_alpha(sel ? C.accent_bright : hov || foc ? C.accent_bright : outline, outline_a);
        dl->AddPolyline(s.pts.data(), (int)s.pts.size(), oc, ow, ImDrawFlags_Closed);
        // the label and the icon
        ImU32 tc = sel ? on_accent : s0.reserved ? with_alpha(C.text_dim, .5f) : bound ? C.text : C.text_dim;
        if (s0.label[0]) {
            float h = s.hi.y - s.lo.y, w = s.hi.x - s.lo.x;
            float ts = std::min(fs * (s0.tsize > 1.f ? 1.25f : .95f) * s0.tsize, h * .5f);
            if (s0.circle) ts = std::min(ts, s.r * .95f);
            ts = std::min(ts, fs * 1.4f);
            std::string t = fit(s0.label, ts, w - 4);
            ImVec2 sz = text_size(ts, t.c_str());
            if (sz.x < w - 2 && sz.y < h) dl->AddText(ImGui::GetFont(), ts, ImVec2(s.c.x - sz.x * .5f, s.c.y - sz.y * .5f), tc, t.c_str());
        }
        if (s0.icon) draw_icon(dl, s, tc, lw);
        if (s0.reserved && s0.label[0] == 0) {}
    }
    // the dots where the arrows leave the inputs
    for (const Chip &c : L.chips) {
        bool em = emphasised(c);
        bool sel = is_sel(c.code);
        ImU32 col = c.conflict ? C.warn : C.text;
        ImVec2 d = c.dot + off;
        float r = fs * (em ? .26f : .2f);
        dl->AddCircleFilled(d, r + lw, with_alpha(C.panel | 0xFF000000u, .8f), 16);
        dl->AddCircleFilled(d, r, sel || em ? (c.conflict ? C.warn : C.accent_bright) : with_alpha(col, .95f), 16);
    }

    // the chips
    float pad = chip_pad(fs), lh = chip_line_h(fs);
    for (const Chip &c : L.chips) {
        ImVec2 lo = c.pos + off, hi = lo + c.size;
        bool em = emphasised(c);
        bool sel = is_sel(c.code);
        ImU32 border = c.conflict ? C.warn : sel ? C.accent_bright : em ? C.accent : with_alpha(C.line, .85f);
        ImU32 fillc = with_alpha(mix(C.panel | 0xFF000000u, C.line, .16f), .96f);
        float rr = fs * .38f;
        dl->AddRectFilled(lo, hi, fillc, rr);
        if (sel) {
            glow(dl, [&] { Shape g; g.pts = rrect_poly(lo, hi, rr); return g; }(), C.accent, fs * .6f, .55f * pulse);
            dl->AddRectFilled(lo, hi, with_alpha(C.accent, .16f), rr);
        }
        dl->AddRect(lo, hi, border, rr, 0, lw * (em ? 1.5f : 1.f));
        float y = lo.y + pad - fs * .02f;
        float ts = fs * .78f, as = fs * .95f;
        dl->AddText(ImGui::GetFont(), ts, ImVec2(lo.x + pad, y + (lh - ts) * .45f), C.text_dim, c.title.c_str());
        y += lh - fs * .12f;
        for (size_t k = 0; k < c.lines.size(); k++) {
            bool ls = action_selected.count(c.ids[k]) != 0;
            ImU32 tc = ls ? C.accent_bright : C.text;
            if (ls) dl->AddRectFilled(ImVec2(lo.x + pad - fs * .22f, y + lh * .12f), ImVec2(lo.x + pad - fs * .1f, y + lh * .88f), C.accent_bright, 1.f);
            dl->AddText(ImGui::GetFont(), as, ImVec2(lo.x + pad + fs * .1f, y + (lh - as) * .4f), tc, c.lines[k].c_str());
            y += lh;
        }
        if (c.extra > 0) {
            char more[24];
            snprintf(more, sizeof more, "+%d", c.extra);
            ImVec2 sz = text_size(ts, more);
            dl->AddText(ImGui::GetFont(), ts, ImVec2(hi.x - pad - sz.x - (c.conflict ? 0.f : 0.f), y - lh + (lh - ts) * .4f), C.text_dim, more);
        }
        if (c.conflict) {  // a badge: the actions that fight over this input
            float br = fs * .5f;
            ImVec2 bc(hi.x - pad - br * .3f, lo.y + pad + br * .8f);
            dl->AddCircleFilled(bc, br, C.warn, 20);
            char n[8];
            snprintf(n, sizeof n, "%d", c.count);
            ImVec2 sz = text_size(fs * .8f, n);
            dl->AddText(ImGui::GetFont(), fs * .8f, ImVec2(bc.x - sz.x * .5f, bc.y - sz.y * .5f), IM_COL32(255, 255, 255, 255), n);
        }
    }
    if (!L.extras.empty()) {
        std::string t = fit("Not on the drawing: " + L.extras, fs * .85f, width - 2 * fs);
        dl->AddText(ImGui::GetFont(), fs * .85f, ImVec2(org.x + fs, org.y + L.need_h - fs * 1.5f), C.text_dim, t.c_str());
    }

    // the tooltip of the input under the mouse: every action on it
    if (hover >= 0) {
        const Shape &s = L.shapes[hover];
        char nm[64];
        bv_input_name(s.code, nm, sizeof nm);
        std::vector<std::string> lines;
        lines.push_back(nm);
        auto cit = chip_of.find(s.code);
        if (s.reserved) lines.push_back(std::string("Kept by the recomp: ") + s.reserved_use);
        else if (cit == chip_of.end()) lines.push_back(p.click_hint ? std::string("Nothing on it. ") + p.click_hint : std::string("Nothing on it."));
        else {
            for (int i = 0; i < p.n_inputs; i++)
                if (p.inputs[i].code == s.code)
                    for (int k = 0; k < p.inputs[i].n; k++) lines.push_back(std::string("  ") + p.inputs[i].actions[k].label);
            if (p.click_hint) lines.push_back(p.click_hint);
        }
        float ts = fs * .95f, w = 0;
        for (auto &l : lines) w = std::max(w, text_size(ts, l.c_str()).x);
        float pd = fs * .6f, h = pd * 2 + lines.size() * ts * 1.25f;
        ImVec2 at = ImGui::GetIO().MousePos + ImVec2(fs * 1.2f, fs * 1.2f);
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (at.x + w + 2 * pd > disp.x) at.x = disp.x - w - 2 * pd - 4;
        if (at.y + h > disp.y) at.y = ImGui::GetIO().MousePos.y - h - fs;
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(at, at + ImVec2(w + 2 * pd, h), with_alpha(mix(C.panel | 0xFF000000u, C.line, .22f), .98f), fs * .4f);
        fg->AddRect(at, at + ImVec2(w + 2 * pd, h), C.accent, fs * .4f, 0, lw);
        for (size_t i = 0; i < lines.size(); i++)
            fg->AddText(ImGui::GetFont(), ts, at + ImVec2(pd, pd + i * ts * 1.25f), i == 0 ? C.accent_bright : C.text, lines[i].c_str());
    }
    return res;
}
