// Renders runtime/binding_view.cpp to PNG files without a window or a GPU: ImGui builds the frame as usual, a small
// software rasteriser draws its triangles (vertex colours, the font atlas, scissor rectangles) and zlib writes the
// file. `make preview-bindings` runs it; look at obj/binding-preview/*.png.
#include "binding_view.h"
#include "imgui.h"

#include <zlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#define PAD_BASE 0x2000
#define PAD_BTN(b) (PAD_BASE + (b))
#define KEY_MOUSE 0x1000
#define KEY_WHEEL_UP (KEY_MOUSE + 0x10)
#define KEY_WHEEL_DOWN (KEY_MOUSE + 0x11)

// ---------------------------------------------------------------------------------------------- software renderer
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA
    void clear(ImU32 c) {
        px.assign((size_t)w * h * 4, 0);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            px[i * 4] = c & 255, px[i * 4 + 1] = (c >> 8) & 255, px[i * 4 + 2] = (c >> 16) & 255, px[i * 4 + 3] = 255;
        }
    }
};
static void texel(const ImTextureData *t, float u, float v, float out[4]) {
    float x = u * t->Width - .5f, y = v * t->Height - .5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = std::clamp(x0 + i, 0, t->Width - 1), yy = std::clamp(y0 + j, 0, t->Height - 1);
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const unsigned char *p = t->Pixels + ((size_t)yy * t->Width + xx) * t->BytesPerPixel;
            float c[4];
            if (t->BytesPerPixel == 4) c[0] = p[0] / 255.f, c[1] = p[1] / 255.f, c[2] = p[2] / 255.f, c[3] = p[3] / 255.f;
            else c[0] = c[1] = c[2] = 1.f, c[3] = p[0] / 255.f;
            for (int k = 0; k < 4; k++) acc[k] += c[k] * wgt;
        }
    memcpy(out, acc, sizeof acc);
}
static void draw_triangle(Image &img, ImDrawVert a, ImDrawVert b, ImDrawVert c, const ImVec4 &clip, const ImTextureData *tex) {
    float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (fabsf(area) < 1e-8f) return;
    if (area < 0) std::swap(b, c), area = -area;  // (every triangle the same way round: the top-left rule below needs it)
    float minx = std::min({a.pos.x, b.pos.x, c.pos.x}), maxx = std::max({a.pos.x, b.pos.x, c.pos.x});
    float miny = std::min({a.pos.y, b.pos.y, c.pos.y}), maxy = std::max({a.pos.y, b.pos.y, c.pos.y});
    int x0 = std::max((int)floorf(minx), (int)floorf(clip.x)), x1 = std::min((int)ceilf(maxx), (int)ceilf(clip.z));
    int y0 = std::max((int)floorf(miny), (int)floorf(clip.y)), y1 = std::min((int)ceilf(maxy), (int)ceilf(clip.w));
    x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, img.w), y1 = std::min(y1, img.h);
    float inv = 1.f / area;
    auto col = [](ImU32 c, int s) { return ((c >> s) & 255) / 255.f; };
    // a pixel centre exactly on an edge belongs to one of the two triangles that share it
    auto owns = [](ImVec2 p0, ImVec2 p1) { float dx = p1.x - p0.x, dy = p1.y - p0.y; return dy < 0 || (dy == 0 && dx > 0); };
    bool own0 = owns(b.pos, c.pos), own1 = owns(c.pos, a.pos), own2 = owns(a.pos, b.pos);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + .5f, py = y + .5f;
            float e0 = (b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px);
            float e1 = (c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px);
            float e2 = (a.pos.x - px) * (b.pos.y - py) - (a.pos.y - py) * (b.pos.x - px);
            if (e0 < 0 || e1 < 0 || e2 < 0) continue;
            if ((e0 == 0 && !own0) || (e1 == 0 && !own1) || (e2 == 0 && !own2)) continue;
            float w0 = e0 * inv, w1 = e1 * inv, w2 = e2 * inv;
            float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
            float t[4] = {1, 1, 1, 1};
            if (tex) texel(tex, u, v, t);
            float r = (w0 * col(a.col, 0) + w1 * col(b.col, 0) + w2 * col(c.col, 0)) * t[0];
            float g = (w0 * col(a.col, 8) + w1 * col(b.col, 8) + w2 * col(c.col, 8)) * t[1];
            float bl = (w0 * col(a.col, 16) + w1 * col(b.col, 16) + w2 * col(c.col, 16)) * t[2];
            float al = (w0 * col(a.col, 24) + w1 * col(b.col, 24) + w2 * col(c.col, 24)) * t[3];
            uint8_t *d = &img.px[((size_t)y * img.w + x) * 4];
            d[0] = (uint8_t)std::clamp(r * al * 255.f + d[0] * (1 - al) + .5f, 0.f, 255.f);
            d[1] = (uint8_t)std::clamp(g * al * 255.f + d[1] * (1 - al) + .5f, 0.f, 255.f);
            d[2] = (uint8_t)std::clamp(bl * al * 255.f + d[2] * (1 - al) + .5f, 0.f, 255.f);
        }
}
static void render_draw_data(Image &img, ImDrawData *dd) {
    if (dd->Textures)
        for (ImTextureData *t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate) t->SetTexID((ImTextureID)(intptr_t)t), t->SetStatus(ImTextureStatus_OK);
            else if (t->Status == ImTextureStatus_WantUpdates) t->SetStatus(ImTextureStatus_OK);
            else if (t->Status == ImTextureStatus_WantDestroy) t->SetTexID(ImTextureID_Invalid), t->BackendUserData = nullptr, t->SetStatus(ImTextureStatus_Destroyed);
        }
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *dl = dd->CmdLists[n];
        for (const ImDrawCmd &cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImTextureData *tex = (const ImTextureData *)(intptr_t)cmd.GetTexID();
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawIdx *ix = dl->IdxBuffer.Data + cmd.IdxOffset + i;
                const ImDrawVert *vb = dl->VtxBuffer.Data + cmd.VtxOffset;
                draw_triangle(img, vb[ix[0]], vb[ix[1]], vb[ix[2]], cmd.ClipRect, tex);
            }
        }
    }
}
static bool write_png(const char *path, const Image &img) {
    std::vector<uint8_t> raw((size_t)img.h * (img.w * 3 + 1));
    for (int y = 0; y < img.h; y++) {
        uint8_t *row = &raw[(size_t)y * (img.w * 3 + 1)];
        row[0] = 0;
        for (int x = 0; x < img.w; x++) memcpy(row + 1 + x * 3, &img.px[((size_t)y * img.w + x) * 4], 3);
    }
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    auto be32 = [](uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; };
    auto chunk = [&](const char *type, const uint8_t *data, uint32_t n) {
        uint8_t h[8];
        be32(h, n);
        memcpy(h + 4, type, 4);
        fwrite(h, 1, 8, f);
        if (n) fwrite(data, 1, n, f);
        uLong crc = crc32(0, (const Bytef *)type, 4);
        if (n) crc = crc32(crc, data, n);
        uint8_t c[4];
        be32(c, (uint32_t)crc);
        fwrite(c, 1, 4, f);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, img.w), be32(ihdr + 4, img.h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", comp.data(), (uint32_t)clen);
    chunk("IEND", nullptr, 0);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------------------------- scenarios
struct Theme {
    const char *name;
    ImU32 panel, line, text, dim, accent, bright, warn, live;
};
static const Theme THEMES[] = {
    {"blue", IM_COL32(6, 10, 17, 255), IM_COL32(178, 192, 208, 255), IM_COL32(244, 247, 252, 255), IM_COL32(150, 166, 184, 255),
     IM_COL32(110, 217, 255, 255), IM_COL32(168, 236, 255, 255), IM_COL32(255, 150, 110, 255), IM_COL32(120, 240, 170, 255)},
    {"gold", IM_COL32(34, 25, 19, 255), IM_COL32(196, 170, 140, 255), IM_COL32(242, 216, 170, 255), IM_COL32(175, 150, 128, 255),
     IM_COL32(213, 162, 79, 255), IM_COL32(247, 226, 162, 255), IM_COL32(222, 104, 78, 255), IM_COL32(150, 214, 150, 255)},
    {"hunter", IM_COL32(50, 25, 24, 255), IM_COL32(222, 206, 198, 255), IM_COL32(242, 238, 232, 255), IM_COL32(196, 176, 166, 255),
     IM_COL32(236, 212, 44, 255), IM_COL32(255, 232, 84, 255), IM_COL32(255, 96, 72, 255), IM_COL32(110, 220, 255, 255)},
    {"light", IM_COL32(244, 244, 240, 255), IM_COL32(86, 96, 90, 255), IM_COL32(28, 33, 29, 255), IM_COL32(98, 107, 100, 255),
     IM_COL32(190, 90, 28, 255), IM_COL32(212, 108, 40, 255), IM_COL32(176, 44, 28, 255), IM_COL32(24, 124, 170, 255)},
};

struct Entry {
    int code;
    std::vector<const char *> labels;
    bool conflict = false;
};
struct Profile {
    std::vector<Entry> entries;
    std::vector<BVAction> acts;
    std::vector<BVInput> ins;
    void build(const char *selected_label = nullptr) {
        size_t total = 0;
        for (auto &e : entries) total += e.labels.size();
        acts.clear(), ins.clear();
        acts.reserve(total);
        int id = 1;
        std::vector<size_t> first;
        for (auto &e : entries) {
            first.push_back(acts.size());
            for (const char *l : e.labels) acts.push_back({id++, l, selected_label && !strcmp(l, selected_label)});
        }
        for (size_t i = 0; i < entries.size(); i++)
            ins.push_back({entries[i].code, acts.data() + first[i], (int)entries[i].labels.size(), entries[i].conflict});
    }
};
#define PB(x) PAD_BTN(SDL_GAMEPAD_BUTTON_##x)
static Profile pad_typical() {
    Profile p;
    p.entries = {
        {PB(SOUTH), {"Dodge / roll", "Cancel / back", "Crouch"}},
        {PB(EAST), {"Attack A", "Gather / open / talk", "Confirm"}},
        {PB(NORTH), {"Attack X / draw", "Menu action X"}},
        {PB(WEST), {"Use item / sheathe", "Menu action Y"}},
        {PB(LEFT_SHOULDER), {"Camera / item bar", "Tab left"}},
        {PB(RIGHT_SHOULDER), {"Dash", "Tab right"}},
        {PAD_BASE + 0x40, {"ZL button", "Page left"}},
        {PAD_BASE + 0x41, {"Weapon action", "Page right"}},
        {PB(DPAD_UP), {"Quick action up", "Menu up"}},
        {PB(DPAD_DOWN), {"Quick action down", "Menu down"}},
        {PB(DPAD_LEFT), {"Quick action left", "Menu left"}},
        {PB(DPAD_RIGHT), {"Quick action right", "Menu right"}},
        {PB(LEFT_STICK), {"Left stick click"}},
        {PB(RIGHT_STICK), {"Right stick click"}},
        {PB(START), {"Start menu"}},
        {PB(BACK), {"Map"}},
    };
    return p;
}
static Profile pad_light() {
    Profile p;
    p.entries = {{PB(SOUTH), {"Dodge / roll"}}, {PB(NORTH), {"Attack X"}}, {PB(RIGHT_SHOULDER), {"Dash"}}, {PB(START), {"Pause"}}};
    return p;
}
static Profile pad_conflicts() {
    Profile p = pad_typical();
    p.entries[0].conflict = true;
    p.entries[1].conflict = true;
    p.entries[6].conflict = true;
    p.entries.push_back({BV_STICK_L_UP, {"Move forward"}});
    p.entries.push_back({BV_STICK_L_LEFT, {"Strafe left"}});
    p.entries.push_back({BV_STICK_R_RIGHT, {"Look right"}});
    return p;
}
static Profile kbm_typical() {
    Profile p;
    p.entries = {
        {SDL_SCANCODE_W, {"Move forward"}},
        {SDL_SCANCODE_A, {"Move left"}},
        {SDL_SCANCODE_S, {"Move back"}},
        {SDL_SCANCODE_D, {"Move right"}},
        {SDL_SCANCODE_SPACE, {"Dodge / roll", "Cancel / back"}},
        {SDL_SCANCODE_E, {"Gather / open / talk", "Confirm"}},
        {SDL_SCANCODE_LSHIFT, {"Dash"}},
        {SDL_SCANCODE_LCTRL, {"Crouch"}},
        {SDL_SCANCODE_Q, {"Camera reset"}},
        {SDL_SCANCODE_R, {"Use item"}},
        {SDL_SCANCODE_F, {"Weapon action"}},
        {SDL_SCANCODE_TAB, {"Map", "Menu +"}},
        {SDL_SCANCODE_1, {"Quick action up"}},
        {SDL_SCANCODE_2, {"Quick action right"}},
        {SDL_SCANCODE_3, {"Quick action down"}},
        {SDL_SCANCODE_4, {"Quick action left"}},
        {SDL_SCANCODE_UP, {"Menu up"}},
        {SDL_SCANCODE_DOWN, {"Menu down"}},
        {SDL_SCANCODE_LEFT, {"Menu left"}},
        {SDL_SCANCODE_RIGHT, {"Menu right"}},
        {SDL_SCANCODE_RETURN, {"Confirm"}},
        {KEY_MOUSE + SDL_BUTTON_LEFT, {"Attack X", "Confirm"}},
        {KEY_MOUSE + SDL_BUTTON_RIGHT, {"Attack A", "Cancel / back"}},
        {KEY_MOUSE + SDL_BUTTON_MIDDLE, {"Lock-on"}},
        {KEY_MOUSE + SDL_BUTTON_X1, {"Use item"}},
        {KEY_MOUSE + SDL_BUTTON_X2, {"Sheathe"}},
        {KEY_WHEEL_UP, {"Item bar left", "Menu up"}},
        {KEY_WHEEL_DOWN, {"Item bar right", "Menu down"}},
    };
    return p;
}
static Profile kbm_conflicts() {
    Profile p = kbm_typical();
    p.entries[4].conflict = true;
    p.entries[5].conflict = true;
    p.entries[21].conflict = true;
    return p;
}

static Profile pad_heavy() {
    static const char *names[] = {"Attack X", "Attack A", "Dodge", "Crouch", "Use item", "Sheathe", "Dash", "Weapon action"};
    Profile p;
    int codes[] = {PB(SOUTH), PB(EAST), PB(WEST), PB(NORTH), PB(LEFT_SHOULDER), PB(RIGHT_SHOULDER), PAD_BASE + 0x40, PAD_BASE + 0x41,
                   PB(DPAD_UP), PB(DPAD_DOWN), PB(DPAD_LEFT), PB(DPAD_RIGHT), PB(LEFT_STICK), PB(RIGHT_STICK), PB(START), PB(BACK),
                   BV_STICK_L_UP, BV_STICK_L_DOWN, BV_STICK_L_LEFT, BV_STICK_L_RIGHT, BV_STICK_R_UP, BV_STICK_R_DOWN, BV_STICK_R_LEFT,
                   BV_STICK_R_RIGHT};
    int k = 0;
    for (int c : codes) {
        Entry e;
        e.code = c;
        int n = 1 + k % 6;
        for (int i = 0; i < n; i++) e.labels.push_back(names[(k + i) % 8]);
        e.conflict = k % 4 == 1;
        p.entries.push_back(e);
        k++;
    }
    p.entries.push_back({SDL_SCANCODE_W, {"Not drawn on a controller"}});
    return p;
}
static Profile kbm_heavy() {
    static const char *names[] = {"Attack X", "Attack A", "Dodge / roll", "Crouch", "Use item", "Sheathe", "Dash", "Weapon action",
                                  "Gather / open / talk", "Camera reset", "Quick action up", "Map"};
    Profile p;
    int k = 0;
    auto add = [&](int code) {
        Entry e;
        e.code = code;
        int n = 1 + (k % 5 == 0 ? 2 : 0);
        for (int i = 0; i < n; i++) e.labels.push_back(names[(k + i) % 12]);
        e.conflict = k % 9 == 4;
        p.entries.push_back(e);
        k++;
    };
    for (int sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_Z; sc++) add(sc);
    for (int sc = SDL_SCANCODE_1; sc <= SDL_SCANCODE_0; sc++) add(sc);
    for (int sc : std::initializer_list<int>{SDL_SCANCODE_TAB, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE,
                   SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_F2, SDL_SCANCODE_F3,
                   SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_ENTER,
                   SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_HOME, SDL_SCANCODE_END,
                   KEY_MOUSE + SDL_BUTTON_LEFT, KEY_MOUSE + SDL_BUTTON_RIGHT, KEY_MOUSE + SDL_BUTTON_MIDDLE, KEY_MOUSE + SDL_BUTTON_X1,
                   KEY_MOUSE + SDL_BUTTON_X2, KEY_WHEEL_UP, KEY_WHEEL_DOWN, SDL_SCANCODE_VOLUMEUP})
        add(sc);
    return p;
}

struct Scenario {
    std::string file, title;
    int W, H;           // the image
    int scheme;
    Profile prof;
    const char *selected_label = nullptr;
    int selected_input = 0, focus_input = 0;
    bool capturing = false;
    std::vector<int> held;
    int theme = 0;
    float font = 14.f;
    float mouse_x = -1, mouse_y = -1;  // in the widget's space (< 0: none)
    float region_w = 0;                // 0: the whole image minus margins
};

static bool g_failed;
static void render(const Scenario &s, const char *dir) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)s.W, (float)s.H);
    ImGui::GetStyle().FontSizeBase = s.font;
    const Theme &th = THEMES[s.theme];
    Profile prof = s.prof;
    prof.build(s.selected_label);
    BVParams p;
    p.scheme = s.scheme;
    p.inputs = prof.ins.data(), p.n_inputs = (int)prof.ins.size();
    p.selected_input = s.selected_input, p.focus_input = s.focus_input, p.capturing = s.capturing;
    p.held = s.held.data(), p.n_held = (int)s.held.size();
    p.colors = bv_colors_from(th.panel, th.line, th.text, th.dim, th.accent, th.bright, th.warn, th.live);
    p.font_px = s.font;
    Image img;
    img.w = s.W, img.h = s.H;
    float margin = s.font * 1.2f;
    float w = s.region_w > 0 ? s.region_w : s.W - 2 * margin;
    ImVec2 widget_origin;
    for (int frame = 0; frame < 4; frame++) {
        p.time_s = 1.0f;
        if (s.mouse_x >= 0) io.AddMousePosEvent(margin + s.mouse_x, margin + s.font * 3 + s.mouse_y);
        else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // (no mouse)
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)s.W, (float)s.H));
        ImGui::Begin("##preview", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)s.W, (float)s.H), th.panel);
        char title[256];
        snprintf(title, sizeof title, "%s   (%dx%d, font %.0f px)", s.title.c_str(), s.W, s.H, s.font);
        dl->AddText(ImGui::GetFont(), s.font, ImVec2(margin, margin * .8f), th.dim, title);
        ImGui::SetCursorScreenPos(ImVec2(margin + (s.W - 2 * margin - w) * .5f, margin + s.font * 3));
        widget_origin = ImGui::GetCursorScreenPos();
        float h = bv_height(p, w);
        if (frame == 3 && !g_failed) printf("  %s: widget %.0f x %.0f\n", s.file.c_str(), w, h);
        BVResult r = bv_draw("##bv", w, std::max(h, (float)s.H - 2 * margin - s.font * 3), p);
        (void)r;
        ImGui::End();
        ImGui::Render();
        img.clear(th.panel);  // (the first frames only let the font textures settle)
        render_draw_data(img, ImGui::GetDrawData());
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", dir, s.file.c_str());
    if (!write_png(path, img)) {
        fprintf(stderr, "cannot write %s\n", path);
        g_failed = true;
    }
}

