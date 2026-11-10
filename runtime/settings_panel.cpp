#include "settings_panel.h"
#include "actions.h"
#include "android_files.h"
#include "binding_view.h"
#include "config.h"
#include "menu_confirm.h"
#include "settings_locale.h"
#include "settings_icons.h"
#include "app_actions.h"
#include "cheats.h"
#include "forge.h"
#include "overlay.h"
#include "keymap.h"
#include "modorder.h"
#include "net/net.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

namespace {
enum class View { Main, Keyboard, CloseChanges, Exit, Restart, Reset, Move, Conflicts, KeyCapture, Keybinds, Actions };
enum class Command { None, Save };
struct Row {
    std::string id, title, value;
    std::string key, help;  // its ini key (config.c g_settings; its help: config_help), or a help text of its own
    bool enabled = true, cold = false, action = false, warn = false;
    bool header = false, current = false;  // a section title (not a row to select); the choice a dropdown has now
    bool menu = false;                     // Confirm opens a dropdown of its choices
    std::function<void(int)> adjust;
    std::function<void()> activate;
};
struct Target {
    std::string id;
    ImVec2 center;
    bool enabled;
};
struct Origin {
    View view;
    std::string focus;
    float scroll;
};
// A row's choices, listed in a window of their own under it (or above it, near the bottom) while it is open: the
// controller moves in the list, Confirm picks, Back or a click elsewhere closes it
struct Dropdown {
    bool open = false, hovered = false, anchored = false;
    std::string row;  // the row it belongs to
    std::vector<std::string> names;
    int selected = 0;
    std::function<void(int)> set;
    ImRect anchor;  // that row on the screen, this frame (draw_rows)
};
struct Keyboard {
    std::string title, original, buffer;
    size_t cursor = 0, capacity = 255;
    bool upper = false, select_all = false;
    std::function<bool(const std::string &, std::string &)> commit;
    bool paste_only = false;  // (Add a cheat: Paste / Clear / Add / Cancel instead of the on-screen keys)
};
struct State {
    Config draft = {}, saved = {};
    bool editing = false, floating = false, advanced = false, focus_request = true, saved_notice = false;
    int page = 0;
    View view = View::Main;
    Command command = Command::None;
    std::string focus, error;
    std::string help_row, hovered;  // the row whose help shows (the one focused or pointed at last); the one pointed at
    bool pointing = false;          // the mouse pointer is on one of the view's rows (hovered: also the focused one)
    float help_height = 0;          // room for the longest help of the tab (last frame's)
    std::vector<Target> targets;
    std::vector<Origin> origins;
    Dropdown dropdown;
    Keyboard keyboard;
    uint64_t quest_keys[HP_QUESTS_MAX] = {}, next_quest_key = 1;
    bool restore_scroll = false;
    float scroll = 0, guide_height = 0;
    int focus_scroll_frames = 0;
    ImVec2 move_start = {}, move_position = {};
    // The mouse: dragging the top of the menu moves it, the grip at its bottom right corner sizes it (custom_size,
    // 0: as tall as its rows, as wide as it is by default); either one detaches it
    ImVec2 custom_size = {}, drag_mouse = {}, drag_from = {};
    int dragging = 0;  // 1 moving, 2 sizing
    bool dragged = false;  // (the press moved the menu: the tab under it is not chosen when it lets go)
    float header_bottom = 0;  // (the top part that moves the menu, from the window's top: last frame's)
    bool position_override = false;
    int capture_bind = -1;     // the keybind (BIND_*) a key is being chosen for; -1: the chat key
    bool capture_add = false;  // the key chosen is that keybind's second, else its only one
    bool capture_from_actions = false;  // the keybind or chat key is being chosen from the Game actions screen (Keys)
} s;
std::atomic<bool> request_exit, request_back, is_dirty, is_confirming, is_capturing, capture_mouse, capture_wheel,
    capture_pointing, capture_pad;
// The game actions (runtime/actions.h, the [Actions*] sections of the ini): edited as a draft like the settings, saved
// with them. dev: the device whose bindings the rows show (0 the controller, 1 keyboard and mouse); weapon: 0 every
// weapon, 1..15 that class's own bindings; capture: the action an input is being chosen for (-1: none)
struct ActionTab {
    ActProfile draft, saved;
    int tab = 0;  // what the list and the drawing show: an index into tab_defs()
    int dev = 0, weapon = 0, capture = -1, capture_row = -1, layout = ACT_LAYOUT_SIMPLE;  // layout: the one shown (a profile without a Layout key is Advanced once it has bindings: not while it is edited)
     // (capture_row: a merged row of the Simple layout)
    bool capture_add = false;
    Uint64 capture_start = 0;
};
ActionTab &acts() {
    static ActionTab *tab = new ActionTab();  // (two profiles: too big for the stack, and zeroed)
    return *tab;
}
// (the key pressed while a key is being chosen, or KEY_MOUSE + a mouse button; -1: none yet)
std::atomic<int> captured_key{-1};
bool forge_requested;  // the Mods tab's "Open the Forge menu" (game thread, as the rest of the panel)
const char *tr(const char *text) { return settings_text(s.draft.language, text); }
std::string fmt(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    vsnprintf(buffer, sizeof buffer, tr(text), args);
    va_end(args);
    return buffer;
}
// The menu's colours ([Application] Theme, applied as it is picked): Blue, the original look; Gold, a warm panel
// where rows light up with a gold bar at their left instead of a frame, and tabs that are only their names,
// underlined; and Hunter, after the game's own windows (the item pouch) - a maroon panel with a red and green rule at
// its top and bottom, gold corners, rows split by lines and the focused one filled gold.
struct Theme {
    ImU32 panel, edge, row, row_hover, row_focus, tab, tab_active, accent, accent_bright, text, text_dim, text_off,
        warn, divider, popup;
    bool bar;         // the focused row: lit with a bar at its left (else framed)
    float rounding;   // of the window, the rows and the tabs
    bool game;        // Hunter: the game's window frame (draw_frame), lines between the rows, a filled header
    ImU32 header;     // (game: the header band's colour)
};
const Theme &theme() {
    static const Theme blue = {IM_COL32(6, 10, 17, 250),     IM_COL32(110, 217, 255, 255), IM_COL32(23, 33, 48, 255),
                               IM_COL32(38, 71, 99, 255),    IM_COL32(41, 82, 117, 255),   IM_COL32(15, 23, 36, 255),
                               IM_COL32(46, 102, 143, 255),  IM_COL32(111, 216, 255, 255), IM_COL32(111, 216, 255, 255),
                               IM_COL32(244, 247, 252, 255), IM_COL32(178, 192, 208, 255), IM_COL32(132, 145, 160, 255),
                               IM_COL32(255, 184, 145, 255), IM_COL32(52, 72, 94, 255),    IM_COL32(13, 20, 31, 255),
                               false,                        3.f};
    static const Theme gold = {IM_COL32(34, 25, 19, 248),    IM_COL32(139, 97, 40, 255),   IM_COL32(251, 236, 206, 10),
                               IM_COL32(229, 195, 126, 28),  IM_COL32(213, 162, 79, 64),   IM_COL32(0, 0, 0, 0),
                               IM_COL32(0, 0, 0, 0),         IM_COL32(213, 162, 79, 255),  IM_COL32(247, 226, 162, 255),
                               IM_COL32(242, 216, 170, 255), IM_COL32(175, 150, 128, 255), IM_COL32(124, 107, 90, 255),
                               IM_COL32(210, 106, 80, 255),  IM_COL32(139, 97, 40, 90),    IM_COL32(42, 31, 23, 252),
                               true,                         6.f};
    static const Theme hunter = {IM_COL32(50, 25, 24, 236),    IM_COL32(0, 0, 0, 255),       IM_COL32(0, 0, 0, 0),
                                 IM_COL32(255, 255, 255, 20),  IM_COL32(176, 145, 24, 235),  IM_COL32(0, 0, 0, 0),
                                 IM_COL32(0, 0, 0, 0),         IM_COL32(236, 212, 44, 255),  IM_COL32(255, 232, 84, 255),
                                 IM_COL32(242, 238, 232, 255), IM_COL32(196, 176, 166, 255), IM_COL32(132, 112, 106, 255),
                                 IM_COL32(255, 96, 72, 255),   IM_COL32(96, 88, 88, 255),    IM_COL32(56, 28, 27, 250),
                                 false,                        0.f,                          true,
                                 IM_COL32(108, 46, 46, 255)};
    return s.draft.menu_theme == THEME_BLUE ? blue : s.draft.menu_theme == THEME_HUNTER ? hunter : gold;
}
ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
// The Mods tab: the mods folder's state (modorder.h), which is saved with the rest of the settings
#ifdef __ANDROID__
constexpr int PAGES = 9, MODS_PAGE = 5, CHEATS_PAGE = 6, APP_PAGE = 7, ANDROID_PAGE = 8;  // (Android: the tab for its own settings)
#else
constexpr int PAGES = 8, MODS_PAGE = 5, CHEATS_PAGE = 6, APP_PAGE = 7;
#endif
struct ScanResult {
    std::vector<std::string> names;  // (the order ModConflict's indexes are in)
    ModScan scan = {};
    std::atomic<bool> done{false};
    ~ScanResult() { modscan_free(&scan); }
};
struct Mods {
    std::vector<ModEntry> draft, saved, boot;
    bool boot_set = false;
    std::shared_ptr<ScanResult> scan;
} mods;
bool same_mods(const std::vector<ModEntry> &a, const std::vector<ModEntry> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].on != b[i].on || strcmp(a[i].name, b[i].name))
            return false;
    return true;
}
bool mods_dirty() { return !same_mods(mods.draft, mods.saved); }
// The Cheats tab: each cheat on or off (cheats.h), applied while the game runs when the settings are saved
struct Cheats {
    std::vector<char> draft, saved;
} cheat_tab;
bool cheats_dirty() { return cheat_tab.draft != cheat_tab.saved; }
void load_cheats() {
    cheat_tab.draft.assign(cheats_count(), 0);
    for (int i = 0; i < cheats_count(); ++i)
        cheat_tab.draft[i] = cheats_on(i);
    cheat_tab.saved = cheat_tab.draft;
}
bool mods_restart() { return !same_mods(mods.saved, mods.boot); }
void load_mods() {
    ModEntry all[MODS_MAX];
    int n = mods_installed(all, MODS_MAX);
    mods.draft.assign(all, all + n);
    mods.saved = mods.draft;
    if (!mods.boot_set) {
        mods.boot = mods.draft;
        mods.boot_set = true;
    }
    mods.scan.reset();
}
void start_scan() {
    auto result = std::make_shared<ScanResult>();
    for (const auto &m : mods.draft)
        result->names.push_back(m.name);
    std::sort(result->names.begin(), result->names.end(),
              [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    mods.scan = result;
    std::thread([result]() {
        std::vector<std::array<char, 128>> names;
        for (const auto &n : result->names) {
            names.emplace_back();
            snprintf(names.back().data(), 128, "%s", n.c_str());
        }
        modscan_run(reinterpret_cast<const char(*)[128]>(names.data()), (int)names.size(), &result->scan);
        result->done = true;
    }).detach();
}
int mod_index(const std::string &name) {
    for (int i = 0; i < (int)mods.draft.size(); ++i)
        if (name == mods.draft[i].name)
            return i;
    return -1;
}
bool dirty();
struct GuideLayout {
    ImVec2 offset[4];
    float height = 0;
};
GuideLayout guide_layout(float width) {
    GuideLayout layout;
    float f = ImGui::GetFontSize(), x = 0, y = 0;
    const char *labels[] = {"Navigation", "Confirm", "Back", "Save"};
    for (int i = 0; i < 4; i++) {
        float item_width = f * 1.4f + ImGui::CalcTextSize(tr(labels[i])).x;
        if (x && x + item_width > width) {
            x = 0;
            y += f + ImGui::GetStyle().ItemSpacing.y;
        }
        layout.offset[i] = ImVec2(x, y);
        x += item_width + f;
    }
    layout.height = y + f;
    return layout;
}
// A row's help at the bottom, wrapped as ImGui wraps it (so its height is what CalcTextSize measured), with the
// warnings in it (shout) in bright red.
void draw_help(ImDrawList *draw, ImVec2 pos, ImU32 col, const std::string &text, float wrap) {
    static const char *const shout[] = {"NOT FOR HDR400 DISPLAYS"};
    const ImU32 red = IM_COL32(255, 20, 20, 255);
    std::vector<bool> loud(text.size(), false);
    for (const char *en : shout)
        for (const char *w : {en, tr(en)})  // (English, and the menu's language)
            for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1))
                for (size_t i = at; i < at + strlen(w); i++) loud[i] = true;
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize(), y = pos.y;
    const char *base = text.c_str(), *end = base + text.size();
    for (const char *line = base; line <= end;) {
        const char *nl = (const char *)memchr(line, '\n', (size_t)(end - line));
        const char *line_end = nl ? nl : end;
        const char *s = line;
        do {
            const char *eol = line_end > s ? ImFontCalcWordWrapPositionEx(font, size, s, line_end, wrap) : s;
            if (eol <= s && s < line_end) eol = s + 1;
            float x = pos.x;
            for (const char *a = s; a < eol;) {
                const char *b = a;
                bool l = loud[(size_t)(a - base)];
                while (b < eol && loud[(size_t)(b - base)] == l) b++;
                draw->AddText(font, size, ImVec2(x, y), l ? red : col, a, b);
                x += font->CalcTextSizeA(size, FLT_MAX, 0.f, a, b).x;
                a = b;
            }
            y += size;
            s = eol < line_end ? ImTextCalcWordWrapNextLineStart(eol, line_end) : line_end;
        } while (s < line_end);
        if (!nl) break;
        line = nl + 1;
    }
}

