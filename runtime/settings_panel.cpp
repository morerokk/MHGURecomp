#include "settings_panel.h"
#include "config.h"
#include "menu_confirm.h"
#include "settings_locale.h"
#include "settings_icons.h"
#include "app_actions.h"
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
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

namespace {
enum class View { Main, Keyboard, CloseChanges, Exit, Restart, Reset, Move, Conflicts, KeyCapture, Keybinds };
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
    bool position_override = false;
    int capture_bind = -1;     // the keybind (BIND_*) a key is being chosen for; -1: the chat key
    bool capture_add = false;  // the key chosen is that keybind's second, else its only one
} s;
std::atomic<bool> request_exit, request_back, is_dirty, is_confirming, is_capturing, capture_mouse;
// (the key pressed while a key is being chosen, or KEY_MOUSE + a mouse button; -1: none yet)
std::atomic<int> captured_key{-1};
const char *tr(const char *text) { return settings_text(s.draft.language, text); }
std::string fmt(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    vsnprintf(buffer, sizeof buffer, tr(text), args);
    va_end(args);
    return buffer;
}
// The Mods tab: the mods folder's state (modorder.h), which is saved with the rest of the settings
constexpr int PAGES = 7, MODS_PAGE = 5;
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
struct GuideLayout {
    ImVec2 offset[3];
    float height = 0;
};
GuideLayout guide_layout(float width) {
    GuideLayout layout;
    float f = ImGui::GetFontSize(), x = 0, y = 0;
    const char *labels[] = {"Navigation", "Confirm", "Back"};
    for (int i = 0; i < 3; i++) {
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
void draw_guides(ImGuiWindow *window, const GuideLayout &layout, float top) {
    auto *draw = window->DrawList;
    ImVec2 start(window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x, top);
    ImU32 background = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK;
    draw->AddRectFilled(ImVec2(window->InnerRect.Min.x, top - ImGui::GetStyle().ItemSpacing.y),
                        ImVec2(window->InnerRect.Max.x, top + layout.height), background);
    const int icons[] = {SettingsIcons::Dpad, SettingsIcons::Confirm, SettingsIcons::Back};
    const char *labels[] = {"Navigation", "Confirm", "Back"};
    float f = ImGui::GetFontSize();
    for (int i = 0; i < 3; i++) {
        ImVec2 pos(start.x + layout.offset[i].x, start.y + layout.offset[i].y);
        SettingsIcons::draw(draw, pos, f, s.draft.button_icons, icons[i],
                            menu_confirm_position(config_live_menu_confirm(), overlay_face_position(0),
                                                  overlay_face_position(1), i == 2));
        draw->AddText(ImVec2(pos.x + f * 1.4f, pos.y), ImGui::GetColorU32(ImGuiCol_Text), tr(labels[i]));
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
    SAME(fsr_upscaler);
    SAME(fsr_quality);
    SAME(fsr_sharpness);
    SAME(fsr_objmotion);
    SAME(fsr_reactive);
    SAME(fsr_animsurf);
    SAME(vsync);
    SAME(shader_prewarm);
    SAME(draw_distance);
    SAME(msaa);
    SAME(fxaa);
    SAME(fxaa_mode);
    SAME(face_buttons);
    SAME(ldn_port);
    SAME(sync_small_monsters);
    SAME(bonus_content);
    SAME(village_in_hub);
    SAME(mouse_sensitivity);
    SAME(aspect_ratio);
    SAME(fov);
    SAME(ui_anchor);
    SAME(background_input);
    SAME(combo_window);
    SAME(volume);
    SAME(mute_in_background);
    SAME(button_icons);
    SAME(language);
    SAME(menu_scale);
    SAME(menu_confirm);
    SAME(confirm_exit);
    SAME(updates);
    SAME(hp.village);
    SAME(hp.quests);
#undef SAME
    if (memcmp(a.binds, b.binds, sizeof a.binds))
        return false;
    if (strcmp(a.ldn_join, b.ldn_join) || strcmp(a.ldn_server, b.ldn_server) || strcmp(a.ldn_key, b.ldn_key) ||
        strcmp(a.chat_key, b.chat_key))
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
bool dirty() { return !equal(s.draft, s.saved) || mods_dirty(); }
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
    mods.draft = mods.saved;
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
                                        "D-pad left", "D-pad right", "Left stick click", "Right stick click"};
// What else a key (or KEY_MOUSE + a mouse button) does: one of the recomp's own keys (keymap.h), a keybind other than
// skip, or (for a keybind's) the chat key; nullptr: nothing
const char *key_use(int code, int skip) {
    if (const char *use = code < KEY_MOUSE ? key_reserved(code) : nullptr)
        return tr(use);
    for (int i = 0; i < BINDS; i++)
        for (int k = 0; k < BIND_KEYS; k++)
            if (i != skip && s.draft.binds[i][k] == code)
                return tr(bind_titles[i]);
    if (skip >= 0 && code == (int)SDL_GetScancodeFromName(s.draft.chat_key))
        return tr("Chat key");
    return nullptr;
}
// Choosing a key: for a keybind (BIND_*, add: its second key) or the chat key (-1)
void capture(int bind, bool add) {
    s.capture_bind = bind;
    s.capture_add = add;
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
// The Keybinds submenu: a row per keybind (its keys; red where one does something else too), the chat key
void keybind_rows(std::vector<Row> &r) {
    for (int i = 0; i < BINDS; i++) {
        if (i == BIND_MOVE_UP)
            header(r, "bind.movement", tr("Movement"));
        if (i == BIND_A)
            header(r, "bind.buttons", tr("Buttons"));
        Row row;
        row.id = "bind." + std::to_string(i);
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
        r.push_back(std::move(row));
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
        c.fsr_upscaler = f.fsr_upscaler;
        c.fsr_quality = f.fsr_quality;
        c.fsr_sharpness = f.fsr_sharpness;
        c.fsr_objmotion = f.fsr_objmotion;
        c.fsr_reactive = f.fsr_reactive;
        c.fsr_animsurf = f.fsr_animsurf;
        c.fps = f.fps;
        c.vsync = f.vsync;
        c.shader_prewarm = f.shader_prewarm;
        c.draw_distance = f.draw_distance;
        c.msaa = f.msaa;
        c.fxaa = f.fxaa;
        c.fxaa_mode = f.fxaa_mode;
        c.volume = f.volume;
        c.mute_in_background = f.mute_in_background;
        break;
    case 2:
        c.face_buttons = f.face_buttons;
        c.menu_confirm = f.menu_confirm;
        c.button_icons = f.button_icons;
        c.background_input = f.background_input;
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
        c.sync_small_monsters = f.sync_small_monsters;
        c.village_in_hub = f.village_in_hub;
        break;
    case 4:
        c.hp = f.hp;
        quest_keys();
        break;
    case MODS_PAGE:
        for (auto &m : mods.draft)
            m.on = m.name[0] != '_';
        std::sort(mods.draft.begin(), mods.draft.end(),
                  [](const ModEntry &a, const ModEntry &b) { return strcasecmp(a.name, b.name) < 0; });
        break;
    case 6:
        c.language = f.language;
        c.menu_scale = f.menu_scale;
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
            number(r, "display.width", "Width", &c.width, 320, 16384, 80, ini("Resolution"));
            number(r, "display.height", "Height", &c.height, 180, 16384, 45, ini("Resolution"));
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
        number(r, "volume", "Volume (%)", &c.volume, (int)lo("Volume"), (int)hi("Volume"), 1, ini("Volume"));
        add_bool(r, "mutebg", "Mute in background", &c.mute_in_background, ini("MuteInBackground"));
        add_bool(r, "fsr", "FSR", &c.fsr, ini("FSR"));
        if (c.fsr) {
            add_choice(
                r, "fsr.provider", "Upscaler", c.fsr_upscaler, {"FSR 3.1", "FSR 4"},
                [](int v) { s.draft.fsr_upscaler = v; }, ini("Upscaler"));
            add_choice(
                r, "fsr.quality", "FSR quality", c.fsr_quality,
                {tr("Native AA"), tr("Quality"), tr("Balanced"), tr("Performance"), tr("Ultra performance")},
                [](int v) { s.draft.fsr_quality = v; }, ini("FSRQuality"));
            number(r, "fsr.sharpness", tr("Sharpness"), &c.fsr_sharpness, lo("Sharpness"), hi("Sharpness"), .05f, ini("Sharpness"));
            add_bool(r, "fsr.motion", "Object motion", &c.fsr_objmotion, ini("ObjectMotion"));
            number(r, "fsr.reactive", tr("Reactive mask"), &c.fsr_reactive, lo("Reactive"), hi("Reactive"), .05f, ini("Reactive"));
            add_bool(r, "fsr.surfaces", "Animated surfaces", &c.fsr_animsurf, ini("AnimSurf"));
        }
        break;
    case 2:
        add_choice(r, "face", "Face buttons", c.face_buttons, {tr("Position"), tr("Label")},
                   [](int v) { c.face_buttons = v; }, ini("FaceButtons"));
        add_choice(r, "menu.confirm", "Confirm button (recomp)", c.menu_confirm,
                   {tr("Original"), tr("Bottom button"), tr("Right button")},
                   [](int v) { s.draft.menu_confirm = v; }, ini("MenuConfirm"));
        add_choice(r, "icons", "Button icons", c.button_icons, {"Xbox", "Switch", "PlayStation"},
                   [](int v) { c.button_icons = v; }, ini("ButtonIcons"));
        add_bool(r, "background", "Background input", &c.background_input, ini("BackgroundInput"));
        add_choice(r, "combo", "Combo input window", c.combo_window, {tr("Lenient"), tr("Strict")},
                   [](int v) { c.combo_window = v; }, ini("ComboInputWindow"));
        number(r, "mouse", tr("Mouse sensitivity"), &c.mouse_sensitivity, lo("MouseSensitivity"), hi("MouseSensitivity"), .05f, ini("MouseSensitivity"));
        action(r, "keybinds", tr("Keybinds"), []() { push(View::Keybinds, "bind.0"); });
        r.back().help = tr("The keys and mouse buttons for the Switch controller's buttons and for moving, and the chat key.");
        break;
    case 3:
        number(r, "port", "Host port (TCP / UDP)", &c.ldn_port, (int)lo("Port"), (int)hi("Port"), 1, ini("Port"));
        text(r, "hosts", "Search hosts", c.ldn_join, sizeof c.ldn_join, *c.ldn_join ? c.ldn_join : tr("LAN"), ini("Join"));
        text(r, "server", "Hub server", c.ldn_server, sizeof c.ldn_server,
             !*c.ldn_server ? std::string(tr("Default")) + (*LDN_DEFAULT_SERVER ? " (" LDN_DEFAULT_SERVER ")" : "")
                            : c.ldn_server,
             ini("Server"));
        text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public"), ini("Key"));
        hub_check(r);
        add_bool(r, "sync", "Sync small monsters", &c.sync_small_monsters, ini("SyncSmallMonsters"));
        add_bool(r, "village", "Village quests in Hub", &c.village_in_hub, ini("VillageQuestsInHub"));
        break;
    case 4: {
        const char *per_quest =
            tr("HP multipliers for single quests, by their quest ID: in those quests they are used instead of the ones above.");
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
    case 6:
        add_choice(r, "language", "Language", c.language, {"English", "Español"},
                   [](int v) { c.language = v; }, ini("Language"));
        number(r, "scale", "Interface scale (%)", &c.menu_scale, (int)lo("MenuScale"), (int)hi("MenuScale"), 10, ini("MenuScale"));
        add_bool(r, "confirm.exit", "Confirm exit", &c.confirm_exit, ini("ConfirmExit"));
        add_choice(r, "updates", "Updates", c.updates, {tr("Ask"), tr("Auto"), tr("Off")},
                   [](int v) { c.updates = v; }, ini("Updates"));
        add_bool(r, "bonus", "Bonus content", &c.bonus_content, ini("BonusContent"));
        action(r, "detach", tr(s.floating ? "Attach menu" : "Detach menu"),
               []() { s.floating = !s.floating; });
        if (s.floating)
            action(r, "move", tr("Move menu"), []() {
                auto *w = ImGui::GetCurrentWindow();
                s.move_start = s.move_position = w->Pos;
                push(View::Move, "move.accept");
            });
        action(r, "restart", tr("Restart"), []() { push(View::Restart, "dialog.cancel"); });
        action(r, "exit", tr("Exit game"), []() { push(View::Exit, "dialog.cancel"); });
        break;
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
    s.saved = s.draft;
    s.saved_notice = true;
    s.error.clear();
    return true;
}
std::vector<Row> rows_for_view(bool *open) {
    if (s.view == View::Main)
        return main_rows();
    std::vector<Row> r;
    if (s.view == View::Keyboard) {
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
    } else if (s.view == View::KeyCapture) {
        int key = captured_key.exchange(-1);
        const char *name = key > 0 && key < KEY_MOUSE ? SDL_GetScancodeName((SDL_Scancode)key) : "";
        const char *reserved = key > 0 && key < KEY_MOUSE ? key_reserved(key) : nullptr;
        if (key == SDL_SCANCODE_ESCAPE)
            pop();
        else if (s.capture_bind >= 0 && key > 0) {
            int *keys = s.draft.binds[s.capture_bind];
            if (reserved)
                s.error = fmt("That key is the recomp's own (%s).", tr(reserved));
            else if (!*config_key_name(key))
                s.error = tr("That key can't be used.");
            else {
                if (!s.capture_add)
                    keys[0] = key, keys[1] = 0;
                else if (keys[0] != key)
                    keys[1] = key;
                pop();
            }
        } else if (key > 0 && *name && strlen(name) < sizeof s.draft.chat_key && !strpbrk(name, ";#")) {
            SDL_strlcpy(s.draft.chat_key, name, sizeof s.draft.chat_key);
            pop();
        } else if (key > 0)
            s.error = tr("That key can't be used for chat.");
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
                                 IM_COL32(111, 216, 255, 255), r.title.c_str());
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
        ImGui::PushStyleColor(ImGuiCol_Button,
                              selected ? ImVec4(.16f, .32f, .46f, 1) : ImVec4(.09f, .13f, .19f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.15f, .28f, .39f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.18f, .36f, .49f, 1));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::BeginDisabled(!r.enabled);
        bool clicked = ImGui::Button("###row", ImVec2(rw, h));
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        if (!list && ImGui::IsItemHovered())
            s.hovered = r.id;
        if (!list && ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))
            s.pointing = true;
        if (selected && r.enabled) {
            if (!passive)
                own_focus(w, id, rect);
            w->DrawList->AddRect(rect.Min, rect.Max, IM_COL32(111, 216, 255, 255), 3.f, 2.f, 0);
        }
        w->DrawList->PushClipRect(rect.Min, rect.Max, true);
        ImU32 ink = r.enabled ? IM_COL32(244, 247, 252, 255) : IM_COL32(132, 145, 160, 255);
        ImU32 value_ink = r.warn ? IM_COL32(255, 110, 96, 255) : ink;
        if (r.current)  // (the dropdown's choice now: a bar on its left)
            w->DrawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + font * .22f, rect.Max.y), IM_COL32(111, 216, 255, 255));
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.05f, .08f, .12f, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.43f, .85f, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::Begin("Settings##dropdown", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(scale);
    draw_rows(options, true);
    s.dropdown.hovered = ImGui::IsWindowHovered();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}
} // namespace
void settings_panel_request_close() { request_exit.store(true); }
void settings_panel_request_back() { request_back.store(true); }
bool settings_panel_dirty() { return is_dirty.load(); }
bool settings_panel_confirming() { return is_confirming.load() || request_exit.load(); }
bool settings_panel_key(int scancode) {
    if (!is_capturing.load())
        return false;
    captured_key = scancode;
    return true;
}
bool settings_panel_mouse(int button) {
    if (!capture_mouse.load() || button < 1 || button > SDL_BUTTON_X2)
        return false;
    captured_key = KEY_MOUSE + button;
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
        return;
    }
    if (!s.editing) {
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
    }
    if (request_exit.exchange(false) && s.view != View::Exit)
        push(View::Exit, "dialog.cancel");
    Config before = s.draft;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = s.draft.menu_scale / 100.f;
    float base = ImGui::GetFontSize();
    float margin = std::min(16.f, vp->WorkSize.x * .025f);
    float width = std::min(vp->WorkSize.x - 2 * margin, base * scale * 56);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->WorkSize.y - 2 * margin));
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
    if (!s.floating)
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * .5f, vp->WorkPos.y + margin), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.025f, .04f, .065f, .98f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.96f, .97f, 1, 1));
    ImGui::Begin("Settings##main", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                     (s.floating ? 0 : ImGuiWindowFlags_NoMove));
    ImGui::SetWindowFontScale(scale);
    if (s.restore_scroll) {
        ImGui::SetScrollY(s.scroll);
        s.restore_scroll = false;
    }
    if (request_back.exchange(false) || pressed(ImGuiKey_GamepadFaceRight) || pressed(ImGuiKey_Escape))
        ask_back(open);
    if (s.dropdown.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !s.dropdown.hovered)
        close_dropdown();  // (a click elsewhere: on a row, it acts as it does with no dropdown open)
    bool pinned_guides = s.view == View::Main;
    bool clipped_body = false;
    auto *window = ImGui::GetCurrentWindow();
    float spacing = ImGui::GetStyle().ItemSpacing.y;
    GuideLayout guides = guide_layout(ImGui::GetContentRegionAvail().x);
    float guide_top =
        std::max(window->Pos.y + window->Size.y - ImGui::GetStyle().WindowPadding.y - guides.height,
                 window->InnerClipRect.Min.y);
    // pinned above the guides: the help of the row focused or pointed at, with room for the longest of the tab (at most
    // half of what is above the guides, so a small window at a large menu size keeps room for the rows)
    float help_area = pinned_guides && s.help_height > 0 ? s.help_height + spacing * 2 : 0;
    help_area = std::min(help_area, (guide_top - window->InnerClipRect.Min.y) / 2);
    s.guide_height = pinned_guides ? guides.height + help_area : 0;
    float help_top = std::max(guide_top - help_area, window->InnerClipRect.Min.y);
    if (pinned_guides)
        ImGui::PushClipRect(
            window->InnerClipRect.Min,
            ImVec2(window->InnerClipRect.Max.x, std::max(window->InnerClipRect.Min.y, help_top - spacing)), true);
    if (s.view == View::Main) {
        if (pressed(ImGuiKey_GamepadR1))
            first_page((s.page + 1) % PAGES);
        if (pressed(ImGuiKey_GamepadL1))
            first_page((s.page + PAGES - 1) % PAGES);
        const char *pages[] = {"Display", "Graphics & Audio", "Controls", "Network", "Miscellaneous", "Mods", "App"};
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
        header->AddRectFilled(
            ImVec2(window->InnerRect.Min.x, window->InnerRect.Min.y),
            ImVec2(window->InnerRect.Max.x, tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y),
            ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK);
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
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  active ? ImVec4(.18f, .4f, .56f, 1) : ImVec4(.06f, .09f, .14f, 1));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button("###tab", ImVec2(tabw, tabh)))
                first_page(i);
            ImGui::PopItemFlag();
            // A label wraps at its spaces; one word wider than the tab ("Miscellaneous") is drawn smaller instead of
            // being cut in two
            const char *name = tr(pages[i]);
            float room = tabw - fw * .5f, size = fw;
            float word = ImGui::CalcTextSize(name).x;
            if (!strchr(name, ' ') && word > room) size = fw * room / word;
            auto label = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, room, name);
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size,
                                                ImVec2(p.x + fw * .25f, p.y + (tabh - label.y) * .5f),
                                                IM_COL32(244, 247, 252, 255), name, nullptr, room);
            if (active)
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + tabh - 3),
                                                          ImVec2(p.x + tabw, p.y + tabh),
                                                          IM_COL32(111, 216, 255, 255));
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(
            ImVec2(scrolling_start.x, scrolling_start.y + header_height + ImGui::GetStyle().ItemSpacing.y));
        float body_top = tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y;
        ImGui::PushClipRect(ImVec2(window->ClipRect.Min.x, std::min(body_top, window->ClipRect.Max.y)),
                            window->ClipRect.Max, true);
        clipped_body = true;
    } else {
        std::string title = s.view == View::Keyboard ? s.keyboard.title
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
        if (io.KeyCtrl && pressed(ImGuiKey_V)) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                insert(clip);
                SDL_free(clip);
            }
        }
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
    auto rows = rows_for_view(open);
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
    if (ui_action_failed())
        s.error = tr("Restart failed. The game is still running.");
    if (!s.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .72f, .57f, 1));
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
    } else if (s.saved_notice)
        ImGui::TextUnformatted(tr((config_restart_required(&s.draft) || mods_restart()) ? "Saved - restart pending" : "Saved"));
    if (s.view == View::Main) {
        if (s.page == 1 && (s.draft.fsr || g_config.fsr))
            ImGui::TextWrapped("%s", tr("FSR overrides MSAA and FXAA while active."));
        if (s.page == MODS_PAGE)
            ImGui::TextWrapped("%s", tr("Mods load from top to bottom: a later mod wins. Select a conflict to choose which mod is used. Press Save, then restart."));
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
        draw->AddLine(ImVec2(x0 + pad, help_top), ImVec2(x1 - pad, help_top), IM_COL32(52, 72, 94, 255));
        if (!help.empty()) {
            draw->PushClipRect(ImVec2(x0 + pad, help_top), ImVec2(x1, guide_top), true);  // (a help cut to its room)
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0 + pad, help_top + spacing),
                          IM_COL32(178, 192, 208, 255), help.c_str(), nullptr, help_wrap);
            draw->PopClipRect();
        }
    }
    if (pinned_guides && s.view == View::Main)
        draw_guides(window, guides, guide_top);
    ImGui::End();
    if (s.dropdown.open && s.dropdown.anchored && !options.empty())
        draw_dropdown(options, scale, base);
    else
        s.dropdown.hovered = false;
    ImGui::PopStyleColor(2);
    is_dirty = *open && (dirty() || pending_text());
    is_confirming = *open && confirmation();
    is_capturing = *open && s.view == View::KeyCapture;
    // (a click on the capture view's buttons is theirs; anywhere else it is the mouse button chosen)
    capture_mouse = is_capturing && s.capture_bind >= 0 && !s.pointing;
    if (!*open)
        s.editing = false;
}