// The mouse: hovering reports the input, a click on it comes back as clicked, a click on a reserved key does not
static bool interaction_test() {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1100, 860);
    ImGui::GetStyle().FontSizeBase = 14;
    Profile prof = pad_typical();
    prof.build();
    BVParams p;
    p.scheme = BV_CONTROLLER;
    p.inputs = prof.ins.data(), p.n_inputs = (int)prof.ins.size();
    p.colors = bv_colors_from(IM_COL32(6, 10, 17, 255), IM_COL32(178, 192, 208, 255), IM_COL32(244, 247, 252, 255), IM_COL32(150, 166, 184, 255),
                              IM_COL32(110, 217, 255, 255), IM_COL32(168, 236, 255, 255), IM_COL32(255, 150, 110, 255), IM_COL32(120, 240, 170, 255));
    p.font_px = 14;
    float w = 1000;
    bool ok = true;
    auto frames = [&](int scheme, ImVec2 mouse, bool down, BVResult *out) {
        p.scheme = scheme;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 860));
        ImGui::Begin("##t", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::SetCursorScreenPos(ImVec2(20, 20));
        float h = bv_height(p, w);
        *out = bv_draw("##bv", w, h, p);
        ImGui::End();
        ImGui::Render();
    };
    struct Case { int scheme; int code; bool want_click; bool want_reserved; };
    Case cases[] = {{BV_CONTROLLER, PB(EAST), true, false}, {BV_CONTROLLER, PAD_BASE + 0x40, true, false},
                    {BV_CONTROLLER, BV_STICK_L_UP, true, false}, {BV_KEYBOARD, SDL_SCANCODE_W, true, false},
                    {BV_KEYBOARD, KEY_MOUSE + SDL_BUTTON_X1, true, false}, {BV_KEYBOARD, KEY_WHEEL_DOWN, true, false},
                    {BV_KEYBOARD, SDL_SCANCODE_ESCAPE, false, true}, {BV_KEYBOARD, SDL_SCANCODE_F12, false, true}};
    for (const Case &c : cases) {
        p.scheme = c.scheme;
        ImVec2 at;
        if (!bv_input_center(p, w, c.code, &at)) {
            fprintf(stderr, "interaction test: input %d has no place on the drawing\n", c.code);
            ok = false;
            continue;
        }
        ImVec2 m(20 + at.x, 20 + at.y);
        BVResult r;
        frames(c.scheme, ImVec2(5, 5), false, &r);
        frames(c.scheme, m, false, &r);
        frames(c.scheme, m, false, &r);
        bool hovered = r.hovered == c.code && r.reserved == c.want_reserved;
        frames(c.scheme, m, true, &r);
        BVResult pressed = r;
        frames(c.scheme, m, true, &r);
        frames(c.scheme, m, false, &r);
        bool clicked = c.want_click ? r.clicked == c.code : r.clicked == 0;
        bool no_early = pressed.clicked == 0;
        if (!hovered || !clicked || !no_early) {
            fprintf(stderr, "interaction test: input %d: hovered %d (%d), clicked %d (want %d), early %d\n", c.code, r.hovered, hovered, r.clicked, c.want_click, pressed.clicked);
            ok = false;
        }
        frames(c.scheme, ImVec2(5, 5), false, &r);
        if (r.hovered != 0 || r.clicked != 0) {
            fprintf(stderr, "interaction test: nothing should be hovered away from the drawing\n");
            ok = false;
        }
    }
    printf("interaction test: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

// What a layout costs: it is built when the width, the font or the bindings change, never for a hover or a selection
static void layout_timing() {
    ImGui::GetStyle().FontSizeBase = 14;
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1300, 900);
    ImGui::NewFrame();  // (a frame is needed for the font)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::Begin("##time");
    struct { const char *name; int scheme; Profile prof; } sets[] = {{"controller, typical", BV_CONTROLLER, pad_typical()},
                                                                      {"controller, 24 inputs", BV_CONTROLLER, pad_heavy()},
                                                                      {"keyboard, typical", BV_KEYBOARD, kbm_typical()},
                                                                      {"keyboard, 66 inputs", BV_KEYBOARD, kbm_heavy()}};
    for (auto &set : sets) {
        set.prof.build();
        BVParams p;
        p.scheme = set.scheme;
        p.inputs = set.prof.ins.data(), p.n_inputs = (int)set.prof.ins.size();
        p.font_px = 14;
        Uint64 t0 = SDL_GetPerformanceCounter();
        const int n = 20;
        for (int i = 0; i < n; i++) bv_height(p, 900.f + i * 7);  // (a new width each time: a new layout)
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency() / n;
        printf("  layout %-22s %.2f ms\n", set.name, ms);
    }
    ImGui::End();
    ImGui::EndFrame();
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *dir = argc > 1 ? argv[1] : "obj/binding-preview";
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefaultVector();
    std::vector<Scenario> list;
    auto add = [&](Scenario s) { list.push_back(std::move(s)); };
    auto mk = [](const char *file, const char *title, int W, int H, int scheme, Profile prof) {
        Scenario s;
        s.file = file, s.title = title, s.W = W, s.H = H, s.scheme = scheme, s.prof = std::move(prof);
        return s;
    };
    // controller
    {
        Scenario s = mk("pad_empty", "Controller, nothing bound", 1100, 760, BV_CONTROLLER, Profile());
        add(s);
        s = mk("pad_light", "Controller, four inputs", 1100, 760, BV_CONTROLLER, pad_light());
        add(s);
        s = mk("pad_typical", "Controller, a full profile", 1100, 860, BV_CONTROLLER, pad_typical());
        add(s);
        s = mk("pad_selected_north", "Controller, selected: North, capturing", 1100, 860, BV_CONTROLLER, pad_typical());
        s.selected_input = PB(NORTH), s.capturing = true, s.selected_label = "Attack X / draw";
        add(s);
        s = mk("pad_selected_l2", "Controller, selected: L2", 1100, 860, BV_CONTROLLER, pad_typical());
        s.selected_input = PAD_BASE + 0x40, s.selected_label = "ZL button";
        add(s);
        s = mk("pad_held", "Controller, buttons held", 1100, 860, BV_CONTROLLER, pad_typical());
        s.held = {PB(SOUTH), PB(RIGHT_SHOULDER), PB(DPAD_LEFT)};
        s.focus_input = PB(START);
        add(s);
        s = mk("pad_conflicts", "Controller, conflicts", 1100, 860, BV_CONTROLLER, pad_conflicts());
        s.selected_input = PB(SOUTH), s.selected_label = "Dodge / roll";
        add(s);
        s = mk("pad_hover", "Controller, mouse over East", 1100, 860, BV_CONTROLLER, pad_typical());
        s.mouse_x = 743, s.mouse_y = 341;
        add(s);
    }
    // keyboard and mouse
    {
        Scenario s = mk("kbm_empty", "Keyboard and mouse, nothing bound", 1300, 800, BV_KEYBOARD, Profile());
        add(s);
        s = mk("kbm_typical", "Keyboard and mouse, a full profile", 1300, 1000, BV_KEYBOARD, kbm_typical());
        add(s);
        s = mk("kbm_selected_space", "Keyboard, selected: Space, capturing", 1300, 1000, BV_KEYBOARD, kbm_typical());
        s.selected_input = SDL_SCANCODE_SPACE, s.capturing = true, s.selected_label = "Dodge / roll";
        add(s);
        s = mk("kbm_selected_mouse", "Mouse, selected: left button", 1300, 1000, BV_KEYBOARD, kbm_typical());
        s.selected_input = KEY_MOUSE + SDL_BUTTON_LEFT, s.selected_label = "Attack X";
        add(s);
        s = mk("kbm_conflicts", "Keyboard and mouse, conflicts and held keys", 1300, 1000, BV_KEYBOARD, kbm_conflicts());
        s.held = {SDL_SCANCODE_W, SDL_SCANCODE_LSHIFT};
        add(s);
        s = mk("kbm_hover_reserved", "Keyboard, mouse over Esc (kept by the recomp)", 1300, 1000, BV_KEYBOARD, kbm_typical());
        s.mouse_x = 33, s.mouse_y = 365;
        add(s);
    }
    // a profile with everything bound
    {
        Scenario s = mk("heavy_pad", "Controller, every input with several actions, conflicts, a stray input", 1300, 1000, BV_CONTROLLER, pad_heavy());
        s.selected_input = PB(WEST), s.selected_label = "Attack X";
        add(s);
        s = mk("heavy_kbm", "Keyboard and mouse, 66 inputs bound", 1300, 1100, BV_KEYBOARD, kbm_heavy());
        s.selected_input = SDL_SCANCODE_G, s.selected_label = "Attack X";
        add(s);
        s = mk("heavy_kbm_900", "Keyboard and mouse, 66 inputs bound, 900 px", 960, 1100, BV_KEYBOARD, kbm_heavy());
        s.font = 12, s.region_w = 900;
        add(s);
    }
    // sizes, in the way the settings menu would host them
    {
        Scenario s = mk("size_720p_pad", "Controller in a 560 px wide panel", 640, 720, BV_CONTROLLER, pad_typical());
        s.font = 12, s.region_w = 600;
        add(s);
        s = mk("size_720p_kbm", "Keyboard in a 900 px wide panel", 960, 760, BV_KEYBOARD, kbm_typical());
        s.font = 12, s.region_w = 900;
        add(s);
        s = mk("size_1080p_pad", "Controller, 1080p menu", 1920, 1080, BV_CONTROLLER, pad_typical());
        s.font = 18, s.region_w = 1500, s.selected_input = PB(EAST), s.selected_label = "Confirm";
        add(s);
        s = mk("size_4k_pad", "Controller, 4K menu", 3840, 2160, BV_CONTROLLER, pad_typical());
        s.font = 36, s.region_w = 3000, s.selected_input = PB(EAST), s.selected_label = "Confirm";
        add(s);
        s = mk("size_4k_kbm", "Keyboard, 4K menu", 3840, 2160, BV_KEYBOARD, kbm_typical());
        s.font = 36, s.region_w = 3300, s.selected_input = SDL_SCANCODE_E, s.selected_label = "Confirm";
        add(s);
        s = mk("size_ultrawide_pad", "Controller, ultrawide 3440x1440", 3440, 1440, BV_CONTROLLER, pad_typical());
        s.font = 24, s.region_w = 2800, s.selected_input = PB(WEST), s.selected_label = "Use item / sheathe";
        add(s);
        s = mk("size_narrow_pad", "Controller, a very narrow panel", 480, 640, BV_CONTROLLER, pad_typical());
        s.font = 11, s.region_w = 440;
        add(s);
        s = mk("size_narrow_kbm", "Keyboard, a narrow panel", 560, 760, BV_KEYBOARD, kbm_typical());
        s.font = 11, s.region_w = 520;
        add(s);
    }
    // themes
    for (int t = 0; t < 4; t++) {
        char f[64], title[96];
        snprintf(f, sizeof f, "theme_%s_pad", THEMES[t].name);
        snprintf(title, sizeof title, "Controller, theme %s", THEMES[t].name);
        Scenario s = mk(f, title, 1100, 860, BV_CONTROLLER, pad_typical());
        s.theme = t, s.selected_input = PB(NORTH), s.selected_label = "Attack X / draw";
        s.held = {PB(RIGHT_SHOULDER)};
        add(s);
        snprintf(f, sizeof f, "theme_%s_kbm", THEMES[t].name);
        snprintf(title, sizeof title, "Keyboard, theme %s", THEMES[t].name);
        s = mk(f, title, 1300, 1000, BV_KEYBOARD, kbm_typical());
        s.theme = t, s.selected_input = SDL_SCANCODE_SPACE, s.selected_label = "Dodge / roll";
        add(s);
    }
    const char *only = argc > 2 ? argv[2] : nullptr;
    printf("binding view preview -> %s\n", dir);
    for (const Scenario &s : list) {
        if (only && !strstr(s.file.c_str(), only)) continue;
        render(s, dir);
    }
    if (!only && !interaction_test()) g_failed = true;
    if (!only) layout_timing();
    return g_failed ? 1 : 0;
}