void draw_guides(ImGuiWindow *window, const GuideLayout &layout, float top) {
    auto *draw = window->DrawList;
    ImVec2 start(window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x, top);
    ImU32 background = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK;
    draw->AddRectFilled(ImVec2(window->InnerRect.Min.x, top - ImGui::GetStyle().ItemSpacing.y),
                        ImVec2(window->InnerRect.Max.x, top + layout.height), background);
    const int icons[] = {SettingsIcons::Dpad, SettingsIcons::Confirm, SettingsIcons::Back, SettingsIcons::Save};
    const char *labels[] = {"Navigation", "Confirm", "Back", "Save"};
    float f = ImGui::GetFontSize();
    for (int i = 0; i < 4; i++) {
        ImVec2 pos(start.x + layout.offset[i].x, start.y + layout.offset[i].y);
        // (Save: the top face button, Y / X / triangle - Ctrl+S on the keyboard - greyed while there is nothing to save)
        SettingsIcons::draw(draw, pos, f, s.draft.button_icons, icons[i],
                            i == 3 ? 3
                                   : menu_confirm_position(config_live_menu_confirm(), overlay_face_position(0),
                                                           overlay_face_position(1), i == 2));
        draw->AddText(ImVec2(pos.x + f * 1.4f, pos.y),
                      i == 3 && !dirty() ? theme().text_off : ImGui::GetColorU32(ImGuiCol_Text), tr(labels[i]));
    }
}
bool confirmation() {
    return s.view == View::CloseChanges || s.view == View::Exit || s.view == View::Restart ||
           s.view == View::Reset;
}
bool equal(const Config &a, const Config &b) {
#define SAME(f)                                                                                              \
    if (a.f != b.f)                                                                                          \
    return false
    SAME(display_mode);
    SAME(width);
    SAME(height);
    SAME(render_w);
    SAME(render_h);
    SAME(fps);
    SAME(fsr);
    SAME(fsr_quality);
    SAME(fsr_sharpness);
    SAME(fsr_objmotion);
    SAME(fsr_reactive);
    SAME(fsr_animsurf);
    SAME(vsync);
    SAME(shader_prewarm);
    SAME(draw_distance);
    SAME(smooth_gradients);
    SAME(fix_kerning);
    SAME(bold_text);
    SAME(hdr);
    SAME(hdr_peak);
    SAME(hdr_game);
    SAME(hdr_ui);
    SAME(hdr_tonemap);
    SAME(hdr_gamma);
    SAME(hdr_exposure);
    SAME(hdr_highlights);
    SAME(hdr_shadows);
    SAME(hdr_contrast);
    SAME(hdr_saturation);
    SAME(hdr_highlight_saturation);
    SAME(hdr_flare);
    SAME(hdr_bloom);
    SAME(hdr_scene);
    SAME(hdr_format);
    SAME(hdr_analysis);
    SAME(msaa);
    SAME(fxaa);
    SAME(fxaa_mode);
    SAME(face_buttons);
    SAME(ldn_port);
    SAME(sync_small_monsters);
    SAME(ldn_direct);
    SAME(bonus_content);
    SAME(village_in_hub);
    SAME(share_urgent);
    SAME(mouse_sensitivity);
    SAME(aspect_ratio);
    SAME(fov);
    SAME(ui_anchor);
    SAME(background_input);
    SAME(combo_window);
    SAME(volume);
    SAME(mute_in_background);
    SAME(button_icons);
    SAME(menu_theme);
    SAME(discord);
    SAME(discord_hunter);
    SAME(language);
    SAME(menu_scale);
    SAME(menu_confirm);
    SAME(confirm_exit);
    SAME(show_fps);
    SAME(menu_chord);
    SAME(show_battery);
    SAME(updates);
    SAME(hp.village);
    SAME(hp.quests);
#undef SAME
    if (memcmp(a.binds, b.binds, sizeof a.binds))
        return false;
    if (strcmp(a.ldn_join, b.ldn_join) || strcmp(a.ldn_server, b.ldn_server) || strcmp(a.ldn_key, b.ldn_key) ||
        strcmp(a.chat_key, b.chat_key) || strcmp(a.save_dir, b.save_dir) || strcmp(a.save_location, b.save_location) || strcmp(a.game_dir, b.game_dir))
        return false;
    for (int n = 0; n < 4; ++n)
        if (a.hp.scale[n] != b.hp.scale[n])
            return false;
    for (int i = 0; i < a.hp.quests; ++i) {
        if (a.hp.quest[i].quest != b.hp.quest[i].quest)
            return false;
        for (int n = 0; n < 4; ++n)
            if (a.hp.quest[i].scale[n] != b.hp.quest[i].scale[n])
                return false;
    }
    return true;
}
bool actions_dirty() { return memcmp(&acts().draft, &acts().saved, sizeof(ActProfile)) != 0; }
bool dirty() { return !equal(s.draft, s.saved) || mods_dirty() || cheats_dirty() || actions_dirty(); }
bool pending_text() {
    bool editing = s.view == View::Keyboard;
    for (const auto &o : s.origins)
        editing |= o.view == View::Keyboard;
    return editing && s.keyboard.buffer != s.keyboard.original;
}
std::string quest_id(int index) { return "quest." + std::to_string(s.quest_keys[index]); }
void quest_keys() {
    for (int i = 0; i < s.draft.hp.quests; ++i)
        s.quest_keys[i] = s.next_quest_key++;
}
void focus(const std::string &id) {
    s.focus = id;
    s.focus_request = true;
}
void close_dropdown() {
    if (!s.dropdown.open)
        return;
    s.dropdown.open = false;
    focus(s.dropdown.row);
}
void push(View view, const std::string &first) {
    s.dropdown.open = false;
    auto *root = ImGui::FindWindowByName("Settings##main");
    s.origins.push_back({s.view, s.focus, root ? root->Scroll.y : 0});
    s.view = view;
    focus(first);
    s.error.clear();
    s.restore_scroll = true;
    s.scroll = 0;
    ImGui::ClearActiveID();
}
void pop() {
    s.dropdown.open = false;
    if (s.origins.empty()) {
        s.view = View::Main;
        s.focus.clear();
    } else {
        auto o = s.origins.back();
        s.origins.pop_back();
        s.view = o.view;
        focus(o.focus);
        s.scroll = o.scroll;
    }
    s.restore_scroll = true;
    s.error.clear();
    ImGui::ClearActiveID();
}
void first_page(int page) {
    s.dropdown.open = false;
    s.page = page;
    s.view = View::Main;
    s.origins.clear();
    s.focus.clear();
    s.focus_request = true;
    s.restore_scroll = true;
    s.scroll = 0;
    s.error.clear();
}
void discard() {
    s.draft = s.saved;
    acts().draft = acts().saved;
    mods.draft = mods.saved;
    cheat_tab.draft = cheat_tab.saved;
    quest_keys();
    s.error.clear();
    s.saved_notice = false;
}
void ask_back(bool *open) {
    if (s.dropdown.open)
        close_dropdown();
    else if (s.view != View::Main) {
        if (s.view == View::Move) {
            s.move_position = s.move_start;
            s.position_override = true;
        }
        pop();
    } else if (dirty())
        push(View::CloseChanges, "dialog.cancel");
    else
        *open = false;
}
void keyboard(const std::string &title, const std::string &initial, size_t cap,
              std::function<bool(const std::string &, std::string &)> commit) {
    s.keyboard = {title, initial, initial, initial.size(), cap, false, false, std::move(commit)};
    push(View::Keyboard, "key.0");
}
void insert(const std::string &text);
void paste_box(const std::string &title, size_t cap, std::function<bool(const std::string &, std::string &)> commit) {
    s.keyboard = {title, "", "", 0, cap, false, false, std::move(commit), true};
    push(View::Keyboard, "key.paste");
}
void paste_clipboard() {
    char *clip = SDL_GetClipboardText();
    if (clip) {
        insert(clip);
        SDL_free(clip);
    }
}
void insert(const std::string &text) {
    if (s.keyboard.select_all) {
        s.keyboard.buffer.clear();
        s.keyboard.cursor = 0;
        s.keyboard.select_all = false;
    }
    if (s.keyboard.buffer.size() + text.size() > s.keyboard.capacity)
        return;
    s.keyboard.buffer.insert(s.keyboard.cursor, text);
    s.keyboard.cursor += text.size();
    s.error.clear();
}
void erase_previous() {
    if (s.keyboard.select_all) {
        s.keyboard.buffer.clear();
        s.keyboard.cursor = 0;
        s.keyboard.select_all = false;
        return;
    }
    if (!s.keyboard.cursor)
        return;
    size_t begin = s.keyboard.cursor - 1;
    while (begin && (static_cast<unsigned char>(s.keyboard.buffer[begin]) & 0xc0) == 0x80)
        --begin;
    s.keyboard.buffer.erase(begin, s.keyboard.cursor - begin);
    s.keyboard.cursor = begin;
    s.error.clear();
}
bool accept_keyboard() {
    if (!s.keyboard.commit(s.keyboard.buffer, s.error))
        return false;
    pop();
    return true;
}
// The row's choices in a dropdown under it; selected: the one it has now (-1: none of them)
void choices(const Row &row, std::vector<std::string> names, int selected, std::function<void(int)> set) {
    int first = std::clamp(selected, 0, (int)names.size() - 1);
    s.dropdown = {true, false, false, row.id, std::move(names), selected, std::move(set), {}};
    focus("choice." + std::to_string(first));
}
// What the settings table (config.c g_settings) says of a setting: does it need a restart, a number's range. ini():
// the key a row is for (its restart mark and its help)
const char *ini(const char *key) { return key; }
bool cold(const char *key) { return key && config_restart_key(key); }
float lo(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return a; }
float hi(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return b; }
void keyed(Row &r, const char *key) {
    r.key = key ? key : "";
    r.cold = cold(key);
}
void add_bool(std::vector<Row> &rows, const char *id, const char *name, int *value, const char *key = nullptr) {
    Row r;
    r.id = id;
    r.title = tr(name);
    keyed(r, key);
    r.value = tr(*value ? "On" : "Off");
    r.adjust = [value](int d) { *value = d > 0 ? 1 : 0; };
    r.activate = [value]() { *value = !*value; };
    rows.push_back(std::move(r));
}
void add_choice(std::vector<Row> &rows, const char *id, const char *name, int selected,
                const std::vector<std::string> &names, std::function<void(int)> set, const char *key = nullptr) {
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = names[std::clamp(selected, 0, (int)names.size() - 1)];
    keyed(r, key);
    r.adjust = [selected, names, set](int d) { set((selected + d + (int)names.size()) % (int)names.size()); };
    Row copy = r;
    r.menu = true;
    r.activate = [copy, names, selected, set]() { choices(copy, names, selected, set); };
    rows.push_back(std::move(r));
}
void add_number(std::vector<Row> &rows, const std::string &id, const std::string &name,
                std::function<double()> get, std::function<void(double)> set, double low, double high,
                double step, bool integral, const char *key = nullptr,
                std::function<bool(double, std::string &)> check = {}) {
    char text[64];
    snprintf(text, sizeof text, integral ? "%.0f" : "%.2f", get());
    Row r;
    r.id = id;
    r.title = name;
    r.value = text;
    keyed(r, key);
    r.adjust = [get, set, low, high, step, check](int d) {
        double v = std::clamp(get() + d * step, low, high);
        std::string error;
        for (int attempts = 0; attempts <= HP_QUESTS_MAX; ++attempts) {
            if (!check || check(v, error)) {
                set(v);
                break;
            }
            double next = std::clamp(v + d * step, low, high);
            if (next == v)
                break;
            v = next;
        }
    };
    rows.push_back(std::move(r));
}
void number(std::vector<Row> &r, const char *id, const char *name, int *p, int low, int high, int step = 1,
            const char *key = nullptr) {
    add_number(
        r, id, tr(name), [p]() { return (double)*p; }, [p](double v) { *p = (int)v; }, low, high, step, true,
        key);
}
// The largest window the screens can show (the Resolution rows' range): the displays' desktop sizes and fullscreen
// modes (DSR / VSR ones included), at most the file's 16384; 16384 when no display is known
void screen_max(int &w, int &h) {
    static int mw, mh;
    if (!mw) {
        int n = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&n);
        for (int i = 0; ids && i < n; ++i) {
            if (const SDL_DisplayMode *d = SDL_GetDesktopDisplayMode(ids[i]))
                mw = std::max(mw, d->w), mh = std::max(mh, d->h);
            int m = 0;
            SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(ids[i], &m);
            for (int k = 0; modes && k < m; ++k)
                mw = std::max(mw, modes[k]->w), mh = std::max(mh, modes[k]->h);
            SDL_free(modes);
        }
        SDL_free(ids);
        if (mw < 320 || mh < 180)
            mw = mh = 16384;
        mw = std::min(mw, 16384), mh = std::min(mh, 16384);
    }
    w = mw, h = mh;
}
void number(std::vector<Row> &r, const std::string &id, const std::string &name, float *p, float low,
            float high, float step, const char *key = nullptr) {
    add_number(
        r, id, name, [p]() { return (double)*p; }, [p](double v) { *p = (float)v; }, low, high, step, false,
        key);
}
// A section's title between the rows (not selectable)
void header(std::vector<Row> &rows, const std::string &id, const std::string &title) {
    Row r;
    r.id = id;
    r.title = title;
    r.header = true;
    r.enabled = false;
    rows.push_back(std::move(r));
}
// A gap between groups of rows (a header without a title)
void spacer(std::vector<Row> &rows, const std::string &id) { header(rows, id, ""); }
void action(std::vector<Row> &rows, const std::string &id, const std::string &name, std::function<void()> fn,
            bool enabled = true) {
    Row r;
    r.id = id;
    r.title = name;
    r.action = true;
    r.activate = std::move(fn);
    r.enabled = enabled;
    rows.push_back(std::move(r));
}
// A text setting (a char array of the draft, size bytes) typed on the on-screen keyboard; shown: its row's value
void text(std::vector<Row> &rows, const char *id, const char *name, char *field, size_t size, const std::string &shown,
          const char *key) {
    size_t at = (size_t)(field - (char *)&s.draft);
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = shown;
    keyed(r, key);
    r.activate = [name, at, size]() {
        keyboard(tr(name), (char *)&s.draft + at, size - 1, [at, size](const std::string &typed, std::string &error) {
            Config temp = s.draft;
            SDL_strlcpy((char *)&temp + at, typed.c_str(), size);
            char why[512];
            if (!config_validate(&temp, why, sizeof why)) {
                error = settings_text(s.draft.language, why);
                return false;
            }
            s.draft = temp;
            return true;
        });
    };
    rows.push_back(std::move(r));
}
// The hub server and key being edited, checked on request (runtime/net/ldn.c); the value: the server's last known
// state, from the check or from the game's own searches and hubs
void hub_check(std::vector<Row> &rows) {
    int ms = -1, hubs = -1;
    std::string state;
    switch (ldn_hub_status(s.draft.ldn_server, s.draft.ldn_key, &ms, &hubs)) {
    case HUB_CHECKING: state = tr("Checking..."); break;
    case HUB_OFF: state = tr("Off"); break;
    case HUB_ONLINE:
        state = hubs >= 0 ? fmt("Online, %d ms, %d hub(s)", ms, hubs) : ms >= 0 ? fmt("Online, %d ms", ms) : tr("Online");
        break;
    case HUB_NOT_FOUND: state = tr("Not found"); break;
    case HUB_UNREACHABLE: state = tr("Unreachable"); break;
    case HUB_LOST: state = tr("Connection lost"); break;
    case HUB_NO_ANSWER: state = tr("No answer"); break;
    case HUB_BUSY: state = tr("Busy"); break;
    case HUB_VERSION: state = tr("Other version"); break;
    case HUB_REFUSED: state = tr("Refused"); break;
    default: state = tr("Not checked"); break;
    }
    Row r;
    r.id = "server.check";
    r.title = tr("Check hub server");
    r.help = tr("Asks the hub server above whether it answers, with the matchmaking key above: how long it takes, and how many hubs it lists.");
    r.value = state;
    r.activate = []() { ldn_hub_check(s.draft.ldn_server, s.draft.ldn_key); };
    rows.push_back(std::move(r));
}
// The keybinds' rows (config.h BIND_*): what each is called
const char *const bind_titles[BINDS] = {"Move forward", "Move back", "Move left", "Move right", "Walk", "A", "B", "X",
                                        "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-pad up", "D-pad down",
                                        "D-pad left", "D-pad right", "Left stick click", "Right stick click",
                                        "Item left", "Item right"};
// What else a key (or KEY_MOUSE + a mouse button) does: one of the recomp's own keys (keymap.h), a keybind other than
// skip, or (for a keybind's) the chat key; nullptr: nothing
const char *key_use(int code, int skip) {
    if (const char *use = code < KEY_MOUSE ? key_reserved(code) : nullptr)
        return tr(use);
    for (int i = 0; i < BINDS; i++)
        for (int k = 0; k < BIND_KEYS; k++)
            if (i != skip && (s.draft.binds[i][k] == code || (config_mod_bound(i) && config_live_bind(i, k) == code)))
                return tr(bind_titles[i]);
    if (skip >= 0 && code == (int)SDL_GetScancodeFromName(s.draft.chat_key))
        return tr("Chat key");
    return nullptr;
}
// Choosing a key: for a keybind (BIND_*, add: its second key) or the chat key (-1)
void capture(int bind, bool add) {
    acts().capture = -1;
    s.capture_bind = bind;
    s.capture_add = add;
    s.capture_from_actions = s.view == View::Actions;
    captured_key = -1;
    push(View::KeyCapture, "capture.cancel");
}
// The key that opens a line of chat; red when something else already uses it
void chat_key_row(std::vector<Row> &rows) {
    Row r;
    r.id = "chat.key";
    r.title = tr("Chat key");
    keyed(r, ini("ChatKey"));
    SDL_Scancode key = SDL_GetScancodeFromName(s.draft.chat_key);
    r.value = key != SDL_SCANCODE_UNKNOWN ? s.draft.chat_key : tr("None");
    if (const char *use = key != SDL_SCANCODE_UNKNOWN ? key_use((int)key, -1) : nullptr) {
        r.value += std::string(" (") + tr("in use:") + " " + use + ")";
        r.warn = true;
    }
    r.activate = []() { capture(-1, false); };
    rows.push_back(std::move(r));
}
// One keybind as a row (the Keybinds submenu's, and the Keys of the Game actions screen)
Row bind_row(int i, const std::string &id) {
        Row row;
        row.id = id;
        row.title = tr(bind_titles[i]);
        for (int k = 0; k < BIND_KEYS; k++) {
            int code = s.draft.binds[i][k];
            if (!code)
                continue;
            row.value += (row.value.empty() ? "" : ", ") + std::string(config_key_name(code));
            if (const char *use = key_use(code, i)) {
                row.value += std::string(" (") + tr("in use:") + " " + use + ")";
                row.warn = true;
            }
        }
        if (row.value.empty())
            row.value = tr("None");
        if (config_mod_bound(i)) { // (these are what work for now; the row's own count again once the mod gives it back)
            int live[BIND_KEYS];
            char now[128];
            for (int k = 0; k < BIND_KEYS; k++)
                live[k] = config_live_bind(i, k);
            config_format_keys(live, now, sizeof now);
            row.value += std::string(" (") + tr("set by a mod:") + " " + now + ")";
            row.warn = true;
        }
        Row copy = row;
        row.menu = true;
        row.activate = [copy, i]() {
            bool one = s.draft.binds[i][0] && !s.draft.binds[i][1];
            std::vector<std::string> names = {tr("Set key")};
            if (one)
                names.push_back(tr("Add a second key"));
            names.push_back(tr("None"));
            choices(copy, names, -1, [i, one](int v) {
                if (v == (one ? 2 : 1))
                    s.draft.binds[i][0] = s.draft.binds[i][1] = 0;
                else
                    capture(i, v == 1);
            });
        };
    return row;
}
// The Keybinds submenu: a row per keybind (its keys; red where one does something else too), the chat key
void keybind_rows(std::vector<Row> &r) {
    for (int i = 0; i < BINDS; i++) {
        if (i == BIND_MOVE_UP)
            header(r, "bind.movement", tr("Movement"));
        if (i == BIND_A)
            header(r, "bind.buttons", tr("Buttons"));
        if (i == BIND_ITEM_LEFT)
            header(r, "bind.items", tr("Item bar"));
        r.push_back(bind_row(i, "bind." + std::to_string(i)));
    }
    header(r, "bind.other", tr("Chat"));
    chat_key_row(r);
    action(r, "bind.reset", tr("Reset keybinds"), []() {
        Config f = config_defaults();
        memcpy(s.draft.binds, f.binds, sizeof f.binds);
        SDL_strlcpy(s.draft.chat_key, f.chat_key, sizeof s.draft.chat_key);
    });
    action(r, "bind.back", tr("Back"), []() { pop(); });
}
// ---- the Game actions submenu (runtime/actions.h)
// The binding the rows edit for an action: the chosen weapon class's own, or the one for every weapon
ActBinding &action_slot(int id) {
    ActionTab &a = acts();
    ActDevice &d = a.draft.dev[a.dev];
    return a.weapon > 0 ? d.weapon[a.weapon - 1][id] : d.base[id];
}
// (after an edit: a class counts as having bindings of its own while any of its actions is set)
void action_touch() {
    for (int d = 0; d < 2; d++)
        for (int w = 0; w < ACT_WEAPONS; w++) {
            bool any = false;
            for (int i = 0; i < ACT_COUNT; i++)
                any |= acts().draft.dev[d].weapon[w][i].set;
            acts().draft.dev[d].has_weapon[w] = any;
        }
}
std::string binding_text(const ActBinding &b) {
    if (!b.n)
        return tr("None");
    std::string text;
    for (int a = 0; a < b.n; a++) {
        if (a)
            text += " / ";
        for (int k = 0; k < b.alt[a].n; k++) {
            char one[64];
            act_input_label(b.alt[a].code[k], one, sizeof one);
            text += (k ? " + " : "") + std::string(one);
        }
    }
    return text;
}
bool bindings_share(const ActBinding &x, const ActBinding &y) {
    for (int a = 0; a < x.n; a++)
        for (int e = 0; e < y.n; e++)
            if (x.alt[a].n == 1 && y.alt[e].n == 1 && x.alt[a].code[0] == y.alt[e].code[0])
                return true;
    return false;
}
// Choosing an input for an action (add: one more way to press it, else its only one)
void action_capture(int id, bool add) {
    acts().capture = id;
    acts().capture_row = -1;
    acts().capture_add = add;
    acts().capture_start = SDL_GetTicks();
    captured_key = -1;
    push(View::KeyCapture, "capture.cancel");
}
int panel_sources(void *, int btn, int dev, int *out, int max);
const char *scheme_name(int dev);
void row_chosen(int row, int code, uint64_t changed);
bool bind_take(int key);
// Takes an input for the action or merged row being set (acts().capture, capture_row, capture_add). false: refused
// (s.error says why when it is worth saying)
bool action_take(int code) {
    ActionTab &a = acts();
    if (code <= 0 || act_input_device(code) != a.dev)
        return false;
    if (code >= BV_STICK_L_UP && code <= BV_STICK_R_RIGHT) {
        s.error = tr("The sticks' directions can't be bound yet: the game reads each stick as a whole.");
        return false;
    }
    const char *reserved = code < SDL_SCANCODE_COUNT ? key_reserved(code) : nullptr;
    if (reserved) {
        s.error = fmt("That key is the recomp's own (%s).", tr(reserved));
        return false;
    }
    ActBinding row_binding, &b = a.capture_row >= 0 ? row_binding : action_slot(a.capture);
    if (a.capture_row >= 0) {
        ActRowState st = act_row_state(&a.draft, a.dev, a.capture_row, &row_binding);
        if (!a.capture_add || st != ACT_ROW_BOUND)
            memset(&row_binding, 0, sizeof row_binding);
        row_binding.set = true;
    } else if (!a.capture_add || !b.set || !b.n) {
        memset(&b, 0, sizeof b);
        b.set = true;
    }
    bool have = false;
    for (int i = 0; i < b.n; i++)
        have |= b.alt[i].n == 1 && b.alt[i].code[0] == code;
    if (!have && b.n < ACT_ALTS) {
        b.alt[b.n].n = 1;
        b.alt[b.n].code[0] = code;
        b.n++;
    }
    if (a.capture_row >= 0) {  // Simple: the row's actions, then the other rows lose the input
        int row = a.capture_row;
        act_row_set(&a.draft, a.dev, row, &row_binding);
        uint64_t took = act_row_claim(&a.draft, a.dev, row, code, panel_sources, nullptr, input_view_dash_zr());
        row_chosen(row, code, took);
        return true;
    }
    action_touch();
    return true;
}
void action_capture_rows(std::vector<Row> &r) {
    ActionTab &a = acts();
    int code = captured_key.exchange(-1);
    bool pad = a.dev == 0;
    bool done = false;
    if (code == SDL_SCANCODE_ESCAPE || (pad && SDL_GetTicks() - a.capture_start > 8000))
        done = true;
    else if (code > 0 && action_take(code))
        done = true;
    if (done) {
        std::string note = s.error;  // (what the choice did to the other rows: pop() clears it)
        a.capture = -1;
        a.capture_row = -1;
        pop();
        if (code != SDL_SCANCODE_ESCAPE)
            s.error = note;
        return;
    }
    action(r, "capture.cancel", tr("Cancel"), []() {
        acts().capture = -1;
        acts().capture_row = -1;
        pop();
    });
}
int panel_sources(void *, int btn, int dev, int *out, int max) { return host_input_sw_sources(btn, dev, out, max); }
const char *scheme_name(int dev) { return tr(dev == 0 ? "Controller" : "Keyboard"); }
// What an action says of itself beyond its name (state-scoped actions: runtime/actions.c scopes[])
const char *action_note(int id) {
    switch (id) {
    case ACT_CROUCH:
        return "B while the hunter stands still. Give it an input of its own and it is told apart from the roll: with the weapon away it works only standing still and Dodge only while moving. It cannot share an input with Interact.";
    case ACT_DODGE:
        return "With Crouch bound, or Dodge and Interact on one input, Dodge works only while moving when the weapon is away (the roll); weapon B-actions with the weapon out are as before.";
    case ACT_INTERACT:
        return "May share an input with Dodge: standing still it only interacts, moving it rolls. Never share it with Crouch.";
    case ACT_ITEMBAR_RIGHT:
    case ACT_ITEMBAR_LEFT:
        return "While L is held and the item bar is open, A (next) and Y (previous) scroll the bar and do nothing else: only these two count then, whatever the other actions are bound to.";
    default:
        return nullptr;
    }
}
// Two actions of one context on one input by design (not a conflict): Interact with Dodge, Crouch with Dodge (the game's
// own B), AttackA with Interact; the item bar's two actions are a state of their own
bool shares_by_design(int a, int b, bool sheathed_attack) {
    auto pair = [&](int x, int y) { return (a == x && b == y) || (a == y && b == x); };
    if (pair(ACT_INTERACT, ACT_DODGE) || pair(ACT_CROUCH, ACT_DODGE) || (!sheathed_attack && pair(ACT_ATTACK_A, ACT_INTERACT)))
        return true;
    bool bar_a = a == ACT_ITEMBAR_RIGHT || a == ACT_ITEMBAR_LEFT, bar_b = b == ACT_ITEMBAR_RIGHT || b == ACT_ITEMBAR_LEFT;
    return (bar_a || bar_b) && !(bar_a && bar_b);
}
// What a merged row shows (Simple layout)
std::string row_value(int row, bool &warn) {
    ActionTab &a = acts();
    ActBinding shown;
    switch (act_row_state(&a.draft, a.dev, row, &shown)) {
    case ACT_ROW_DEFAULT: return tr("Default");
    case ACT_ROW_UNSET: warn = true; return tr("Not set: choose a button");
    case ACT_ROW_CUSTOM: warn = true; return tr("Custom (differs by context: see Advanced)");
    default: break;
    }
    std::string text = binding_text(shown);
    for (int o = 0; o < act_row_count(); o++)
        if (o != row && act_rows_clash(&a.draft, a.dev, row, o)) {
            text += std::string(" (") + tr("also:") + " " + act_row(o)->name + ")";
            warn = true;
            break;
        }
    return text;
}
// Choosing an input for a merged row
void row_capture(int row, bool add) {
    action_capture(act_row(row)->head, add);
    acts().capture_row = row;
}
// What choosing an input did to the other rows: said once, in red, under the title
void row_chosen(int row, int code, uint64_t changed) {
    (void)row, (void)code;
    if (!changed)
        return;
    std::string names;
    for (int i = 0; i < act_row_count(); i++)
        if (changed >> i & 1)
            names += (names.empty() ? "" : ", ") + std::string(tr(act_row(i)->label));
    s.error = fmt("Taken from: %s. Choose a new button for it.", names.c_str());
}
// What the list and the drawing show: a context at a time (Simple merges the hunt and the town), and the keys that are not
// actions (movement, the item bar, chat) with the keyboard
enum { TAB_HUNT, TAB_TOWN, TAB_MENUS, TAB_KEYS };
struct TabDef {
    int kind;
    const char *label;
};
std::vector<TabDef> tab_defs() {
    std::vector<TabDef> t;
    if (acts().layout == ACT_LAYOUT_SIMPLE) {
        t.push_back({TAB_HUNT, "Hunt and town"});
        t.push_back({TAB_MENUS, "Menus"});
    } else {
        t.push_back({TAB_HUNT, "Hunt"});
        t.push_back({TAB_TOWN, "Town and Hub"});
        t.push_back({TAB_MENUS, "Menus"});
    }
    if (acts().dev == 1)
        t.push_back({TAB_KEYS, "Keys"});
    return t;
}
int tab_kind() {
    std::vector<TabDef> t = tab_defs();
    acts().tab = std::clamp(acts().tab, 0, (int)t.size() - 1);
    return t[acts().tab].kind;
}
int tab_view(int kind) { return kind == TAB_TOWN ? ACT_VIEW_TOWN : kind == TAB_MENUS ? ACT_VIEW_MB2 : ACT_VIEW_MB1; }
void tab_step(int d) {
    int n = (int)tab_defs().size();
    acts().tab = (acts().tab + d + n) % n;
    focus("act.tab");
}
// The keys of the Keys tab: the movement, the item bar and the chat key; Advanced also the keys of the Switch buttons
bool key_in_simple(int i) { return i <= BIND_WALK || i == BIND_ITEM_LEFT || i == BIND_ITEM_RIGHT; }
void key_rows(std::vector<Row> &r) {
    bool simple = acts().layout == ACT_LAYOUT_SIMPLE;
    header(r, "key.h.move", tr("Movement"));
    for (int i = BIND_MOVE_UP; i <= BIND_WALK; i++)
        r.push_back(bind_row(i, "key." + std::to_string(i)));
    r.back().help = tr("Walk slows the left stick's movement while it is held. The keys move the stick, so the game's own Control Type options apply.");
    header(r, "key.h.items", tr("Item bar"));
    for (int i = BIND_ITEM_LEFT; i <= BIND_ITEM_RIGHT; i++)
        r.push_back(bind_row(i, "key." + std::to_string(i)));
    header(r, "key.h.chat", tr("Chat"));
    chat_key_row(r);
    if (!simple) {
        header(r, "key.h.buttons", tr("Buttons"));
        for (int i = BIND_A; i <= BIND_RSTICK; i++)
            r.push_back(bind_row(i, "key." + std::to_string(i)));
        r.back().help = tr("What a key presses on the Switch controller. An action left on Default reads these; an action with a key of its own does not.");
    }
}
void simple_rows(std::vector<Row> &r) {
    ActionTab &a = acts();
    ActProfile &p = a.draft;
    uint64_t unset = 0;
    int n = act_rows_unset(&p, a.dev, &unset);
    if (n) {
        Row row;
        row.id = "act.unset";
        row.title = tr("Actions not set");
        row.value = std::to_string(n);
        row.warn = true;
        row.enabled = false;
        row.help = tr("Actions you have to give a button: something took theirs, or you set them to None.");
        r.push_back(std::move(row));
    }
    int last_group = -1;
    for (int i = 0; i < act_row_count(); i++) {
        const ActRow *ar = act_row(i);
        if (ar->group != (tab_kind() == TAB_MENUS ? 1 : 0))
            continue;
        if (ar->group != last_group) {
            header(r, "act.g" + std::to_string(ar->group), tr(act_row_group_name(ar->group)));
            last_group = ar->group;
        }
        Row row;
        row.id = "act.row." + std::string(ar->name);
        row.title = tr(ar->label);
        row.value = row_value(i, row.warn);
        row.help = tr("Merged with the same button of the town and the menus where it has one. Giving it a button another action of the same kind uses takes that button from it.");
        if (const char *note = action_note(ar->head))
            row.help += std::string(" ") + tr(note);
        Row copy = row;
        row.menu = true;
        row.activate = [copy, i]() {
            ActBinding shown;
            ActRowState st = act_row_state(&acts().draft, acts().dev, i, &shown);
            bool more = st == ACT_ROW_BOUND && shown.n < ACT_ALTS;
            std::vector<std::string> names = {tr("Set")};
            if (more)
                names.push_back(tr("Add another"));
            names.push_back(tr("Not set"));
            names.push_back(tr("Default"));
            choices(copy, names, -1, [i, more](int v) {
                if (v == 0 || (more && v == 1))
                    return row_capture(i, v == 1);
                ActBinding none;
                memset(&none, 0, sizeof none);
                act_row_set(&acts().draft, acts().dev, i, v == (more ? 2 : 1) ? &none : nullptr);
            });
        };
        r.push_back(std::move(row));
    }
    header(r, "act.note", tr("Every action on its own, and bindings for one weapon, are in the Advanced layout."));
}
void advanced_rows(std::vector<Row> &r) {
    ActionTab &a = acts();
    ActProfile &p = a.draft;
    int kind = tab_kind();
    if (kind == TAB_HUNT) {
        std::vector<std::string> weapons = {tr("All weapons")};
        for (int w = 0; w < ACT_WEAPONS; w++)
            weapons.push_back(act_weapon_name(w));
        add_choice(r, "act.weapon", "For", a.weapon, weapons, [](int v) { acts().weapon = v; });
        r.back().help = tr("All weapons, or one weapon class: a binding set for a class is used instead of the general one while a hunter holds that weapon.");
    }
    int var = kind == TAB_HUNT ? a.weapon : 0;
    static const int views[] = {ACT_VIEW_MB1, ACT_VIEW_TOWN, ACT_VIEW_MB2};
    static const char *const titles[] = {"Hunt", "Town and Hub", "Menus"};
    for (int vi = 0; vi < 3; vi++) {
        if (views[vi] != tab_view(kind))
            continue;  // (one context at a time)
        header(r, "act.h" + std::to_string(vi), tr(titles[vi]));
        int ids[ACT_COUNT], n = act_order(views[vi], ids, ACT_COUNT);
        for (int k = 0; k < n; k++) {
            int id = ids[k], from = id;
            Row row;
            row.id = "act." + std::string(act_name(id));
            row.title = tr(act_label(id));
            const ActBinding *b = act_effective(&p, a.dev, id, var, &from);
            const ActBinding &own = action_slot(id);
            if (!b)
                row.value = tr("Default");
            else {
                row.value = binding_text(*b);
                if (from != id)
                    row.value += std::string(" (") + tr("same as") + " " + act_name(from) + ")";
                else if (var > 0 && !own.set)
                    row.value += std::string(" (") + tr("all weapons") + ")";
                for (int m = 0; m < n; m++) {  // another action of this context on the same input
                    int other = ids[m], ofrom = other;
                    const ActBinding *ob = other != id ? act_effective(&p, a.dev, other, var, &ofrom) : nullptr;
                    if (!ob || ofrom == from || !bindings_share(*b, *ob))
                        continue;
                    if (shares_by_design(id, other, p.sheathed_attack))
                        continue;
                    row.value += std::string(" (") + tr("also:") + " " + act_name(other) + ")";
                    row.warn = true;
                    break;
                }
            }
            row.help = fmt("Ini name: %s. Default: the button it has without rebinding. An action with no binding of its own can follow another one (shown as \"same as\").", act_name(id));
            if (const char *note = action_note(id))
                row.help += std::string(" ") + tr(note);
            Row copy = row;
            row.menu = true;
            row.activate = [copy, id]() {
                const ActBinding &own = action_slot(id);
                bool more = own.set && own.n > 0 && own.n < ACT_ALTS;
                std::vector<std::string> names = {tr("Set")};
                if (more)
                    names.push_back(tr("Add another"));
                names.push_back(tr("None"));
                names.push_back(tr("Default"));
                choices(copy, names, -1, [id, more](int v) {
                    int base = more ? 2 : 1;
                    ActBinding &b = action_slot(id);
                    if (v == 0 || (more && v == 1))
                        action_capture(id, v == 1);
                    else {
                        memset(&b, 0, sizeof b);
                        b.set = v == base;  // None: bound to nothing on purpose; Default: no binding of its own
                        action_touch();
                    }
                });
            };
            r.push_back(std::move(row));
        }
    }
}
void action_rows(std::vector<Row> &r) {
    ActionTab &a = acts();
    ActProfile &p = a.draft;
    bool simple = a.layout == ACT_LAYOUT_SIMPLE;
    auto toggle = [&r](const char *id, const char *name, bool *value, const char *help) {
        Row row;
        row.id = id;
        row.title = tr(name);
        row.value = tr(*value ? "On" : "Off");
        row.help = tr(help);
        row.adjust = [value](int d) { *value = d > 0; };
        row.activate = [value]() { *value = !*value; };
        r.push_back(std::move(row));
    };
    toggle("act.enable", "Rebind game actions", &p.enable,
           "On: the game reads the bindings below instead of the controller's buttons and the keybinds. An action left on Default keeps the button it has now. Off: nothing of the game is changed.");
    toggle("act.menus", "Also in the game's menus", &p.menus,
           "On: the Menus actions below apply (confirm, cancel, moving, tabs). Off: the game's menus read the controller and the keybinds as before.");
    add_choice(r, "act.layout", "Layout", simple ? 0 : 1, {tr("Simple"), tr("Advanced")},
               [](int v) {
                   acts().draft.layout = acts().layout = v == 0 ? ACT_LAYOUT_SIMPLE : ACT_LAYOUT_ADVANCED;
                   if (v == 0)
                       acts().weapon = 0;
               });
    r.back().help = tr("Simple: one button for what goes together (Confirm also talks in town, Cancel also backs out there), the same bindings underneath. Advanced: every action on its own, and per weapon.");
    add_choice(r, "act.device", "Control scheme", a.dev, {tr("Controller"), tr("Keyboard")},
               [](int v) {
                   acts().dev = v;
                   acts().draft.scheme = v == 0 ? ACT_SCHEME_PAD : ACT_SCHEME_KBM;
               });
    r.back().help = tr("Which bindings the list shows and edits: the controller's, or the keyboard and mouse's. They are separate: an action can have both.");
    int kind = tab_kind();
    {
        std::vector<std::string> names;
        for (const TabDef &d : tab_defs())
            names.push_back(tr(d.label));
        add_choice(r, "act.tab", "Show", a.tab, names, [](int v) { acts().tab = v; });
        r.back().help = tr("What the list and the drawing show: one part of the game at a time. L1 / R1 on a controller go through them. Keys: the keys that are not actions (movement, the item bar, chat).");
    }
    if (kind == TAB_KEYS)
        key_rows(r);
    else if (!simple) {
        if (kind == TAB_HUNT) {
            toggle("act.sheathed", "Attack A also interacts", &p.sheathed_attack,
                   "Only matters once Interact has a binding of its own. Off: with the weapon away only Interact presses the game's A (gather, carve, talk), with it out only Attack A does. On: Attack A presses it in both cases.");
            toggle("act.split", "Split Dash from weapon R-action", &p.split,
                   "On: Dash and the weapon's R-action (guard, charge, aim) are two actions you can put on two buttons. Binding Weapon R-action turns it on by itself. Off: they are one button, as in the game.");
        }
        advanced_rows(r);
    } else
        simple_rows(r);
    if (a.dev == 0 && kind == TAB_MENUS) {
        header(r, "act.h.menu", tr("Settings menu"));
        add_bool(r, "menu.chord", "Plus + Minus opens this menu", &s.draft.menu_chord, ini("MenuChord"));
        r.back().help = tr("Pressing Plus and Minus together (Start + Back on a controller) opens this menu. F1 does on a keyboard.");
    }
    action(r, "act.reset", tr("Reset game actions"), []() {
        ActProfile &p = acts().draft;
        memset(p.dev, 0, sizeof p.dev);
    });
    r.back().help = tr("Every action of both devices and all weapons back to Default.");
    action(r, "act.back", tr("Back"), []() { pop(); });
}
// ---- the drawing of the controller, or of the keyboard and the mouse, beside the list (runtime/binding_view.h)
// What the focused row (or the one being set) reads is lit on it with an arrow to its label; the row under the pointer is
// ringed; every input down right now glows; an input clicked on it goes to the row being set.
const int BV_KEY_CHAT = 999;  // (the chat key's id among the keys of the Keys tab)
bool g_bv_pointing;           // the pointer is on the drawing: a click there is the drawing's, not a button chosen
std::string g_bv_help;        // the help of the row focused or pointed at, under the drawing
std::string short_label(const std::string &full) {
    std::string t = full;
    size_t cut = t.find_first_of(",:");
    if (cut != std::string::npos)
        t.resize(cut);
    size_t par = t.rfind(" (");
    if (par != std::string::npos && !t.empty() && t.back() == ')')
        t.resize(par);
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    return t;
}
// The physical inputs of a device a binding reads (a sw: token is what Tier 0 gives that Switch button), and those an
// action reads while it has no binding (Tier 0's)
void binding_inputs(const ActBinding &b, int dev, std::vector<int> &out) {
    bool zr = input_view_dash_zr();
    for (int a = 0; a < b.n; a++)
        for (int k = 0; k < b.alt[a].n; k++) {
            int c = b.alt[a].code[k];
            if (c >= ACT_SW && c < ACT_SW + 0x60) {
                int d = (c - ACT_SW) / 0x20, btn = (c - ACT_SW) % 0x20;
                if (btn == ACT_KCR)
                    btn = zr ? 9 : 7;
                else if (btn == ACT_KCZR)
                    btn = zr ? 7 : 9;
                int buf[16];
                if (d == 2 || d == dev) {
                    int n = host_input_sw_sources(btn, dev, buf, 16);
                    out.insert(out.end(), buf, buf + n);
                }
            } else if (act_input_device(c) == dev)
                out.push_back(c);
        }
}
void default_inputs(int id, int dev, std::vector<int> &out) {
    int btn = act_default_button(id, input_view_dash_zr());
    if (btn < 0)
        return;
    int buf[16];
    int n = host_input_sw_sources(btn, dev, buf, 16);
    out.insert(out.end(), buf, buf + n);
}
// What a row of the list is: an action (Advanced), a merged row (Simple), a keybind or the chat key (Keys)
struct FocusTarget {
    int kind;  // 0 none, 1 action, 2 merged row, 3 keybind, 4 chat key
    int key;
};
FocusTarget focus_target(const std::string &id) {
    if (id.rfind("act.row.", 0) == 0) {
        for (int i = 0; i < act_row_count(); i++)
            if (id.compare(8, std::string::npos, act_row(i)->name) == 0)
                return {2, i};
    } else if (id.rfind("act.", 0) == 0) {
        int a = act_find(id.c_str() + 4);
        if (a >= 0)
            return {1, a};
    } else if (id.rfind("key.", 0) == 0 && id.size() > 4 && id[4] >= '0' && id[4] <= '9')
        return {3, atoi(id.c_str() + 4)};
    else if (id == "chat.key")
        return {4, BV_KEY_CHAT};
    return {0, -1};
}
int key_of(const std::string &id, int kind) {
    FocusTarget t = focus_target(id);
    if (kind == TAB_KEYS)
        return t.kind == 3 || t.kind == 4 ? t.key : -1;
    if (acts().layout == ACT_LAYOUT_SIMPLE)
        return t.kind == 2 ? t.key : -1;
    return t.kind == 1 ? t.key : -1;
}
int selected_key(int kind) {
    ActionTab &a = acts();
    if (s.view == View::KeyCapture) {
        if (a.capture >= 0)
            return a.capture_row >= 0 ? a.capture_row : a.capture;
        return s.capture_bind >= 0 ? s.capture_bind : BV_KEY_CHAT;
    }
    return key_of(s.focus, kind);
}
std::string key_label(int key, int kind) {
    if (key < 0)
        return "";
    if (kind == TAB_KEYS)
        return key == BV_KEY_CHAT ? tr("Chat key") : tr(bind_titles[key]);
    if (acts().layout == ACT_LAYOUT_SIMPLE)
        return short_label(tr(act_row(key)->label));
    return short_label(tr(act_label(key)));
}
struct BvEntry {
    int key;
    std::string label;
    int from;
};
struct BvStore {
    std::deque<std::string> text;
    std::vector<BVAction> acts;
    std::vector<BVInput> ins;
    std::map<int, std::vector<int>> inputs_of;  // a row's key -> the inputs it reads
};
// The inputs of the shown part and what is on each, from the draft (so the drawing follows an edit at once)
void bv_collect(BvStore &st, int kind, int sel_key) {
    ActionTab &a = acts();
    const ActProfile &p = a.draft;
    int dev = a.dev;
    bool simple = a.layout == ACT_LAYOUT_SIMPLE;
    std::map<int, std::vector<BvEntry>> by_code;
    auto add = [&](int code, int key, const std::string &label, int from) {
        std::vector<BvEntry> &v = by_code[code];
        for (const BvEntry &e : v)
            if (e.key == key)
                return;
        v.push_back({key, label, from});
        st.inputs_of[key].push_back(code);
    };
    if (kind == TAB_KEYS) {
        for (int i = 0; i < BINDS; i++) {
            if (simple && !key_in_simple(i))
                continue;
            for (int k = 0; k < BIND_KEYS; k++)
                if (s.draft.binds[i][k])
                    add(s.draft.binds[i][k], i, tr(bind_titles[i]), i);
        }
        SDL_Scancode chat = SDL_GetScancodeFromName(s.draft.chat_key);
        if (chat != SDL_SCANCODE_UNKNOWN)
            add((int)chat, BV_KEY_CHAT, tr("Chat key"), BV_KEY_CHAT);
    } else {
        struct Item {
            int key, from;
            std::string label;
            bool by_default;
            std::vector<int> codes;
        };
        std::vector<Item> items;
        if (simple) {
            for (int i = 0; i < act_row_count(); i++) {
                const ActRow *ar = act_row(i);
                if (ar->group != (kind == TAB_MENUS ? 1 : 0))
                    continue;
                ActBinding shown;
                ActRowState st2 = act_row_state(&p, dev, i, &shown);
                Item it{i, i, short_label(tr(ar->label)), st2 == ACT_ROW_DEFAULT, {}};
                if (st2 == ACT_ROW_BOUND)
                    binding_inputs(shown, dev, it.codes);
                else if (st2 == ACT_ROW_DEFAULT)
                    default_inputs(ar->head, dev, it.codes);
                else if (st2 == ACT_ROW_CUSTOM) {
                    int from = ar->head;
                    const ActBinding *b = act_effective(&p, dev, ar->head, 0, &from);
                    if (b)
                        binding_inputs(*b, dev, it.codes);
                    else
                        it.by_default = true, default_inputs(ar->head, dev, it.codes);
                }
                items.push_back(it);
            }
        } else {
            int view = tab_view(kind), var = kind == TAB_HUNT ? a.weapon : 0;
            int ids[ACT_COUNT], n = act_order(view, ids, ACT_COUNT);
            for (int k = 0; k < n; k++) {
                int id = ids[k], from = id;
                const ActBinding *b = act_effective(&p, dev, id, var, &from);
                Item it{id, from, short_label(tr(act_label(id))), b == nullptr, {}};
                if (b)
                    binding_inputs(*b, dev, it.codes);
                else
                    default_inputs(id, dev, it.codes);
                items.push_back(it);
            }
        }
        // an input some action has chosen is no longer what the others read by default
        for (const Item &it : items)
            for (int code : it.codes) {
                bool taken = false;
                if (it.by_default)
                    for (const Item &o : items)
                        if (!o.by_default && o.from != it.from && std::find(o.codes.begin(), o.codes.end(), code) != o.codes.end())
                            taken = true;
                if (!taken)
                    add(code, it.key, it.label, it.from);
            }
    }
    auto conflict = [&](const std::vector<BvEntry> &e) {
        for (size_t i = 0; i < e.size(); i++)
            for (size_t j = i + 1; j < e.size(); j++) {
                if (e[i].from == e[j].from)
                    continue;
                if (kind == TAB_KEYS)
                    return true;
                if (simple ? act_rows_clash(&p, dev, e[i].key, e[j].key) : !shares_by_design(e[i].key, e[j].key, p.sheathed_attack))
                    return true;
            }
        return false;
    };
    size_t total = 0;
    for (auto &kv : by_code)
        total += kv.second.size();
    st.acts.reserve(total);
    st.ins.reserve(by_code.size());
    for (auto &kv : by_code) {
        size_t first = st.acts.size();
        for (const BvEntry &e : kv.second) {
            st.text.push_back(e.label);
            st.acts.push_back({e.key, st.text.back().c_str(), e.key == sel_key});
        }
        st.ins.push_back({kv.first, st.acts.data() + first, (int)kv.second.size(), conflict(kv.second)});
    }
}
BVColors bv_colors(const Theme &look) {
    ImU32 live = s.draft.menu_theme == THEME_BLUE ? IM_COL32(120, 240, 170, 255)
                 : s.draft.menu_theme == THEME_HUNTER ? IM_COL32(110, 220, 255, 255) : IM_COL32(150, 214, 150, 255);
    return bv_colors_from(look.panel | IM_COL32_A_MASK, look.text_dim, look.text, look.text_dim, look.accent,
                          look.accent_bright, look.warn, live);
}
// An input clicked on the drawing: while one is being chosen it is that press; else the focused row takes it
void bv_click(int code) {
    if (s.view == View::KeyCapture) {
        captured_key = code;
        return;
    }
    if (s.view != View::Actions)
        return;
    FocusTarget t = focus_target(s.focus);
    ActionTab &a = acts();
    s.error.clear();
    if (t.kind == 1 || t.kind == 2) {
        a.capture = t.kind == 1 ? t.key : act_row(t.key)->head;
        a.capture_row = t.kind == 2 ? t.key : -1;
        a.capture_add = false;
        action_take(code);
        a.capture = -1;
        a.capture_row = -1;
    } else if (t.kind == 3 || t.kind == 4) {
        s.capture_bind = t.kind == 3 ? t.key : -1;
        s.capture_add = false;
        bind_take(code);
        s.capture_bind = -1;
    } else
        s.error = tr("Select a row of the list first, then click the input for it.");
}
// The tabs over the drawing: what the list and the drawing show (L1 / R1 on a controller)
void draw_bind_tabs(float width) {
    ActionTab &a = acts();
    const Theme &look = theme();
    std::vector<TabDef> defs = tab_defs();
    tab_kind();  // (the current tab is one of them)
    float fs = ImGui::GetFontSize(), gap = fs * .4f, h = fs * 2.f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float icon = fs * 1.5f, x = at.x + icon + gap * 2, room = width - 2 * (icon + gap * 2);
    std::vector<float> pw;
    float total = 0;
    for (const TabDef &d : defs) {
        pw.push_back(ImGui::CalcTextSize(tr(d.label)).x + fs * 1.6f);
        total += pw.back() + gap;
    }
    float shrink = total > room ? room / total : 1.f;
    SettingsIcons::draw(dl, ImVec2(at.x, at.y + (h - fs) * .5f), fs, s.draft.button_icons, SettingsIcons::Left, 0);
    SettingsIcons::draw(dl, ImVec2(at.x + width - icon, at.y + (h - fs) * .5f), fs, s.draft.button_icons, SettingsIcons::Right, 0);
    bool capturing = s.view == View::KeyCapture;
    for (int i = 0; i < (int)defs.size(); i++) {
        float w = pw[i] * shrink;
        ImGui::SetCursorScreenPos(ImVec2(x, at.y));
        ImGui::PushID(i);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        bool clicked = ImGui::InvisibleButton("##tab", ImVec2(w, h));
        ImGui::PopItemFlag();
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        g_bv_pointing |= hovered;
        bool active = a.tab == i;
        dl->AddRectFilled(ImVec2(x, at.y), ImVec2(x + w, at.y + h),
                          active ? look.tab_active : hovered && !capturing ? look.row_hover : look.tab, look.rounding);
        const char *name = tr(defs[i].label);
        ImVec2 sz = ImGui::CalcTextSize(name);
        float size = fs * (sz.x > w - fs * .4f ? (w - fs * .4f) / sz.x : 1.f);
        dl->AddText(ImGui::GetFont(), size, ImVec2(x + (w - sz.x * size / fs) * .5f, at.y + (h - size) * .5f),
                    active ? look.accent_bright : look.text, name);
        if (active)
            dl->AddRectFilled(ImVec2(x + w * .15f, at.y + h - 3), ImVec2(x + w * .85f, at.y + h), look.accent);
        if (clicked && !capturing) {
            a.tab = i;
            focus("act.tab");
        }
        x += w + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h + gap));
    ImGui::Dummy(ImVec2(width, 1));
}
void draw_bind_view(float width, float max_h, bool with_help) {
    ActionTab &a = acts();
    const Theme &look = theme();
    int kind = tab_kind();
    draw_bind_tabs(width);
    int sel_key = selected_key(kind);
    BvStore st;
    bv_collect(st, kind, sel_key);
    std::vector<int> sel, foc;
    auto it = st.inputs_of.find(sel_key);
    if (sel_key >= 0 && it != st.inputs_of.end())
        sel = it->second;
    int hov_key = s.view == View::Actions ? key_of(s.hovered, kind) : -1;
    it = st.inputs_of.find(hov_key);
    if (hov_key >= 0 && hov_key != sel_key && it != st.inputs_of.end())
        foc = it->second;
    int held[64];
    int n_held = host_input_held(held, 64);
    std::string hint = sel_key >= 0 ? fmt("Click to give it to: %s", key_label(sel_key, kind).c_str()) : "";
    BVParams p;
    p.scheme = a.dev == 0 ? BV_CONTROLLER : BV_KEYBOARD;
    p.inputs = st.ins.data(), p.n_inputs = (int)st.ins.size();
    p.selected_list = sel.data(), p.n_selected = (int)sel.size();
    p.focus_list = foc.data(), p.n_focus = (int)foc.size();
    p.capturing = s.view == View::KeyCapture;
    p.held = held, p.n_held = n_held;
    p.time_s = (float)ImGui::GetTime();
    p.click_hint = hint.empty() ? nullptr : hint.c_str();
    p.colors = bv_colors(look);
    p.font_px = ImGui::GetFontSize();
    p.max_height = max_h;
    float w = bv_fit_width(p, width), h = bv_height(p, w);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - w) * .5f);
    BVResult r = bv_draw("##bindview", w, h, p);
    g_bv_pointing |= ImGui::IsItemHovered();
    if (r.clicked)
        bv_click(r.clicked);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(look.text_dim));
    ImGui::TextWrapped("%s", tr("Inputs you press light up. Point at one to see what is on it."));
    ImGui::PopStyleColor();
    if (with_help && !g_bv_help.empty()) {  // what the row is, under the drawing
        ImVec2 at = ImGui::GetCursorScreenPos();
        float gap = ImGui::GetStyle().ItemSpacing.y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + gap * .5f), ImVec2(at.x + width, at.y + gap * .5f), look.divider);
        ImGui::Dummy(ImVec2(1, gap));
        float fs2 = ImGui::GetFontSize();
        float text_h = ImGui::CalcTextSize(g_bv_help.c_str(), nullptr, false, width).y;
        ImGui::BeginChild("##bvhelp", ImVec2(width, std::min(text_h + 2, fs2 * 7.5f)), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
        ImGui::TextWrapped("%s", g_bv_help.c_str());
        ImGui::EndChild();
    }
}
void reset_section() {
    Config f = config_defaults(), &c = s.draft;
    switch (s.page) {
    case 0:
        c.display_mode = f.display_mode;
        c.width = f.width;
        c.height = f.height;
        c.render_w = f.render_w;
        c.render_h = f.render_h;
        c.aspect_ratio = f.aspect_ratio;
        c.fov = f.fov;
        c.ui_anchor = f.ui_anchor;
        break;
    case 1:
        c.fsr = f.fsr;
        c.fsr_quality = f.fsr_quality;
        c.fsr_sharpness = f.fsr_sharpness;
        c.fsr_objmotion = f.fsr_objmotion;
        c.fsr_reactive = f.fsr_reactive;
        c.fsr_animsurf = f.fsr_animsurf;
        c.show_fps = f.show_fps;
        c.show_battery = f.show_battery;
        c.fps = f.fps;
        c.vsync = f.vsync;
        c.shader_prewarm = f.shader_prewarm;
        c.draw_distance = f.draw_distance;
        c.smooth_gradients = f.smooth_gradients;
        c.fix_kerning = f.fix_kerning;
        c.bold_text = f.bold_text;
        c.hdr = f.hdr;
        c.hdr_peak = f.hdr_peak;
        c.hdr_game = f.hdr_game;
        c.hdr_ui = f.hdr_ui;
        c.hdr_tonemap = f.hdr_tonemap;
        c.hdr_gamma = f.hdr_gamma;
        c.hdr_exposure = f.hdr_exposure;
        c.hdr_highlights = f.hdr_highlights;
        c.hdr_shadows = f.hdr_shadows;
        c.hdr_contrast = f.hdr_contrast;
        c.hdr_saturation = f.hdr_saturation;
        c.hdr_highlight_saturation = f.hdr_highlight_saturation;
        c.hdr_flare = f.hdr_flare;
        c.hdr_bloom = f.hdr_bloom;
        c.hdr_scene = f.hdr_scene;
        c.hdr_format = f.hdr_format;
        c.hdr_analysis = f.hdr_analysis;
        c.msaa = f.msaa;
        c.fxaa = f.fxaa;
        c.fxaa_mode = f.fxaa_mode;
        c.volume = f.volume;
        c.mute_in_background = f.mute_in_background;
        break;
    case 2:
        c.face_buttons = f.face_buttons;
        c.background_input = f.background_input;
        c.menu_chord = f.menu_chord;
        c.combo_window = f.combo_window;
        c.mouse_sensitivity = f.mouse_sensitivity;
        SDL_strlcpy(c.chat_key, f.chat_key, sizeof c.chat_key);
        memcpy(c.binds, f.binds, sizeof c.binds);
        break;
    case 3:
        c.ldn_port = f.ldn_port;
        SDL_strlcpy(c.ldn_join, f.ldn_join, sizeof c.ldn_join);
        SDL_strlcpy(c.ldn_server, f.ldn_server, sizeof c.ldn_server);
        SDL_strlcpy(c.ldn_key, f.ldn_key, sizeof c.ldn_key);
        c.ldn_direct = f.ldn_direct;
        break;
    case 4:
        c.sync_small_monsters = f.sync_small_monsters;
        c.village_in_hub = f.village_in_hub;
        c.share_urgent = f.share_urgent;
        c.hp = f.hp;
        quest_keys();
        break;
    case MODS_PAGE:
        for (auto &m : mods.draft)
            m.on = m.name[0] != '_';
        std::sort(mods.draft.begin(), mods.draft.end(),
                  [](const ModEntry &a, const ModEntry &b) { return strcasecmp(a.name, b.name) < 0; });
        break;
    case CHEATS_PAGE:
        for (int i = 0; i < (int)cheat_tab.draft.size(); ++i)
            cheat_tab.draft[i] = 1;
        break;
#ifdef __ANDROID__
    case ANDROID_PAGE:
        SDL_strlcpy(c.save_dir, f.save_dir, sizeof c.save_dir);
        SDL_strlcpy(c.game_dir, f.game_dir, sizeof c.game_dir);
        break;
#endif
    case APP_PAGE:
        c.language = f.language;
        c.menu_theme = f.menu_theme;
        c.menu_scale = f.menu_scale;
        c.menu_confirm = f.menu_confirm;
        SDL_strlcpy(c.save_location, f.save_location, sizeof c.save_location);
        c.button_icons = f.button_icons;
        c.confirm_exit = f.confirm_exit;
        c.updates = f.updates;
        c.bonus_content = f.bonus_content;
        break;
    }
    pop();
}
// Suggested order: the mods that replace each other's files keep their places among the mods that are on, with the
// ones that change the most files first and the small tweaks last, so a tweak wins over an overhaul.
std::vector<ModEntry> suggested_mods() {
    std::vector<ModEntry> order = mods.draft;
    auto result = mods.scan;
    if (!result || !result->done)
        return order;
    std::vector<int> slots;
    std::vector<std::string> involved;
    for (int i = 0; i < (int)order.size(); ++i) {
        if (!order[i].on)
            continue;
        for (int k = 0; k < result->scan.nconflicts; ++k) {
            const ModConflict &c = result->scan.conflicts[k];
            if (!c.replaced)
                continue;
            const std::string &a = result->names[c.a], &b = result->names[c.b];
            int ia = mod_index(a), ib = mod_index(b);
            if (ia >= 0 && ib >= 0 && mods.draft[ia].on && mods.draft[ib].on && (order[i].name == a || order[i].name == b)) {
                slots.push_back(i);
                involved.push_back(order[i].name);
                break;
            }
        }
    }
    auto files = [&](const std::string &name) {
        auto at = std::find(result->names.begin(), result->names.end(), name);
        return at == result->names.end() ? 0 : result->scan.files[at - result->names.begin()];
    };
    std::stable_sort(involved.begin(), involved.end(),
                     [&](const std::string &a, const std::string &b) { return files(a) > files(b); });
    for (size_t k = 0; k < slots.size(); ++k)
        snprintf(order[slots[k]].name, sizeof order[slots[k]].name, "%s", involved[k].c_str());
    return order;
}
// One row per pair of mods that are on and change the same file or archive entry; selecting it picks the one used
void conflict_rows(std::vector<Row> &r) {
    auto result = mods.scan;
    if (!result || !result->done)
        return;
    for (int k = 0; k < result->scan.nconflicts; ++k) {
        const ModConflict &c = result->scan.conflicts[k];
        if (!c.replaced)  // (archives whose changes are combined are not a problem)
            continue;
        int ia = mod_index(result->names[c.a]), ib = mod_index(result->names[c.b]);
        if (ia < 0 || ib < 0 || !mods.draft[ia].on || !mods.draft[ib].on)
            continue;
        if (ia > ib)
            std::swap(ia, ib);
        const char *first = mods.draft[ia].name, *second = mods.draft[ib].name;
        std::string text = fmt("%s and %s both change %d file(s); %s is used", first, second, c.replaced, second);
        std::string a = first, b = second;
        action(r, "mods.conflict." + std::to_string(k), text, []() {});
        {  // select it to choose which of the two is used: that one moves below the other
            Row &row = r.back();
            Row copy = row;
            copy.title = tr("Which mod is used?");
            row.menu = true;
            row.activate = [copy, a, b]() {
                choices(copy, {fmt("Use %s", a.c_str()), fmt("Use %s", b.c_str())}, 1, [a, b](int v) {
                    int ia = mod_index(a), ib = mod_index(b), win = v ? ib : ia, lose = v ? ia : ib;
                    if (ia < 0 || ib < 0 || win > lose)
                        return;
                    ModEntry moved = mods.draft[win];
                    mods.draft.erase(mods.draft.begin() + win);
                    mods.draft.insert(mods.draft.begin() + lose, moved);  // (the loser is now one place earlier)
                    s.saved_notice = false;
                });
            };
        }
    }
}
// "Add a cheat": a text box to paste one into (Ctrl+V); Accept checks it, adds it to cheats/added.txt and turns it on
void add_cheat() {
    paste_box(tr("Copy a cheat (its [Name] line and its codes), then Paste and Add."), 65536,
             [](const std::string &text, std::string &error) {
                 int before = cheats_count();
                 char err[256];
                 if (!cheats_add(text.c_str(), err, sizeof err)) {
                     error = tr(err);
                     return false;
                 }
                 for (int i = before; i < cheats_count(); ++i) {  // (on, and running from the next frame)
                     cheat_tab.draft.push_back(1);
                     cheat_tab.saved.push_back(1);
                 }
                 return true;
             });
}
void cheat_rows(std::vector<Row> &r) {
    action(r, "cheats.add", tr("Add a cheat"), []() { add_cheat(); });
    r.back().help = tr("Paste a cheat code (Atmosphere format, as for Ryujinx). It is saved in cheats/added.txt and turns on right away.");
    header(r, "cheats.header", tr("Cheats"));
    if (cheat_tab.draft.empty())
        action(r, "cheats.none", tr("No cheats found in the cheats folder"), []() {});
    for (int i = 0; i < (int)cheat_tab.draft.size(); ++i) {
        Row row;
        row.id = "cheat." + std::to_string(i);
        row.title = cheats_name(i);
        row.help = cheats_file(i);
        if (cheats_master(i)) {
            row.value = tr("Always on");
            row.enabled = false;
        } else {
            row.value = tr(cheat_tab.draft[i] ? "On" : "Off");
            row.adjust = [i](int d) {
                cheat_tab.draft[i] = d > 0;
                s.saved_notice = false;
            };
            row.activate = [i]() {
                cheat_tab.draft[i] = !cheat_tab.draft[i];
                s.saved_notice = false;
            };
        }
        r.push_back(std::move(row));
    }
}
void mods_rows(std::vector<Row> &r) {
    if (!mods.scan)
        start_scan();
    auto result = mods.scan;  // (the scan thread fills it; `done` says when)
    if (mods.draft.empty())
        action(r, "mods.none", tr("No mods found in the mods folder"), []() {});
    for (int i = 0; i < (int)mods.draft.size(); ++i) {
        std::string name = mods.draft[i].name;
        bool on = mods.draft[i].on, clash = false;
        if (on && result->done)
            for (int k = 0; k < result->scan.nconflicts && !clash; ++k) {
                const ModConflict &c = result->scan.conflicts[k];
                if (!c.replaced || (result->names[c.a] != name && result->names[c.b] != name))
                    continue;
                int other = mod_index(result->names[result->names[c.a] == name ? c.b : c.a]);
                clash = other >= 0 && mods.draft[other].on;
            }
        Row row;
        row.id = "mod." + name;
        row.title = name;
        row.value = std::string(tr(on ? "On" : "Off")) + (clash ? " !" : "");
        row.cold = true;
        auto set_on = [name](bool v) {
            int at = mod_index(name);
            if (at >= 0 && mods.draft[at].on != v) {
                mods.draft[at].on = v;
                s.saved_notice = false;
            }
        };
        row.adjust = [set_on](int d) { set_on(d > 0); };
        Row copy = row;
        row.menu = true;
        row.activate = [copy, name, on, set_on]() {
            auto move = [name](int d) {
                int at = mod_index(name), to = at + d;
                if (at >= 0 && to >= 0 && to < (int)mods.draft.size()) {
                    std::swap(mods.draft[at], mods.draft[to]);
                    s.saved_notice = false;
                }
            };
            choices(copy, {tr(on ? "Turn off" : "Turn on"), tr("Move up"), tr("Move down")}, -1,
                    [set_on, move, on](int v) {
                        if (v == 0)
                            set_on(!on);
                        else
                            move(v == 1 ? -1 : 1);
                    });
        };
        r.push_back(std::move(row));
    }
    header(r, "mods.header", tr("Load order"));  // (the rows below are not mods)
    if (!result->done) {
        action(r, "mods.scanning", tr("Checking for conflicts..."), []() {});
    } else {
        std::vector<Row> found;
        conflict_rows(found);
        if (found.empty())
            action(r, "mods.clear", tr("No conflicts found"), []() {});
        else {
            action(r, "mods.conflicts", fmt("Mods change the same files (%d): select to review", (int)found.size()),
                   []() { push(View::Conflicts, ""); });
            r.back().value = "!";
        }
    }
    auto order = suggested_mods();
    action(
        r, "mods.suggest", tr("Apply suggested order"),
        [order]() {
            mods.draft = order;
            s.saved_notice = false;
        },
        !same_mods(order, mods.draft));
    action(r, "mods.rescan", tr("Rescan mods"), []() {
        std::vector<ModEntry> now(MODS_MAX);
        int n = mods_installed(now.data(), MODS_MAX);
        // new folders join the end (on), gone ones leave; the order and the switches of the rest stay
        std::vector<ModEntry> next;
        for (const auto &m : mods.draft)
            for (int i = 0; i < n; ++i)
                if (!strcmp(now[i].name, m.name)) {
                    next.push_back(m);
                    break;
                }
        mods.draft = next;
        for (int i = 0; i < n; ++i)
            if (mod_index(now[i].name) < 0) {
                now[i].on = true;  // (as at start: a mod added later loads)
                mods.draft.push_back(now[i]);
            }
        mods.scan.reset();
    });
    // Forge's menu (its plugins' settings) opens with a key (Insert): with a controller alone (an Android TV box) this
    // row is the way in. It opens over this menu, which waits under it; closing it comes back here (runtime/overlay.cpp).
    const char *names[1];
    bool plugins = forge_plugin_titles(names, 1) > 0 || forge_plugins_have(FORGE_IMGUI_RENDER);
    header(r, "mods.forge.header", tr("Forge plugins"));
    action(r, "mods.forge", tr("Open the Forge menu"), []() { forge_requested = true; }, plugins);
    r.back().help = tr("The Forge plugins' own settings, over this menu. Cancel (the button that goes back in the game's menus) or Start + Back closes it and comes back here. On a keyboard the Forge menu key (Insert) opens and closes it.");
}
std::vector<Row> main_rows() {
    std::vector<Row> r;
    Config &c = s.draft;
    switch (s.page) {
    case 0:
        add_choice(
            r, "display.mode", "Window mode", c.display_mode,
            {tr("Windowed"), tr("Fullscreen"), tr("Borderless")}, [](int v) { c.display_mode = v; }, ini("Mode"));
        add_choice(
            r, "display.auto", "Screen resolution", !c.width ? 0 : 1, {tr("Desktop"), tr("Custom")},
            [](int v) {
                s.draft.width = v ? 1920 : 0;
                s.draft.height = v ? 1080 : 0;
            },
            ini("Resolution"));
        if (c.width) {
            int sw, sh;
            screen_max(sw, sh);
            number(r, "display.width", "Width", &c.width, 320, sw, 80, ini("Resolution"));
            number(r, "display.height", "Height", &c.height, 180, sh, 45, ini("Resolution"));
        }
        add_choice(
            r, "render.auto", "Render resolution", !c.render_w ? 0 : 1, {tr("Auto"), tr("Custom")},
            [](int v) {
                s.draft.render_w = v ? 1920 : 0;
                s.draft.render_h = v ? 1080 : 0;
            },
            ini("RenderResolution"));
        if (c.render_w) {
            number(r, "render.width", "Width", &c.render_w, 320, 16384, 80, ini("RenderResolution"));
            number(r, "render.height", "Height", &c.render_h, 180, 16384, 45, ini("RenderResolution"));
        }
        add_choice(
            r, "aspect.auto", "Automatic aspect ratio", c.aspect_ratio == 0 ? 0 : 1,
            {tr("Auto"), tr("Custom")}, [](int v) { s.draft.aspect_ratio = v ? 16.f / 9 : 0; }, ini("AspectRatio"));
        if (c.aspect_ratio)
            number(r, "aspect", tr("Aspect ratio"), &c.aspect_ratio, lo("AspectRatio"), hi("AspectRatio"), .05f, ini("AspectRatio"));
        number(r, "fov", tr("Field of view"), &c.fov, lo("FOV"), hi("FOV"), .05f, ini("FOV"));
        add_choice(
            r, "anchor", "HUD anchor", c.ui_anchor - 1, {"16:9", "21:9", "32:9", tr("Edges")},
            [](int v) { c.ui_anchor = v + 1; }, ini("UIAnchor"));
        break;
    case 1:
        header(r, "audio.header", tr("Audio"));
        number(r, "volume", "Volume (%)", &c.volume, (int)lo("Volume"), (int)hi("Volume"), 1, ini("Volume"));
        add_bool(r, "mutebg", "Mute in background", &c.mute_in_background, ini("MuteInBackground"));
        header(r, "graphics.header", tr("Graphics"));
        add_bool(r, "show.fps", "FPS counter", &c.show_fps, ini("ShowFPS"));
#ifdef __ANDROID__
        add_choice(r, "show.battery", "Battery indicator", c.show_battery, {tr("Off"), tr("With FPS counter"), tr("By itself")},
                   [](int v) { c.show_battery = v; }, ini("ShowBattery"));
        r.back().help = tr("The battery's charge, in the FPS counter's box or in a box of its own (drag it like the FPS counter).");
#endif
        number(r, "fps", "Frame rate limit", &c.fps, (int)lo("FPS"), (int)hi("FPS"), 1, ini("FPS"));
        {  // left / right: one frame per second more or less; Confirm: the common ones
            static const int presets[] = {30, 60, 90, 120, 150, 240};
            r.back().menu = true;
            Row copy = r.back();
            r.back().activate = [copy]() {
                std::vector<std::string> names;
                int now = -1;
                for (int i = 0; i < (int)std::size(presets); i++) {
                    names.push_back(std::to_string(presets[i]));
                    if (presets[i] <= s.draft.fps)
                        now = presets[i] == s.draft.fps ? i : -1 - i;  // (not one of them: start at the one below)
                }
                choices(copy, names, now, [](int v) { s.draft.fps = presets[v]; });
                if (now < -1)
                    focus("choice." + std::to_string(-1 - now));
            };
        }
        add_bool(r, "vsync", "VSync", &c.vsync, ini("VSync"));
        add_bool(r, "prewarm", "Shader prewarm", &c.shader_prewarm, ini("ShaderPrewarm"));
        add_choice(r, "msaa", "MSAA",
                   c.msaa == 8   ? 3
                   : c.msaa == 4 ? 2
                   : c.msaa == 2 ? 1
                                 : 0,
                   {tr("Off"), "2x", "4x", "8x"}, [](int v) { c.msaa = 1 << v; }, ini("MSAA"));
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        add_bool(r, "fxaa", "FXAA", &c.fxaa, ini("FXAA"));
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        add_choice(r, "fxaamode", "FXAA mode", c.fxaa_mode, {tr("Original"), "3.11"}, [](int v) { c.fxaa_mode = v; },
                   ini("FXAAMode"));
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        number(r, "drawdist", tr("Draw distance"), &c.draw_distance, lo("DrawDistance"), hi("DrawDistance"), .25f,
               ini("DrawDistance"));
        if (c.draw_distance == 0)
            r.back().value = tr("No limit");
        add_bool(r, "smooth", "Smooth gradients", &c.smooth_gradients, ini("SmoothGradients"));
        add_bool(r, "kerning", "Fix kerning", &c.fix_kerning, ini("FixKerning"));
        add_bool(r, "boldtext", "Bold text", &c.bold_text, ini("BoldText"));
        add_bool(r, "fsr", "FSR", &c.fsr, ini("FSR"));
        if (c.hdr) {  // (FSR's input stays 8-bit: with HDR it is off, config.c)
            r.back().enabled = false;
            r.back().value = tr("Off (HDR)");
        }
        if (c.fsr && !c.hdr) {
            add_choice(
                r, "fsr.quality", "FSR quality", c.fsr_quality,
                {tr("Native AA"), tr("Quality"), tr("Balanced"), tr("Performance"), tr("Ultra performance")},
                [](int v) { s.draft.fsr_quality = v; }, ini("FSRQuality"));
            number(r, "fsr.sharpness", tr("Sharpness"), &c.fsr_sharpness, lo("Sharpness"), hi("Sharpness"), .05f, ini("Sharpness"));
            add_bool(r, "fsr.motion", "Object motion", &c.fsr_objmotion, ini("ObjectMotion"));
            number(r, "fsr.reactive", tr("Reactive mask"), &c.fsr_reactive, lo("Reactive"), hi("Reactive"), .05f, ini("Reactive"));
            add_bool(r, "fsr.surfaces", "Animated surfaces", &c.fsr_animsurf, ini("AnimSurf"));
        }
        header(r, "hdr.header", "HDR");
        add_bool(r, "hdr", "HDR", &c.hdr, ini("HDR"));
        if (g_config.fsr)  // (FSR running: it is turned off, for HDR, at the next start)
            r.back().cold = true;
        if (c.hdr) {
            number(r, "hdr.peak", tr("Peak brightness (nits)"), &c.hdr_peak, lo("PeakBrightness"), hi("PeakBrightness"), 50.f,
                   ini("PeakBrightness"));
            number(r, "hdr.game", tr("Game brightness (nits)"), &c.hdr_game, lo("GameBrightness"), hi("GameBrightness"), 5.f,
                   ini("GameBrightness"));
            number(r, "hdr.ui", tr("UI brightness (nits)"), &c.hdr_ui, lo("UIBrightness"), hi("UIBrightness"), 5.f,
                   ini("UIBrightness"));
            add_choice(r, "hdr.gamma", "Gamma correction", c.hdr_gamma, {tr("Off"), "2.2", "BT.1886"},
                       [](int v) { s.draft.hdr_gamma = v; }, ini("GammaCorrection"));
            number(r, "hdr.exposure", tr("Exposure"), &c.hdr_exposure, lo("Exposure"), hi("Exposure"), .05f, ini("Exposure"));
            number(r, "hdr.highlights", tr("Highlights"), &c.hdr_highlights, lo("Highlights"), hi("Highlights"), 1.f,
                   ini("Highlights"));
            number(r, "hdr.shadows", tr("Shadows"), &c.hdr_shadows, lo("Shadows"), hi("Shadows"), 1.f, ini("Shadows"));
            number(r, "hdr.contrast", tr("Contrast"), &c.hdr_contrast, lo("Contrast"), hi("Contrast"), 1.f, ini("Contrast"));
            number(r, "hdr.saturation", tr("Saturation"), &c.hdr_saturation, lo("Saturation"), hi("Saturation"), 1.f,
                   ini("Saturation"));
            number(r, "hdr.hlsat", tr("Highlight saturation"), &c.hdr_highlight_saturation, lo("HighlightSaturation"),
                   hi("HighlightSaturation"), 1.f, ini("HighlightSaturation"));
            add_bool(r, "hdr.analysis", "HDR analysis", &c.hdr_analysis, ini("Analysis"));
        }
        break;
    case 2:
        add_choice(r, "face", "Face buttons", c.face_buttons, {tr("Position"), tr("Label")},
                   [](int v) { c.face_buttons = v; }, ini("FaceButtons"));
        add_bool(r, "background", "Background input", &c.background_input, ini("BackgroundInput"));
        add_choice(r, "combo", "Combo input window", c.combo_window, {tr("Lenient"), tr("Strict")},
                   [](int v) { c.combo_window = v; }, ini("ComboInputWindow"));
        number(r, "mouse", tr("Mouse sensitivity"), &c.mouse_sensitivity, lo("MouseSensitivity"), hi("MouseSensitivity"), .05f, ini("MouseSensitivity"));
        action(r, "keybinds", tr("Keybinds"), []() { push(View::Keybinds, "bind.0"); });
        r.back().help = tr("The keys and mouse buttons for the Switch controller's buttons, for moving and for the item bar, and the chat key.");
        action(r, "actions", tr("Game actions"), []() { push(View::Actions, "act.enable"); });
        r.back().help = tr("Rebind the game itself: every action of a hunt, of the town and of the menus on the controller button, key or mouse button you choose, also per weapon, with the controller or the keyboard drawn beside the list. The keys for moving, the item bar and chat are there too (Keys). Off by default.");
        add_bool(r, "menu.chord", "Plus + Minus opens this menu", &c.menu_chord, ini("MenuChord"));
        r.back().help = tr("Pressing Plus and Minus together (Start + Back on a controller) opens this menu. Turn it off to keep that combination for the game or a cheat, for example when you play with a keyboard and mouse. F1 still opens the menu.");
        break;
    case 3:
        number(r, "port", "Host port (TCP / UDP)", &c.ldn_port, (int)lo("Port"), (int)hi("Port"), 1, ini("Port"));
        text(r, "hosts", "Search hosts", c.ldn_join, sizeof c.ldn_join, *c.ldn_join ? c.ldn_join : tr("LAN"), ini("Join"));
        text(r, "server", "Hub server", c.ldn_server, sizeof c.ldn_server,
             !*c.ldn_server ? std::string(tr("Default")) + (*LDN_DEFAULT_SERVER ? " (" LDN_DEFAULT_SERVER ")" : "")
                            : c.ldn_server,
             ini("Server"));
#ifdef __ANDROID__
        text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public (Android players only)"), ini("Key"));
#else
        text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public"), ini("Key"));
#endif
        hub_check(r);
        add_bool(r, "direct", "Direct connections", &c.ldn_direct, ini("Direct"));
        r.back().help = tr("In an internet hub, your game and the host's send their packets straight to each other when both routers let them, for a lower ping; else through the hub server. The other player's game then sees your IP address. Off: always through the hub server.");
        header(r, "discord.header", "Discord");
        add_bool(r, "discord", "Discord Rich Presence", &c.discord, ini("RichPresence"));
        add_bool(r, "discord.hunter", "Show hunter name and HR", &c.discord_hunter, ini("ShowHunter"));
        break;
    case 4: {
        const char *per_quest =
            tr("HP multipliers for single quests, by their quest ID: in those quests they are used instead of the ones above.");
        header(r, "hub.header", tr("Hub"));
        add_bool(r, "sync", "Sync small monsters", &c.sync_small_monsters, ini("SyncSmallMonsters"));
        add_bool(r, "village", "Village quests in Hub", &c.village_in_hub, ini("VillageQuestsInHub"));
        add_bool(r, "urgent", "Urgent quests count for all", &c.share_urgent, ini("ShareUrgentCredit"));
        header(r, "hp.header", tr("Monster HP Scaling"));
        for (int i = 0; i < 4; i++)
            number(r, "hp." + std::to_string(i), std::to_string(i + 1) + " " + tr(i == 0 ? "Hunter" : "Hunters"), &c.hp.scale[i],
                   lo("1Player"), hi("1Player"), .05f, ini("1Player"));
        number(r, "hp.village", tr("Village quests in Hub"), &c.hp.village, lo("HubHP"), hi("HubHP"), .05f,
               ini("HubHP"));
        action(r, "hp.advanced", tr("Per-quest overrides"), []() { s.advanced = !s.advanced; });
        r.back().help = per_quest;
        if (s.advanced) {
            for (int i = 0; i < c.hp.quests; i++) {
                auto id = quest_id(i);
                add_number(
                    r, id, tr("Quest ID"), [i]() { return (double)s.draft.hp.quest[i].quest; },
                    [i](double v) { s.draft.hp.quest[i].quest = (uint32_t)v; }, 1, 4294967295., 1, true, ini("1Player"),
                    [i](double v, std::string &e) {
                        for (int j = 0; j < s.draft.hp.quests; j++)
                            if (j != i && s.draft.hp.quest[j].quest == (uint32_t)v) {
                                e = tr("Duplicate quest ID.");
                                return false;
                            }
                        return true;
                    });
                for (int n = 0; n < 4; n++)
                    number(r, id + "." + std::to_string(n), std::to_string(n + 1) + " " + tr(n == 0 ? "Hunter" : "Hunters"),
                           &c.hp.quest[i].scale[n], HP_SCALE_MIN, HP_SCALE_MAX, .05f, ini("1Player"));
                action(r, id + ".remove", tr("Remove quest"), [i]() {
                    for (int j = i; j < s.draft.hp.quests - 1; j++) {
                        s.draft.hp.quest[j] = s.draft.hp.quest[j + 1];
                        s.quest_keys[j] = s.quest_keys[j + 1];
                    }
                    --s.draft.hp.quests;
                    focus(i < s.draft.hp.quests ? quest_id(i) : "hp.add");
                });
            }
            action(
                r, "hp.add", tr("Add quest"),
                []() {
                    auto &hp = s.draft.hp;
                    uint32_t q = 1;
                    bool exists;
                    do {
                        exists = false;
                        for (int i = 0; i < hp.quests; i++)
                            if (hp.quest[i].quest == q) {
                                exists = true;
                                ++q;
                                break;
                            }
                    } while (exists);
                    int i = hp.quests++;
                    hp.quest[i].quest = q;
                    for (float &v : hp.quest[i].scale)
                        v = 1;
                    s.quest_keys[i] = s.next_quest_key++;
                    focus(quest_id(i));
                },
                c.hp.quests < HP_QUESTS_MAX);
        }
        for (auto &row : r)
            if (!row.id.compare(0, 6, "quest.") || row.id == "hp.add")
                row.help = per_quest;
        break;
    }
    case MODS_PAGE:
        mods_rows(r);
        break;
    case CHEATS_PAGE:
        cheat_rows(r);
        break;
    case APP_PAGE:
        header(r, "look.header", tr("Appearance"));
        add_choice(r, "language", "Language", c.language, {"English", "Español"},
                   [](int v) { c.language = v; }, ini("Language"));
        add_choice(r, "theme", "Menu theme", c.menu_theme, {tr("Gold"), tr("Blue"), tr("Hunter")},
                   [](int v) { c.menu_theme = v; }, ini("Theme"));
        number(r, "scale", "Interface scale (%)", &c.menu_scale, (int)lo("MenuScale"), (int)hi("MenuScale"), 10, ini("MenuScale"));
        action(r, "detach", tr(s.floating ? "Attach menu" : "Detach menu"),
               []() {
                   s.floating = !s.floating;
                   if (!s.floating)
                       s.custom_size = ImVec2(0, 0);
               });
        r.back().help =
            tr(s.floating
                   ? "Puts the menu back at the top of the screen, at its usual size."
                   : "Lets the menu go anywhere on the screen: use Move menu, or drag its tabs with the mouse. Its bottom right corner sizes it.");
        if (s.floating)
            action(r, "move", tr("Move menu"), []() {
                auto *w = ImGui::GetCurrentWindow();
                s.move_start = s.move_position = w->Pos;
                push(View::Move, "move.accept");
            });
        header(r, "menu.controls.header", tr("Menu controls"));
        add_choice(r, "menu.confirm", "Confirm button (recomp)", c.menu_confirm,
                   {tr("Original"), tr("Bottom button"), tr("Right button")},
                   [](int v) { s.draft.menu_confirm = v; }, ini("MenuConfirm"));
        add_choice(r, "icons", "Button icons", c.button_icons, {"Xbox", "Switch", "PlayStation"},
                   [](int v) { c.button_icons = v; }, ini("ButtonIcons"));
        header(r, "general.header", tr("General"));
        add_bool(r, "confirm.exit", "Confirm exit", &c.confirm_exit, ini("ConfirmExit"));
        add_choice(r, "updates", "Updates", c.updates, {tr("Ask"), tr("Auto"), tr("Off")},
                   [](int v) { c.updates = v; }, ini("Updates"));
        add_bool(r, "bonus", "Bonus content", &c.bonus_content, ini("BonusContent"));
#ifndef __ANDROID__  // (Android: its own tab's Save folder)
        header(r, "save.header", tr("Save data"));
        text(r, "save.location", "Save folder", c.save_location, sizeof c.save_location,
             *c.save_location ? c.save_location : tr("Default folder"), ini("SaveLocation"));
        r.back().help = tr("Where the save is kept (the folder that holds save_0), for example on another drive or in a folder you sync. Type a path, or use Choose folder. Press Save, then restart the game. Your current save is not moved: copy the save_0 folder there first.");
        action(r, "save.location.pick", tr("Choose folder"), []() { ui_request_import(4); });
        r.back().help = tr("Opens your system's folder picker to choose the save folder.");
        action(r, "save.location.reset", tr("Use the default folder"), []() { s.draft.save_location[0] = 0; },
               *c.save_location != 0);
        r.back().help = tr("Keeps the save in the default folder again (%APPDATA%\\MHGURecomp\\save on Windows, ~/.local/share/MHGURecomp/save on Linux) after Save and a restart. The save in the other folder is left where it is.");
#endif
        break;
#ifdef __ANDROID__
    case ANDROID_PAGE: {  // (Android/data can't be written by the player's file managers: the app copies files in and out)
        header(r, "files.header", tr("App files"));
        const char *files = android_files_folder(), *problem = android_files_folder_problem();
        action(r, "files.folder", tr("Files folder"), []() { ui_request_import(8); });
        r.back().value = *files ? files : *problem ? tr("App folder (chosen folder not usable)") : tr("App folder (default)");
        r.back().help = std::string(tr("Keeps the app's files (settings, save, mods, cheats, screenshots, log.txt) in a folder you choose, so you can open them with this device's file managers: the app's own folder, in Android/data, can't be opened by them. The app copies its files there (it asks before replacing older files already there) and closes; start it again. The game files are used where they are. Android asks for \"All files access\".")) +
                        (*problem ? std::string("\n\n") + problem : std::string());
        action(r, "files.folder.reset", tr("Use the app's folder"), []() { ui_request_import(9); }, *files != 0 || *problem != 0);
        r.back().help = tr("Keeps the app's files in its own folder again: they are copied back (it asks before replacing older files already there) and the app closes. The files in the chosen folder are left where they are.");
        header(r, "save.header", tr("Save data"));
        text(r, "save.folder", "Save folder", c.save_dir, sizeof c.save_dir, *c.save_dir ? c.save_dir : tr("App folder (default)"),
             ini("SaveFolder"));
        r.back().activate = []() { ui_request_import(4); };
        r.back().help = tr("Where the save is kept, so syncing software can keep it the same on your devices: pick the same folder on each. The folder holds save_0, like the PC version's save folder. Android asks for \"All files access\" so the app can use a folder outside its own. Press Save, then restart the app. Your current save is not moved: use Export save first to put it there.");
        action(r, "save.folder.reset", tr("Use the app's folder"), []() { s.draft.save_dir[0] = 0; }, *c.save_dir != 0);
        r.back().help = tr("Keeps the save in the app's own folder again (after Save and a restart). The save in the other folder is left where it is.");
        action(r, "import.save", tr("Import save"), []() { ui_request_import(2); });
        r.back().help = tr("Copies a save's system and system_backup files into the app from any folder on this device. Your current save is kept. The app closes afterwards.");
        action(r, "export.save", tr("Export save"), []() { ui_request_import(3); });
        r.back().help = tr("Copies the save's system and system_backup files out to any folder on this device, as a backup or to move it to another device.");
        header(r, "game.header", tr("Game files and mods"));
        text(r, "game.folder", "Game files folder", c.game_dir, sizeof c.game_dir, *c.game_dir ? c.game_dir : tr("App folder (default)"),
             ini("GameFolder"));
        r.back().activate = []() { ui_request_import(7); };
        r.back().help = tr("Uses the game files where they are, for example on a USB drive or SD card, so they don't have to fit in the app's own folder. Pick the folder that holds exefs and romfs. Android asks for \"All files access\" so the app can read it. Press Save, then restart the app. Nothing is copied; the drive has to stay connected while you play.");
        action(r, "game.folder.reset", tr("Use the app's folder"), []() { s.draft.game_dir[0] = 0; }, *c.game_dir != 0);
        r.back().help = tr("Uses the game files in the app's own folder again (after Save and a restart). The files in the other folder are left where they are.");
        action(r, "import.mod.zip", tr("Import mod (zip)"), []() { ui_request_import(5); });
        r.back().help = tr("Adds a mod from a .zip file anywhere on this device. The app checks the zip's layout and offers to fix it if the game couldn't find the mod's files. Close and reopen the app to load it.");
        action(r, "import.mod.folder", tr("Import mod (folder)"), []() { ui_request_import(6); });
        r.back().help = tr("Adds a mod from a folder anywhere on this device. The app checks the folder's layout and offers to fix it if the game couldn't find the mod's files. Close and reopen the app to load it.");
        action(r, "import.game", tr("Import game files"), []() { ui_request_import(1); });
        r.back().help = tr("Copies the exefs and romfs folders into the app from any folder on this device, replacing the game files it has, and uses them from now on. The app closes afterwards.");
        break;
    }
#endif
    }
    spacer(r, "actions.gap");  // (Save and Reset this tab, and on Menu Options Restart and Exit game, apart)
    if (s.page == APP_PAGE) {
#ifndef __ANDROID__  // (Android can't start the app again from itself: close it and open it again)
        action(r, "restart", tr("Restart"), []() { push(View::Restart, "dialog.cancel"); });
#endif
        action(r, "exit", tr("Exit game"), []() { push(View::Exit, "dialog.cancel"); });
    }
    action(r, "save", tr("Save"), []() { s.command = Command::Save; }, dirty());
    action(r, "reset", tr("Reset this tab"), []() { push(View::Reset, "dialog.cancel"); });
    return r;
}
bool save() {
    if (pending_text()) {
        std::string error;
        if (!s.keyboard.commit(s.keyboard.buffer, error)) {
            while (s.view != View::Keyboard && !s.origins.empty())
                pop();
            s.error = error;
            return false;
        }
        s.keyboard.original = s.keyboard.buffer;
    }
    char error[512];
    if (!config_save(&s.draft, error, sizeof error)) {
        s.error = settings_text(s.draft.language, error);
        return false;
    }
    if (mods_dirty()) {
        if (!mods_save_order(mods.draft.data(), (int)mods.draft.size(), error, sizeof error)) {
            s.error = settings_text(s.draft.language, error);
            return false;
        }
        mods.saved = mods.draft;
    }
    if (cheats_dirty()) {
        std::unique_ptr<bool[]> on(new bool[cheat_tab.draft.size() + 1]);
        for (size_t i = 0; i < cheat_tab.draft.size(); ++i)
            on[i] = cheat_tab.draft[i] != 0;
        if (!cheats_set(on.get(), (int)cheat_tab.draft.size(), error, sizeof error)) {
            s.error = settings_text(s.draft.language, error);
            return false;
        }
        cheat_tab.saved = cheat_tab.draft;
    }
    if (actions_dirty()) {
        if (!acts().draft.layout) acts().draft.layout = acts().layout;  // (what was shown stays: bindings must not turn Simple into Advanced)
        // (after config_save: it keeps the [Actions*] lines it finds, these replace them)
        char path[1100];
        config_path(path, sizeof path);
        if (!act_save(path, &acts().draft)) {
            s.error = tr("Could not save the game actions.");
            return false;
        }
        act_use_profile(&acts().draft);  // in use from the next frame
        input_view_apply();
        acts().saved = acts().draft;
    }
    s.saved = s.draft;
    s.saved_notice = true;
    s.error.clear();
    if (acts().layout == ACT_LAYOUT_SIMPLE && acts().draft.enable) {  // (saved all the same: the player may mean it)
        std::string names;
        for (int d = 0; d < 2; d++) {
            uint64_t which = 0;
            act_rows_unset(&acts().draft, d, &which);
            for (int i = 0; i < act_row_count(); i++)
                if ((which >> i & 1) && (act_row(i)->head == ACT_MENU_CONFIRM || act_row(i)->head == ACT_MENU_CANCEL))
                    names += (names.empty() ? "" : ", ") + std::string(tr(act_row(i)->label)) + " (" + scheme_name(d) + ")";
        }
        if (!names.empty())
            s.error = fmt("Saved. Not set: %s. The menus cannot be used or left with the scheme that has it unset.", names.c_str());
    }
    return true;
}
// Takes a key (or a mouse button) for the keybind (s.capture_bind) or the chat key (-1) being chosen. false: refused
bool bind_take(int key) {
    const char *name = key > 0 && key < KEY_MOUSE ? SDL_GetScancodeName((SDL_Scancode)key) : "";
    const char *reserved = key > 0 && key < KEY_MOUSE ? key_reserved(key) : nullptr;
    if (s.capture_bind >= 0 && key > 0) {
        int *keys = s.draft.binds[s.capture_bind];
        if (reserved) {
            s.error = fmt("That key is the recomp's own (%s).", tr(reserved));
            return false;
        }
        if (!*config_key_name(key)) {
            s.error = tr("That key can't be used.");
            return false;
        }
        if (!s.capture_add)
            keys[0] = key, keys[1] = 0;
        else if (keys[0] != key)
            keys[1] = key;
        return true;
    }
    if (key > 0 && *name && strlen(name) < sizeof s.draft.chat_key && !strpbrk(name, ";#")) {
        SDL_strlcpy(s.draft.chat_key, name, sizeof s.draft.chat_key);
        return true;
    }
    if (key > 0)
        s.error = tr("That key can't be used for chat.");
    return false;
}
std::vector<Row> rows_for_view(bool *open) {
    if (s.view == View::Main)
        return main_rows();
    std::vector<Row> r;
    if (s.view == View::Keyboard && s.keyboard.paste_only) {
        action(r, "key.paste", tr("Paste"), []() { paste_clipboard(); });
        action(r, "key.clear", tr("Clear"), []() {
            s.keyboard.buffer.clear();
            s.keyboard.cursor = 0;
        });
        action(r, "key.accept", tr("Add"), []() { accept_keyboard(); });
        action(r, "key.cancel", tr("Cancel"), []() { pop(); });
    } else if (s.view == View::Keyboard) {
        std::string chars = "1234567890abcdefghijklmnopqrstuvwxyz.:,-_/@";
        if (s.keyboard.upper)
            for (char &c : chars)
                c = (char)SDL_toupper(c);
        for (int i = 0; i < (int)chars.size(); i++) {
            std::string ch(1, chars[i]);
            action(r, "key." + std::to_string(i), ch, [ch]() { insert(ch); });
        }
        action(r, "key.delete", tr("Delete"), []() { erase_previous(); });
        action(r, "key.clear", tr("Clear"), []() {
            s.keyboard.buffer.clear();
            s.keyboard.cursor = 0;
        });
        action(r, "key.space", tr("Space"), []() { insert(" "); });
        action(r, "key.case", tr("Case"), []() { s.keyboard.upper = !s.keyboard.upper; });
        action(r, "key.accept", tr("Accept"), []() { accept_keyboard(); });
        action(r, "key.cancel", tr("Cancel"), []() { pop(); });
    } else if (s.view == View::Conflicts) {
        conflict_rows(r);
        action(r, "conflicts.back", tr("Back"), []() { pop(); });
    } else if (s.view == View::Keybinds) {
        keybind_rows(r);
    } else if (s.view == View::Actions) {
        action_rows(r);
    } else if (s.view == View::KeyCapture && acts().capture >= 0) {
        action_capture_rows(r);
    } else if (s.view == View::KeyCapture) {
        int key = captured_key.exchange(-1);
        if (key == SDL_SCANCODE_ESCAPE)
            pop();
        else if (key > 0 && bind_take(key))
            pop();
        if (s.view == View::KeyCapture) {
            action(r, "capture.none", tr("None"), []() {
                if (s.capture_bind >= 0)
                    s.draft.binds[s.capture_bind][0] = s.draft.binds[s.capture_bind][1] = 0;
                else
                    s.draft.chat_key[0] = 0;
                pop();
            });
            action(r, "capture.cancel", tr("Cancel"), []() { pop(); });
        }
    } else if (s.view == View::Move) {
        action(r, "move.accept", tr("Accept"), []() { pop(); });
        action(r, "move.cancel", tr("Cancel"), []() {
            s.move_position = s.move_start;
            s.position_override = true;
            pop();
        });
    } else {
        action(r, "dialog.cancel", tr("Cancel"), []() { pop(); });
        if (s.view == View::Reset)
            action(r, "dialog.reset", tr("Reset"), []() { reset_section(); });
        if (s.view == View::CloseChanges) {
            action(r, "dialog.save", tr("Save and close"), [open]() {
                if (save())
                    *open = false;
            });
            action(r, "dialog.discard", tr("Discard and close"), [open]() {
                discard();
                *open = false;
            });
        }
        if (s.view == View::Exit) {
            action(r, "dialog.exit", tr((dirty() || pending_text()) ? "Save and exit" : "Exit game"), []() {
                if ((!dirty() && !pending_text()) || save())
                    ui_request_action(false);
            });
            if (dirty() || pending_text())
                action(r, "dialog.discard", tr("Exit without saving"), []() { ui_request_action(false); });
        }
        if (s.view == View::Restart)
            action(r, "dialog.restart", tr("Save and restart"), []() {
                if (save())
                    ui_request_action(true);
            });
    }
    return r;
}
// The open dropdown's choices
std::vector<Row> dropdown_rows() {
    std::vector<Row> r;
    for (int i = 0; i < (int)s.dropdown.names.size(); i++) {
        action(r, "choice." + std::to_string(i), s.dropdown.names[i], [i]() {
            auto set = s.dropdown.set;
            close_dropdown();
            set(i);
        });
        r.back().current = i == s.dropdown.selected;
    }
    return r;
}
// A row's help: its own, or what the default ini says of its setting (each line looked up in settings_locale.h)
std::string help_text(const Row &r) {
    if (!r.help.empty() || r.key.empty())
        return r.help;
    static std::vector<std::pair<std::string, std::string>> cache;  // (key, help in English)
    auto at = std::find_if(cache.begin(), cache.end(), [&](const auto &e) { return e.first == r.key; });
    if (at == cache.end()) {
        char buf[2048];
        cache.emplace_back(r.key, config_help(r.key.c_str(), buf, sizeof buf) ? buf : "");
        at = cache.end() - 1;
    }
    std::string out, line;
    for (size_t i = 0, j; i < at->second.size(); i = j + 1) {
        j = at->second.find('\n', i);
        if (j == std::string::npos)
            j = at->second.size();
        line = at->second.substr(i, j - i);
        out += (out.empty() ? "" : "\n") + std::string(tr(line.c_str()));
    }
    return out;
}
bool pressed(ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); }
bool direction(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return pressed(a, true) || pressed(b, true) || pressed(c, true);
}
// list: a dropdown's choices (left / right do nothing there)
void navigate(std::vector<Row> &rows, bool list = false) {
    if (rows.empty())
        return;
    auto it =
        std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.id == s.focus && r.enabled; });
    if (it == rows.end()) {
        it = std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.enabled; });
        if (it == rows.end())
            return;
        focus(it->id);
    }
    int index = (int)(it - rows.begin());
    int dy = direction(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown) ? 1
             : direction(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp)     ? -1
                                                                                                 : 0;
    int dx = direction(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight) ? 1
             : direction(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft)  ? -1
                                                                                                    : 0;
    if (pressed(ImGuiKey_Tab, true))
        dy = ImGui::GetIO().KeyShift ? -1 : 1;
    if (list)
        dx = 0;
    if (dx && it->adjust) {
        it->adjust(dx);
        return;
    }
    int delta = dy;
    if (s.view == View::Keyboard && (dx || dy) && !pressed(ImGuiKey_Tab, true)) {
        auto from =
            std::find_if(s.targets.begin(), s.targets.end(), [](const Target &t) { return t.id == s.focus; });
        if (from != s.targets.end()) {
            float best = 1e30f;
            std::string next;
            for (const auto &t : s.targets) {
                float x = t.center.x - from->center.x, y = t.center.y - from->center.y;
                if (!t.enabled || (dy && y * dy <= 1) || (dx && (x * dx <= 1 || std::abs(y) > 1)))
                    continue;
                float score = dy ? std::abs(y) * 1000 + std::abs(x) : std::abs(x);
                if (score < best) {
                    best = score;
                    next = t.id;
                }
            }
            if (!next.empty())
                focus(next);
            return;
        }
    }
    if (dx)
        delta = dx;
    if (delta) {
        int next = std::clamp(index + delta, 0, (int)rows.size() - 1);
        int step = delta > 0 ? 1 : -1;
        while (next >= 0 && next < (int)rows.size() && !rows[next].enabled)
            next += step;
        if (next >= 0 && next < (int)rows.size())
            focus(rows[next].id);
    }
    auto first = std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.enabled; });
    auto last = std::find_if(rows.rbegin(), rows.rend(), [](const Row &r) { return r.enabled; });
    if (pressed(ImGuiKey_Home))
        focus(first->id);
    if (pressed(ImGuiKey_End))
        focus(last->id);
}
// Keep ImGui internals at this boundary: rows retain native activation and rendering.
void own_focus(ImGuiWindow *w, ImGuiID id, const ImRect &rect) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    // Focus first: a click outside the menu leaves no focused window, and SetNavID asserts one
    ImGui::FocusWindow(w);
    ImGui::SetNavID(id, ImGuiNavLayer_Main, 0,
                    ImRect(ImVec2(rect.Min.x - w->Pos.x, rect.Min.y - w->Pos.y),
                           ImVec2(rect.Max.x - w->Pos.x, rect.Max.y - w->Pos.y)));
    g.NavCursorVisible = true;
    g.NavInitRequest = false;
    g.NavMoveSubmitted = false;
    // Let auto-sizing settle after a view change; do not fight later mouse-wheel scrolling.
    if (s.focus_request) {
        s.focus_scroll_frames = 3;
        s.focus_request = false;
    }
    if (s.focus_scroll_frames > 0) {
        --s.focus_scroll_frames;
        if (s.guide_height) {
            float top = w->ClipRect.Min.y, bottom = w->ClipRect.Max.y;
            float delta = rect.Min.y < top ? rect.Min.y - top : rect.Max.y > bottom ? rect.Max.y - bottom : 0;
            if (delta)
                ImGui::SetScrollY(w, w->Scroll.y + delta);
        } else
            ImGui::ScrollToRect(w, rect, ImGuiScrollFlags_KeepVisibleEdgeY);
    }
}
// list: a dropdown's choices; else the view's rows (while a dropdown is open they keep its row marked, the focus being
// in the dropdown)
void draw_rows(std::vector<Row> &rows, bool list = false) {
    auto *w = ImGui::GetCurrentWindow();
    float font = ImGui::GetFontSize(), width = ImGui::GetContentRegionAvail().x;
    bool keyboard_view = s.view == View::Keyboard && !list;
    bool passive = s.dropdown.open && !list;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float cell = (width - 9 * gap) / 10;
    if (!list) {
        s.targets.clear();
        s.hovered.clear();
        s.pointing = false;
        s.dropdown.anchored = false;
    }
    std::function<void()> activate;
    for (int i = 0; i < (int)rows.size(); i++) {
        Row &r = rows[i];
        if (r.header) {  // (a section's title: accent-coloured, a little apart from the rows above it)
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = font * (i ? 2.2f : 1.6f);
            ImGui::Dummy(ImVec2(width, h));
            w->DrawList->AddText(ImGui::GetFont(), font, ImVec2(p.x + font * .25f, p.y + h - font * 1.35f),
                                 theme().accent, r.title.c_str());
            continue;
        }
        bool grid = keyboard_view && r.id.size() > 4 && r.id[4] >= '0' && r.id[4] <= '9';
        float rw = grid ? cell : width;
        if (grid && i % 10)
            ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float wrap = std::max(font, rw * (r.action ? 1.f : .58f) - font);
        ImVec2 label = ImGui::CalcTextSize(r.title.c_str(), nullptr, false, wrap);
        float h = grid ? font * 2 : std::max(font * 2.1f, label.y + font * .8f);
        ImGui::PushID(r.id.c_str());
        ImGuiID id = w->GetID("row");
        ImRect rect(p, ImVec2(p.x + rw, p.y + h));
        bool selected = passive ? r.id == s.dropdown.row : s.focus == r.id;
        if (passive && selected)
            s.dropdown.anchor = rect, s.dropdown.anchored = true;
        const Theme &t = theme();
        ImGui::PushStyleColor(ImGuiCol_Button, vec(selected && r.enabled ? t.row_focus : t.row));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(selected && r.enabled ? t.row_focus : t.row_hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(t.row_focus));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, t.rounding);
        ImGui::BeginDisabled(!r.enabled);
        bool clicked = ImGui::Button("###row", ImVec2(rw, h));
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (!list && ImGui::IsItemHovered())
            s.hovered = r.id;
        if (!list && ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))
            s.pointing = true;
        if (selected && r.enabled) {
            if (!passive)
                own_focus(w, id, rect);
            if (t.bar)
                w->DrawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + font * .22f, rect.Max.y), t.accent,
                                           t.rounding, ImDrawFlags_RoundCornersLeft);
            else if (!t.game)
                w->DrawList->AddRect(rect.Min, rect.Max, t.accent, t.rounding, 0, 2.f);
        }
        if (t.game && !grid)  // (Hunter: a line under each row)
            w->DrawList->AddLine(ImVec2(rect.Min.x, rect.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f),
                                 ImVec2(rect.Max.x, rect.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f), t.divider,
                                 2.f);
        w->DrawList->PushClipRect(rect.Min, rect.Max, true);
        ImU32 ink = !r.enabled ? t.text_off : selected && t.bar ? t.accent_bright : t.text;
        ImU32 value_ink = r.warn ? IM_COL32(255, 110, 96, 255) : ink;
        if (r.current)  // (the dropdown's choice now: a bar on its left)
            w->DrawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + font * .22f, rect.Max.y), t.accent,
                                       t.rounding, ImDrawFlags_RoundCornersLeft);
        w->DrawList->AddText(ImGui::GetFont(), font, ImVec2(p.x + font * .5f, p.y + (h - label.y) * .5f), ink,
                             r.title.c_str(), nullptr, wrap);
        float arrow = 0;
        if (r.menu && r.enabled) {  // (Confirm opens a dropdown: an arrow after the value)
            arrow = font * 1.1f;
            ImGui::RenderArrow(w->DrawList, ImVec2(p.x + rw - font * .5f - font * .7f, p.y + (h - font) * .5f), ink,
                               s.dropdown.open && s.dropdown.row == r.id ? ImGuiDir_Up : ImGuiDir_Down, .7f);
        }
        if (!r.value.empty()) {
            std::string value = r.value + (r.cold ? " *" : "");
            float available = rw * .38f - font - arrow;
            if (ImGui::CalcTextSize(value.c_str()).x > available) {
                do {
                    size_t n = value.size() - 1;
                    while (n && ((unsigned char)value[n] & 0xc0) == 0x80)
                        --n;
                    value.resize(n);
                } while (!value.empty() && ImGui::CalcTextSize((value + "...").c_str()).x > available);
                value += "...";
            }
            auto sz = ImGui::CalcTextSize(value.c_str());
            w->DrawList->AddText(ImVec2(p.x + rw - font * .5f - arrow - sz.x, p.y + (h - font) * .5f), value_ink,
                                 value.c_str());
        }
        w->DrawList->PopClipRect();
        s.targets.push_back({r.id, rect.GetCenter(), r.enabled});
        ImGui::PopID();
        if (clicked && r.enabled) {
            focus(r.id);
            activate = r.activate;
        }
    }
    if (activate)
        activate();
}
// The open dropdown, a window of its own over the menu: under its row, at the right, or above the row when there is
// more room there; it scrolls when it doesn't fit. scale, base: the menu's (settings_panel)
void draw_dropdown(std::vector<Row> &options, float scale, float base) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImGuiStyle &style = ImGui::GetStyle();
    float font = base * scale, longest = 0;
    for (const auto &o : options)
        longest = std::max(longest, ImGui::CalcTextSize(o.title.c_str()).x * scale);
    const ImRect &a = s.dropdown.anchor;
    float w = std::clamp(longest + font * 2 + style.WindowPadding.x * 2, a.GetWidth() * .4f, a.GetWidth());
    float h = options.size() * (font * 2.1f + style.ItemSpacing.y) - style.ItemSpacing.y + style.WindowPadding.y * 2;
    float below = vp->WorkPos.y + vp->WorkSize.y - a.Max.y, above = a.Min.y - vp->WorkPos.y;
    bool up = h > below && above > below;
    h = std::min(h, up ? above : below);
    ImGui::SetNextWindowPos(ImVec2(a.Max.x - w, up ? a.Min.y - h : a.Max.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(theme().popup));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(theme().edge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme().rounding);
    ImGui::Begin("Settings##dropdown", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(scale);
    draw_rows(options, true);
    s.dropdown.hovered = ImGui::IsWindowHovered();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
// Hunter's window frame, over the rest: a red rule with a green one inside it along the top and the bottom, and a gold
// square on each corner
void draw_frame(ImGuiWindow *w, const Theme &look) {
    if (!look.game)
        return;
    const ImU32 red = IM_COL32(176, 34, 30, 255), green = IM_COL32(80, 142, 54, 255), gold = IM_COL32(222, 190, 40, 255);
    float b = w->WindowBorderSize, x0 = w->Pos.x + b, x1 = w->Pos.x + w->Size.x - b, y0 = w->Pos.y + b,
          y1 = w->Pos.y + w->Size.y - b, line = std::max(2.f, ImGui::GetFontSize() * .14f);
    w->DrawList->PushClipRectFullScreen();
    w->DrawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + line), red);
    w->DrawList->AddRectFilled(ImVec2(x0, y0 + line), ImVec2(x1, y0 + line * 1.7f), green);
    w->DrawList->AddRectFilled(ImVec2(x0, y1 - line * 1.7f), ImVec2(x1, y1 - line), green);
    w->DrawList->AddRectFilled(ImVec2(x0, y1 - line), ImVec2(x1, y1), red);
    float c = ImGui::GetFontSize() * .5f;
    for (ImVec2 corner : {w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y), ImVec2(w->Pos.x, w->Pos.y + w->Size.y),
                          ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y)}) {
        ImVec2 a(corner.x - c * .5f, corner.y - c * .5f), z(corner.x + c * .5f, corner.y + c * .5f);
        w->DrawList->AddRectFilled(a, z, IM_COL32(0, 0, 0, 255));
        w->DrawList->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(z.x - 1, z.y - 1), gold, 0, 0, std::max(1.f, c * .16f));
        w->DrawList->AddRectFilled(ImVec2(corner.x - c * .14f, corner.y - c * .14f),
                                   ImVec2(corner.x + c * .14f, corner.y + c * .14f), gold);
    }
    w->DrawList->PopClipRect();
}
// The corner grip that sizes the menu (mouse_drag): three short lines across the bottom right corner
float grip_size() { return ImGui::GetFontSize() * 1.3f; }
void draw_grip(ImGuiWindow *w, const Theme &look) {
    ImVec2 corner(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    float g = grip_size(), inset = look.bar ? g * .35f : g * .2f;
    bool lit =
        s.dragging == 2 || ImRect(ImVec2(corner.x - g, corner.y - g), corner).Contains(ImGui::GetIO().MousePos);
    w->DrawList->PushClipRectFullScreen();
    for (int i = 1; i <= 3; i++) {
        float d = g * .22f * i;
        w->DrawList->AddLine(ImVec2(corner.x - inset - d, corner.y - inset),
                             ImVec2(corner.x - inset, corner.y - inset - d), lit ? look.accent : look.text_off, 1.5f);
    }
    w->DrawList->PopClipRect();
}
// The mouse on the menu, before it is drawn: a press on its top part (the tabs, or a dialog's title) and a drag moves
// it, one on the grip at its bottom right corner sizes it. Either one detaches it (Menu Options: Attach menu puts it back).
// The drawing in a window of its own beside the list, when the screen is wide enough
void draw_bind_window(ImVec2 pos, float width, float min_h, float scale, float margin, const Theme &look) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, std::min(min_h, vp->WorkSize.y - 2 * margin)),
                                        ImVec2(width, std::max(min_h, vp->WorkSize.y - 2 * margin)));
    ImGui::Begin("Settings##bind", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(scale);
    float fs = ImGui::GetFontSize();
    draw_bind_view(ImGui::GetContentRegionAvail().x, vp->WorkSize.y - 2 * margin - fs * 14.f, true);
    g_bv_pointing |= ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);  // (a click anywhere in this window is not a mouse button chosen)
    draw_frame(ImGui::GetCurrentWindow(), look);
    ImGui::End();
}
void mouse_drag(ImGuiViewport *vp, float width) {
    ImGuiIO &io = ImGui::GetIO();
    auto *w = ImGui::FindWindowByName("Settings##main");
    if (!w || s.view == View::Move)
        return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !s.dropdown.open) {
        float g = grip_size();
        ImVec2 corner(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        s.dragged = false;
        bool grip = ImRect(ImVec2(corner.x - g, corner.y - g), corner).Contains(io.MousePos);
        bool top = ImRect(w->Pos, ImVec2(corner.x, w->Pos.y + s.header_bottom)).Contains(io.MousePos);
        s.dragging = grip ? 2 : top ? 1 : 0;
        s.drag_mouse = io.MousePos;
        s.drag_from = s.dragging == 2 ? w->Size : w->Pos;
    }
    if (!s.dragging)
        return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s.dragging = 0;
        return;
    }
    ImVec2 d(io.MousePos.x - s.drag_mouse.x, io.MousePos.y - s.drag_mouse.y);
    if (!s.dragged && d.x * d.x + d.y * d.y < io.MouseDragThreshold * io.MouseDragThreshold)
        return;
    s.dragged = true;
    if (!s.floating) {
        s.floating = true;
        s.move_position = w->Pos;
    }
    if (s.dragging == 2) {
        s.custom_size = ImVec2(s.drag_from.x + d.x, s.drag_from.y + d.y);
        s.move_position = w->Pos;
    } else {
        float h = w->Size.y;
        s.move_position.x = std::clamp(s.drag_from.x + d.x, vp->WorkPos.x - width * .5f,
                                       vp->WorkPos.x + vp->WorkSize.x - width * .5f);
        s.move_position.y = std::clamp(s.drag_from.y + d.y, vp->WorkPos.y,
                                       vp->WorkPos.y + std::max(0.f, vp->WorkSize.y - h * .25f));
    }
    s.position_override = true;
}
} // namespace
void settings_panel_request_close() { request_exit.store(true); }
void settings_panel_request_back() { request_back.store(true); }
bool settings_panel_take_forge() {
    bool was = forge_requested;
    forge_requested = false;
    return was;
}
bool settings_panel_dirty() { return is_dirty.load(); }
int settings_panel_theme() { return s.editing ? s.draft.menu_theme : config_editable().menu_theme; }
bool settings_panel_confirming() { return is_confirming.load() || request_exit.load(); }
bool settings_panel_key(int scancode) {
    if (!is_capturing.load())
        return false;
    captured_key = scancode;
    return true;
}
bool settings_panel_mouse(int button) {
    if (!capture_mouse.load() || button < 1 || button > SDL_BUTTON_X2 ||
        (button == SDL_BUTTON_LEFT && capture_pointing.load()))
        return false;
    captured_key = KEY_MOUSE + button;
    return true;
}
bool settings_panel_pad_capture() { return capture_pad.load(); }
void settings_panel_pad(int code) {
    if (capture_pad.load())
        captured_key = code;
}
bool settings_panel_wheel(int notches) {
    if (!capture_wheel.load() || !notches)
        return false;
    captured_key = notches > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    return true;
}
// Ownership has to precede NewFrame: native cancel/windowing runs there.
void settings_panel_before_frame(bool open) {
    if (!open && !request_exit.load())
        return;
    const ImGuiKey keys[] = {ImGuiKey_GamepadFaceRight,
                             ImGuiKey_GamepadFaceLeft,
                             ImGuiKey_GamepadFaceUp,
                             ImGuiKey_GamepadL1,
                             ImGuiKey_GamepadR1,
                             ImGuiKey_GamepadDpadUp,
                             ImGuiKey_GamepadDpadDown,
                             ImGuiKey_GamepadDpadLeft,
                             ImGuiKey_GamepadDpadRight,
                             ImGuiKey_GamepadLStickUp,
                             ImGuiKey_GamepadLStickDown,
                             ImGuiKey_GamepadLStickLeft,
                             ImGuiKey_GamepadLStickRight,
                             ImGuiKey_Escape,
                             ImGuiKey_UpArrow,
                             ImGuiKey_DownArrow,
                             ImGuiKey_LeftArrow,
                             ImGuiKey_RightArrow,
                             ImGuiKey_Tab,
                             ImGuiKey_Home,
                             ImGuiKey_End};
    for (ImGuiKey key : keys)
        ImGui::SetKeyOwner(key, 0x6d686775, ImGuiInputFlags_LockThisFrame);
    if (s.view == View::Keyboard) {
        ImGui::SetKeyOwner(ImGuiKey_Enter, 0x6d686775, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Space, 0x6d686775, ImGuiInputFlags_LockThisFrame);
    }
}
void settings_panel(bool *open) {
    if (!*open) {
        s.editing = false;
        is_dirty = false;
        is_confirming = false;
        is_capturing = false;
        capture_mouse = false;
        capture_pad = false;
        return;
    }
    if (!s.editing) {
        act_get_profile(&acts().saved);
        acts().draft = acts().saved;
        acts().capture = -1;
        acts().capture_row = -1;
        acts().tab = 0;
        acts().dev = acts().saved.scheme == ACT_SCHEME_KBM ? 1 : acts().saved.scheme == ACT_SCHEME_PAD ? 0 : host_input_last_device();
        acts().weapon = 0;
        acts().layout = act_layout(&acts().saved);
        s.draft = s.saved = config_editable();
        s.editing = true;
        s.view = View::Main;
        s.origins.clear();
        s.focus.clear();
        s.focus_request = true;
        s.error.clear();
        s.saved_notice = false;
        s.floating = false;
        s.advanced = false;
        s.dropdown.open = false;
        s.help_row.clear();
        quest_keys();
        load_mods();
        cheats_rescan();  // (files added, edited or removed since the last time count from now)
        load_cheats();
    }
    if (request_exit.exchange(false) && s.view != View::Exit)
        push(View::Exit, "dialog.cancel");
    Config before = s.draft;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = s.draft.menu_scale / 100.f;
    float base = ImGui::GetFontSize();
    float margin = std::min(16.f, vp->WorkSize.x * .025f);
    float width = std::min(vp->WorkSize.x - 2 * margin, base * scale * 56);
    bool sized = s.custom_size.x > 0;
    // the drawing of the controller or the keyboard: above the list in the same window, or beside it in a window of its own
    bool bv_view = s.view == View::Actions || (s.view == View::KeyCapture && (acts().capture >= 0 || s.capture_from_actions));
    int bv_mode = 0;  // 0 none, 1 in the window, 2 beside it
    float bv_gap = std::max(6.f, base * scale * .6f), bv_width = 0;
    if (bv_view) {
        float least = base * scale * 30, list_min = base * scale * 40, want = base * scale * 72;
        float whole = vp->WorkSize.x - 2 * margin;
        if (!sized && !s.floating && whole >= list_min + bv_gap + least)  // (the list a little narrower: the drawing is the wider of the two)
            width = std::clamp(whole - bv_gap - want, list_min, width);
        float list = sized ? s.custom_size.x : width;
        float room = whole - list - bv_gap;
        if (s.floating)
            room = vp->WorkPos.x + vp->WorkSize.x - margin - (s.move_position.x + list) - bv_gap;
        bv_mode = room >= least ? 2 : 1;
        bv_width = std::min(room, base * scale * 120);
    }
    if (sized) {
        ImVec2 least(std::min(base * scale * 26, vp->WorkSize.x), std::min(base * scale * 14, vp->WorkSize.y));
        s.custom_size.x = std::clamp(s.custom_size.x, least.x, vp->WorkSize.x);
        s.custom_size.y = std::clamp(s.custom_size.y, least.y, vp->WorkSize.y);
        width = s.custom_size.x;
        ImGui::SetNextWindowSize(s.custom_size, ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->WorkSize.y - 2 * margin));
    }
    mouse_drag(vp, width);
    if (s.view == View::Move) {
        float x =
            (direction(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight) ? 1 : 0) -
            (direction(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft) ? 1 : 0);
        float y =
            (direction(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown) ? 1 : 0) -
            (direction(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp) ? 1 : 0);
        auto *w = ImGui::FindWindowByName("Settings##main");
        float h = w ? w->Size.y : 200;
        s.move_position.x = std::clamp(s.move_position.x + x * 16, vp->WorkPos.x,
                                       vp->WorkPos.x + std::max(0.f, vp->WorkSize.x - width));
        s.move_position.y = std::clamp(s.move_position.y + y * 16, vp->WorkPos.y,
                                       vp->WorkPos.y + std::max(0.f, vp->WorkSize.y - h));
        s.position_override = true;
    }
    if (s.position_override && s.floating) {
        ImGui::SetNextWindowPos(s.move_position, ImGuiCond_Always);
        s.position_override = false;
    }
    if (!s.floating) {
        float pair = width + (bv_mode == 2 ? bv_gap + bv_width : 0);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - pair) * .5f, vp->WorkPos.y + margin), ImGuiCond_Always);
    }
    const Theme &look = theme();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(look.panel));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(look.text));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(look.edge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        look.bar ? 12.f : look.game ? 0.f : ImGui::GetStyle().WindowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                        look.bar ? 1.5f : look.game ? 2.f : ImGui::GetStyle().WindowBorderSize);
    ImGui::Begin("Settings##main", nullptr,
                 (sized ? 0 : ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(scale);
    if (s.restore_scroll) {
        ImGui::SetScrollY(s.scroll);
        s.restore_scroll = false;
    }
    if (request_back.exchange(false) || pressed(ImGuiKey_GamepadFaceRight) || pressed(ImGuiKey_Escape))
        ask_back(open);
    if (s.view == View::Actions && !s.dropdown.open) {  // (what the list and the drawing show)
        if (pressed(ImGuiKey_GamepadR1))
            tab_step(1);
        if (pressed(ImGuiKey_GamepadL1))
            tab_step(-1);
    }
    if (s.dropdown.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !s.dropdown.hovered)
        close_dropdown();  // (a click elsewhere: on a row, it acts as it does with no dropdown open)
    bool pinned_guides = s.view == View::Main;
    bool clipped_body = false;
    auto *window = ImGui::GetCurrentWindow();
    float spacing = ImGui::GetStyle().ItemSpacing.y;
    GuideLayout guides = guide_layout(ImGui::GetContentRegionAvail().x);
    // pinned above the guides: the help of the row focused or pointed at, with room for the longest of the tab
    float help_area = pinned_guides && s.help_height > 0 ? s.help_height + spacing * 2 : 0;
    s.guide_height = pinned_guides ? guides.height + help_area : 0;
    float guide_top =
        std::max(window->Pos.y + window->Size.y - ImGui::GetStyle().WindowPadding.y - guides.height,
                 window->InnerClipRect.Min.y);
    float help_top = std::max(guide_top - help_area, window->InnerClipRect.Min.y);
    if (pinned_guides)
        ImGui::PushClipRect(
            window->InnerClipRect.Min,
            ImVec2(window->InnerClipRect.Max.x, std::max(window->InnerClipRect.Min.y, help_top - spacing)), true);
    if (s.view == View::Main) {
        // Save from anywhere on the tab: the top face button, or Ctrl+S
        if ((pressed(ImGuiKey_GamepadFaceUp) || (ImGui::GetIO().KeyCtrl && pressed(ImGuiKey_S))) && dirty() &&
            !s.dropdown.open)
            s.command = Command::Save;
        if (pressed(ImGuiKey_GamepadR1))
            first_page((s.page + 1) % PAGES);
        if (pressed(ImGuiKey_GamepadL1))
            first_page((s.page + PAGES - 1) % PAGES);
        const char *pages[] = {"Display", "Graphics & Audio", "Controls", "Network", "Misc", "Mods", "Cheats", "Menu Options"
#ifdef __ANDROID__
                               , "Android"
#endif
        };
        float avail = ImGui::GetContentRegionAvail().x;
        float fw = ImGui::GetFontSize();
        ImVec2 scrolling_start = ImGui::GetCursorScreenPos();
        ImVec2 tabs_start(scrolling_start.x, window->Pos.y + ImGui::GetStyle().WindowPadding.y);
        ImGui::SetCursorScreenPos(tabs_start);
        float gutter = fw * 2.1f;
        float tabs_width = avail - gutter * 2;
        int columns = tabs_width / fw < 50 ? 4 : PAGES;
        float tabw = (tabs_width - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
        float tabh = fw * (columns == PAGES ? 2.7f : 2.1f);
        int lines = (PAGES + columns - 1) / columns;
        float header_height = tabh * lines + ImGui::GetStyle().ItemSpacing.y * (lines - 1);
        auto *header = ImGui::GetWindowDrawList();
        {  // (over the rows scrolled under the tabs; inside the border, with the window's rounded top corners - the
           // right one left to the window where the scrollbar is)
            float border = window->WindowBorderSize;
            bool scrollbar = window->ScrollbarY;
            header->AddRectFilled(
                ImVec2(window->Pos.x + border, window->Pos.y + border),
                ImVec2(scrollbar ? window->InnerRect.Max.x : window->Pos.x + window->Size.x - border,
                       tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y),
                look.game ? look.header : ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK, std::max(0.f, window->WindowRounding - border),
                scrollbar ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTop);
        }
        SettingsIcons::draw(header, ImVec2(tabs_start.x, tabs_start.y + (header_height - fw) * .5f), fw,
                            s.draft.button_icons, SettingsIcons::Left, 0);
        SettingsIcons::draw(
            header, ImVec2(tabs_start.x + avail - fw * 1.5f, tabs_start.y + (header_height - fw) * .5f), fw,
            s.draft.button_icons, SettingsIcons::Right, 0);
        for (int i = 0; i < PAGES; i++) {
            if (i % columns)
                ImGui::SameLine();
            else
                ImGui::SetCursorPosX(tabs_start.x - ImGui::GetWindowPos().x + gutter);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool active = s.page == i;
            ImGui::PushStyleColor(ImGuiCol_Button, vec(active ? look.tab_active : look.tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(look.row_hover));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, look.rounding);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button("###tab", ImVec2(tabw, tabh)) && !s.dragged)
                first_page(i);
            ImGui::PopItemFlag();
            ImGui::PopStyleVar();
            // A label wraps at its spaces; one word wider than the tab is drawn smaller instead of
            // being cut in two
            const char *name = tr(pages[i]);
            float room = tabw - fw * .5f, size = fw;
            float word = ImGui::CalcTextSize(name).x;
            if (!strchr(name, ' ') && word > room) size = fw * room / word;
            auto label = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, room, name);
            // (Gold: the names centred, the other tabs dimmed, the active one underlined in its middle)
            bool named = look.bar || look.game;
            float indent = named ? (tabw - label.x) * .5f : fw * .25f;
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size,
                                                ImVec2(p.x + indent, p.y + (tabh - label.y) * .5f),
                                                named ? (active ? look.accent_bright : look.text_dim) : look.text,
                                                name, nullptr, room);
            float inset = named ? tabw * .18f : 0;
            if (active)
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + inset, p.y + tabh - 3),
                                                          ImVec2(p.x + tabw - inset, p.y + tabh), look.accent,
                                                          named && !look.game ? 2.f : 0.f);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        if (look.bar) {  // (Gold: a rule under the tabs, gold at the sides and fading out in the middle)
            float y = tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y * .5f;
            float mid = (window->InnerRect.Min.x + window->InnerRect.Max.x) * .5f;
            ImU32 clear = look.accent & ~IM_COL32_A_MASK;
            header->AddRectFilledMultiColor(ImVec2(tabs_start.x, y), ImVec2(mid, y + 2), look.accent, clear, clear,
                                            look.accent);
            header->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(tabs_start.x + avail, y + 2), clear, look.accent,
                                            look.accent, clear);
        }
        ImGui::SetCursorScreenPos(
            ImVec2(scrolling_start.x, scrolling_start.y + header_height + ImGui::GetStyle().ItemSpacing.y));
        float body_top = tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y;
        s.header_bottom = body_top - window->Pos.y;
        ImGui::PushClipRect(ImVec2(window->ClipRect.Min.x, std::min(body_top, window->ClipRect.Max.y)),
                            window->ClipRect.Max, true);
        clipped_body = true;
    } else {
        std::string title = s.view == View::Keyboard ? s.keyboard.title
                            : s.view == View::KeyCapture && acts().capture >= 0
                                ? fmt(acts().dev == 0 ? "Press a controller button for: %s (Esc or wait: cancel)"
                                                      : "Press a key or mouse button for: %s (Esc: cancel)",
                                      tr(acts().capture_row >= 0 ? act_row(acts().capture_row)->label : act_label(acts().capture)))
                            : s.view == View::Actions ? fmt("Game actions: %s", scheme_name(acts().dev))
                            : s.view == View::KeyCapture && s.capture_bind >= 0
                                ? fmt("Press a key or mouse button for %s (Esc: cancel)", tr(bind_titles[s.capture_bind]))
                                : tr(s.view == View::Exit         ? "Exit game?"
                                     : s.view == View::Restart    ? "Restart game?"
                                     : s.view == View::Reset      ? "Reset this tab?"
                                     : s.view == View::Move       ? "Move menu"
                                     : s.view == View::Conflicts  ? "Mod conflicts"
                                     : s.view == View::Keybinds   ? "Keybinds"
                                     : s.view == View::KeyCapture ? "Press the key for chat (Esc: cancel)"
                                                                  : "Unsaved changes");
        ImGui::TextWrapped("%s", title.c_str());
    }
    if (s.view == View::Keyboard) {
        ImGuiIO &io = ImGui::GetIO();
        for (ImWchar ch : io.InputQueueCharacters) {
            char utf8[5];
            int n = ImTextCharToUtf8(utf8, ch);
            utf8[n] = 0;
            insert(utf8);
        }
        if (pressed(ImGuiKey_Backspace, true))
            erase_previous();
        if (io.KeyCtrl && pressed(ImGuiKey_V))
            paste_clipboard();
        if (io.KeyCtrl && pressed(ImGuiKey_A))
            s.keyboard.select_all = true;
        if (pressed(ImGuiKey_Delete, true) && s.keyboard.select_all)
            erase_previous();
        else if (pressed(ImGuiKey_Delete, true) && s.keyboard.cursor < s.keyboard.buffer.size()) {
            size_t n = s.keyboard.cursor + 1;
            while (n < s.keyboard.buffer.size() && ((unsigned char)s.keyboard.buffer[n] & 0xc0) == 0x80)
                ++n;
            s.keyboard.buffer.erase(s.keyboard.cursor, n - s.keyboard.cursor);
        }
        if (pressed(ImGuiKey_GamepadL1, true) && s.keyboard.cursor) {
            --s.keyboard.cursor;
            while (s.keyboard.cursor && ((unsigned char)s.keyboard.buffer[s.keyboard.cursor] & 0xc0) == 0x80)
                --s.keyboard.cursor;
        }
        if (pressed(ImGuiKey_GamepadR1, true) && s.keyboard.cursor < s.keyboard.buffer.size()) {
            ++s.keyboard.cursor;
            while (s.keyboard.cursor < s.keyboard.buffer.size() &&
                   ((unsigned char)s.keyboard.buffer[s.keyboard.cursor] & 0xc0) == 0x80)
                ++s.keyboard.cursor;
        }
        if (pressed(ImGuiKey_Enter))
            accept_keyboard();
        if (s.view == View::Keyboard) {
            auto text = s.keyboard.buffer;
            text.insert(s.keyboard.cursor, "|");
            ImGui::TextWrapped("%s", text.c_str());
        }
    }
    g_bv_pointing = false;
    if (bv_mode == 1)
        draw_bind_view(ImGui::GetContentRegionAvail().x, std::max(base * scale * 18, (vp->WorkSize.y - 2 * margin) * .5f), false);
    auto rows = rows_for_view(open);
    if (bv_view && s.view == View::Actions) {  // (the help of the row moved to, or pointed at, for under the drawing)
        static std::string last_focus, help_row;
        if (s.focus != last_focus)
            last_focus = help_row = s.focus;
        ImVec2 moved = ImGui::GetIO().MouseDelta;
        if ((moved.x || moved.y) && !s.hovered.empty())
            help_row = s.hovered;
        g_bv_help.clear();
        for (const Row &r : rows)
            if (r.id == help_row)
                g_bv_help = help_text(r);
    }
    std::vector<Row> options;  // (a dropdown open: the controller moves in it)
    if (s.dropdown.open)
        options = dropdown_rows();
    if (s.view == View::Move)
        focus("move.accept");
    else
        navigate(s.dropdown.open ? options : rows, s.dropdown.open);
    draw_rows(rows);
    float help_wrap = window->InnerRect.GetWidth() - 2 * ImGui::GetStyle().WindowPadding.x;
    std::string help;
    if (s.view == View::Main) {  // (the row moved to, or the one the mouse moved onto)
        static std::string last_focus;
        if (s.focus != last_focus)
            last_focus = s.help_row = s.focus;
        ImVec2 moved = ImGui::GetIO().MouseDelta;
        if ((moved.x || moved.y) && !s.hovered.empty())
            s.help_row = s.hovered;
        const std::string &id = s.dropdown.open ? s.dropdown.row : s.help_row;
        float tallest = 0;
        for (const auto &r : rows) {
            std::string text = help_text(r);
            if (!text.empty())
                tallest = std::max(tallest, ImGui::CalcTextSize(text.c_str(), nullptr, false, help_wrap).y);
            if (r.id == id)
                help = text;
        }
        s.help_height = tallest;
    } else
        s.help_height = 0;
    if (s.command != Command::None) {
        auto cmd = s.command;
        s.command = Command::None;
        if (cmd == Command::Save && save())
            focus("reset");
    }
    if (!equal(before, s.draft) && dirty())
        s.saved_notice = false;
    {  // (the folder Save folder's or Game files folder's picker chose: the picker ran on the window's thread)
        char folder[sizeof s.draft.save_dir];
        int which = ui_take_picked_folder(folder, sizeof folder);
#ifdef __ANDROID__
        if (which == 4) SDL_strlcpy(s.draft.save_dir, folder, sizeof s.draft.save_dir);
        if (which == 7) SDL_strlcpy(s.draft.game_dir, folder, sizeof s.draft.game_dir);
#else
        Config temp = s.draft;  // (a folder whose path the settings file can't hold, with ';' or '#', is not taken)
        if (which == 4 && SDL_strlcpy(temp.save_location, folder, sizeof temp.save_location) < sizeof temp.save_location &&
            config_validate(&temp, nullptr, 0))
            s.draft = temp;
#endif
    }
    if (ui_action_failed())
        s.error = tr("Restart failed. The game is still running.");
    if (!s.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec(look.warn));
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
    } else if (s.saved_notice)
        ImGui::TextUnformatted(tr((config_restart_required(&s.draft) || mods_restart()) ? "Saved - restart pending" : "Saved"));
    if (s.view == View::Main) {
        if (s.page == 1 && (s.draft.fsr || g_config.fsr))
            ImGui::TextWrapped("%s", tr("FSR overrides MSAA and FXAA while active."));
        if (s.page == MODS_PAGE)
            ImGui::TextWrapped("%s", tr("Mods load from top to bottom: a later mod wins. Select a conflict to choose which mod is used. Press Save, then restart."));
        if (s.page == CHEATS_PAGE) {
            ImGui::PushStyleColor(ImGuiCol_Text, vec(theme().warn));
            ImGui::TextWrapped("%s", tr("Online: only use cheats when everyone in the lobby agrees to it."));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", tr("Cheats turn on and off when you press Save, no restart needed. Turning one off undoes its changes to the game's code; values it already changed (money, items) stay. Cheats added with Add a cheat work right away; cheat files added or edited in the folder show up the next time this menu opens."));
        }
        if (std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.cold; }))  // (the rows with " *")
            ImGui::TextUnformatted(tr("* Restart required"));
        // Keep scroll space for the pinned hints so the last action can scroll fully above them.
        ImGui::Dummy(ImVec2(1, guides.height + help_area + ImGui::GetStyle().ItemSpacing.y));
    }
    if (clipped_body)
        ImGui::PopClipRect();
    if (pinned_guides)
        ImGui::PopClipRect();
    if (pinned_guides && help_area > 0) {
        auto *draw = window->DrawList;
        float x0 = window->InnerRect.Min.x, x1 = window->InnerRect.Max.x, pad = ImGui::GetStyle().WindowPadding.x;
        draw->AddRectFilled(ImVec2(x0, help_top - spacing), ImVec2(x1, guide_top),
                            ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK);
        draw->AddLine(ImVec2(x0 + pad, help_top), ImVec2(x1 - pad, help_top), look.divider);
        if (!help.empty())
            draw_help(draw, ImVec2(x0 + pad, help_top + spacing), look.text_dim, help, help_wrap);
    }
    if (pinned_guides && s.view == View::Main)
        draw_guides(window, guides, guide_top);
    if (s.view != View::Main)
        s.header_bottom = ImGui::GetStyle().WindowPadding.y + ImGui::GetFontSize() * 1.5f;
    draw_frame(window, look);
    draw_grip(window, look);
    ImVec2 main_pos = window->Pos, main_size = window->Size;
    ImGui::End();
    if (bv_mode == 2) {
        draw_bind_window(ImVec2(main_pos.x + main_size.x + bv_gap, main_pos.y), bv_width, main_size.y, scale, margin, look);
        if (g_bv_pointing && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ImGui::SetWindowFocus("Settings##main");  // (a click on the drawing does not take the list's keyboard and controller)
    }
    if (s.dropdown.open && s.dropdown.anchored && !options.empty())
        draw_dropdown(options, scale, base);
    else
        s.dropdown.hovered = false;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    is_dirty = *open && (dirty() || pending_text());
    is_confirming = *open && confirmation();
    is_capturing = *open && s.view == View::KeyCapture;
    // (a left click on the capture view's buttons is theirs; any other button, or a left click anywhere else, is the
    // mouse button chosen)
    bool for_action = is_capturing && acts().capture >= 0;
    capture_mouse = is_capturing && (for_action ? acts().dev == 1 : s.capture_bind >= 0);
    capture_pointing = s.pointing || g_bv_pointing;
    capture_wheel = capture_mouse.load();  // (the capture view doesn't scroll)
    capture_pad = for_action && acts().dev == 0;
    if (!*open)
        s.editing = false;
}
