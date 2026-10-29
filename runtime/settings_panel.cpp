#include "settings_panel.h"
#include "config.h"
#include "menu_confirm.h"
#include "settings_locale.h"
#include "settings_icons.h"
#include "app_actions.h"
#include "overlay.h"
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
enum class View { Main, Choices, Keyboard, CloseChanges, Exit, Restart, Reset, Move, Conflicts };
enum class Command { None, Save };
struct Row {
    std::string id, title, value;
    bool enabled = true, cold = false, action = false;
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
struct Choice {
    std::vector<std::string> names;
    int selected;
    std::function<void(int)> set;
    std::string title;
    bool preview = false;  // a setting's values: the highlighted one is live while the list is open (quick apply)
};
struct Keyboard {
    std::string title, original, buffer;
    size_t cursor = 0, capacity = 255;
    bool upper = false, select_all = false;
    std::function<bool(const std::string &, std::string &)> commit;
};
struct State {
    Config draft = {}, saved = {}, previewed = {};  // previewed: what quick apply last made live
    bool editing = false, floating = false, advanced = false, focus_request = true, saved_notice = false;
    int page = 0;
    View view = View::Main;
    Command command = Command::None;
    std::string focus, error;
    std::vector<Target> targets;
    std::vector<Origin> origins;
    Choice choice;
    Keyboard keyboard;
    uint64_t quest_keys[HP_QUESTS_MAX] = {}, next_quest_key = 1;
    bool restore_scroll = false;
    float scroll = 0, guide_height = 0;
    int focus_scroll_frames = 0;
    ImVec2 move_start = {}, move_position = {};
    bool position_override = false;
} s;
std::atomic<bool> request_exit, request_back, is_dirty, is_confirming;
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
    SAME(aa);
    SAME(smaa_quality);
    SAME(cmaa2_quality);
    SAME(cmaa2_extra_sharpness);
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
    if (strcmp(a.ldn_join, b.ldn_join) || strcmp(a.ldn_server, b.ldn_server) || strcmp(a.ldn_key, b.ldn_key))
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
void push(View view, const std::string &first) {
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
    if (s.view != View::Main) {
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
void choices(const Row &row, std::vector<std::string> names, int selected, std::function<void(int)> set,
             bool preview = false) {
    s.choice = {std::move(names), selected, std::move(set), row.title, preview};
    push(View::Choices, "choice." + std::to_string(selected));
}
// Quick apply: the draft with the highlighted value of an open list of a setting's values in it (else the draft).
Config live_draft() {
    if (s.view != View::Choices || !s.choice.preview || s.focus.compare(0, 7, "choice.") ||
        !isdigit((unsigned char)s.focus[7]))
        return s.draft;
    int i = atoi(s.focus.c_str() + 7);
    if (i < 0 || i >= (int)s.choice.names.size())
        return s.draft;
    Config keep = s.draft;
    s.choice.set(i);
    Config live = s.draft;
    s.draft = keep;
    return live;
}
// What the settings table (config.c g_settings) says of a setting: does it need a restart, a number's range
bool cold(const char *key) { return config_restart_key(key); }
float lo(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return a; }
float hi(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return b; }
void add_bool(std::vector<Row> &rows, const char *id, const char *name, int *value, bool cold = false) {
    Row r;
    r.id = id;
    r.title = tr(name);
    r.cold = cold;
    r.value = tr(*value ? "On" : "Off");
    r.adjust = [value](int d) { *value = d > 0 ? 1 : 0; };
    r.activate = [value]() { *value = !*value; };
    rows.push_back(std::move(r));
}
void add_choice(std::vector<Row> &rows, const char *id, const char *name, int selected,
                const std::vector<std::string> &names, std::function<void(int)> set, bool cold = false) {
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = names[std::clamp(selected, 0, (int)names.size() - 1)];
    r.cold = cold;
    r.adjust = [selected, names, set](int d) { set((selected + d + (int)names.size()) % (int)names.size()); };
    Row copy = r;
    r.activate = [copy, names, selected, set]() { choices(copy, names, selected, set, true); };
    rows.push_back(std::move(r));
}
void add_number(std::vector<Row> &rows, const std::string &id, const std::string &name,
                std::function<double()> get, std::function<void(double)> set, double low, double high,
                double step, bool integral, bool cold = false,
                std::function<bool(double, std::string &)> check = {}) {
    char text[64];
    snprintf(text, sizeof text, integral ? "%.0f" : "%.2f", get());
    Row r;
    r.id = id;
    r.title = name;
    r.value = text;
    r.cold = cold;
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
            bool cold = false) {
    add_number(
        r, id, tr(name), [p]() { return (double)*p; }, [p](double v) { *p = (int)v; }, low, high, step, true,
        cold);
}
void number(std::vector<Row> &r, const std::string &id, const std::string &name, float *p, float low,
            float high, float step, bool cold = false) {
    add_number(
        r, id, name, [p]() { return (double)*p; }, [p](double v) { *p = (float)v; }, low, high, step, false,
        cold);
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
          bool cold) {
    size_t at = (size_t)(field - (char *)&s.draft);
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = shown;
    r.cold = cold;
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
    r.value = state;
    r.activate = []() { ldn_hub_check(s.draft.ldn_server, s.draft.ldn_key); };
    rows.push_back(std::move(r));
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
        c.aa = f.aa;
        c.smaa_quality = f.smaa_quality;
        c.cmaa2_quality = f.cmaa2_quality;
        c.cmaa2_extra_sharpness = f.cmaa2_extra_sharpness;
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
        row.activate = [copy, name, on, set_on]() {
            auto move = [name](int d) {
                int at = mod_index(name), to = at + d;
                if (at >= 0 && to >= 0 && to < (int)mods.draft.size()) {
                    std::swap(mods.draft[at], mods.draft[to]);
                    s.saved_notice = false;
                }
            };
            choices(copy, {tr(on ? "Turn off" : "Turn on"), tr("Move up"), tr("Move down")}, 0,
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
            {tr("Windowed"), tr("Fullscreen"), tr("Borderless")}, [](int v) { c.display_mode = v; }, cold("Mode"));
        add_choice(
            r, "display.auto", "Screen resolution", !c.width ? 0 : 1, {tr("Desktop"), tr("Custom")},
            [](int v) {
                s.draft.width = v ? 1920 : 0;
                s.draft.height = v ? 1080 : 0;
            },
            cold("Resolution"));
        if (c.width) {
            number(r, "display.width", "Width", &c.width, 320, 16384, 80, cold("Resolution"));
            number(r, "display.height", "Height", &c.height, 180, 16384, 45, cold("Resolution"));
        }
        add_choice(
            r, "render.auto", "Render resolution", !c.render_w ? 0 : 1, {tr("Auto"), tr("Custom")},
            [](int v) {
                s.draft.render_w = v ? 1920 : 0;
                s.draft.render_h = v ? 1080 : 0;
            },
            cold("RenderResolution"));
        if (c.render_w) {
            number(r, "render.width", "Width", &c.render_w, 320, 16384, 80, cold("RenderResolution"));
            number(r, "render.height", "Height", &c.render_h, 180, 16384, 45, cold("RenderResolution"));
        }
        add_choice(
            r, "aspect.auto", "Automatic aspect ratio", c.aspect_ratio == 0 ? 0 : 1,
            {tr("Auto"), tr("Custom")}, [](int v) { s.draft.aspect_ratio = v ? 16.f / 9 : 0; }, cold("AspectRatio"));
        if (c.aspect_ratio)
            number(r, "aspect", tr("Aspect ratio"), &c.aspect_ratio, lo("AspectRatio"), hi("AspectRatio"), .05f, cold("AspectRatio"));
        number(r, "fov", tr("Field of view"), &c.fov, lo("FOV"), hi("FOV"), .05f, cold("FOV"));
        add_choice(
            r, "anchor", "HUD anchor", c.ui_anchor - 1, {"16:9", "21:9", "32:9", tr("Edges")},
            [](int v) { c.ui_anchor = v + 1; }, cold("UIAnchor"));
        break;
    case 1:
        number(r, "fps", "Frame rate limit", &c.fps, (int)lo("FPS"), (int)hi("FPS"), 1, cold("FPS"));
        add_bool(r, "vsync", "VSync", &c.vsync, cold("VSync"));
        add_bool(r, "prewarm", "Shader prewarm", &c.shader_prewarm, cold("ShaderPrewarm"));
        add_choice(r, "msaa", "MSAA",
                   c.msaa == 8   ? 3
                   : c.msaa == 4 ? 2
                   : c.msaa == 2 ? 1
                                 : 0,
                   {tr("Off"), "2x", "4x", "8x"}, [](int v) { c.msaa = 1 << v; });
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        if (AA_USES_S2X(c.aa) && !c.fsr) {  // (SMAA S2x / 4x render with MSAA 2x: config_live_msaa)
            r.back().enabled = false;
            r.back().value = c.aa == AA_SMAA_S2X ? "2x (SMAA S2x)" : "2x (SMAA 4x)";
        }
        add_choice(r, "aa", "Anti-aliasing", c.aa,
                   {tr("Off"), tr("Game"), "FXAA", "CMAA2", "SMAA", "SMAA S2x", "SMAA T2x", "SMAA 4x"}, [](int v) { c.aa = v; });
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        if (c.aa >= AA_SMAA && !c.fsr)
            add_choice(r, "smaa", "SMAA quality", c.smaa_quality, {tr("Low"), tr("Medium"), tr("High"), tr("Ultra")},
                       [](int v) { c.smaa_quality = v; });
        if (c.aa == AA_CMAA2 && !c.fsr) {
            add_choice(r, "cmaa2", "CMAA2 quality", c.cmaa2_quality, {tr("Low"), tr("Medium"), tr("High"), tr("Ultra")},
                       [](int v) { c.cmaa2_quality = v; });
            add_bool(r, "cmaa2sharp", "Extra sharpness", &c.cmaa2_extra_sharpness, false);
        }
        number(r, "drawdist", tr("Draw distance"), &c.draw_distance, lo("DrawDistance"), hi("DrawDistance"), .25f,
               cold("DrawDistance"));
        if (c.draw_distance == 0)
            r.back().value = tr("No limit");
        number(r, "volume", "Volume (%)", &c.volume, (int)lo("Volume"), (int)hi("Volume"), 1, cold("Volume"));
        add_bool(r, "mutebg", "Mute in background", &c.mute_in_background, cold("MuteInBackground"));
        add_bool(r, "fsr", "FSR", &c.fsr, cold("FSR"));
        if (c.fsr) {
            add_choice(
                r, "fsr.provider", "Upscaler", c.fsr_upscaler, {"FSR 3.1", "FSR 4"},
                [](int v) { s.draft.fsr_upscaler = v; }, cold("Upscaler"));
            add_choice(
                r, "fsr.quality", "FSR quality", c.fsr_quality,
                {tr("Native AA"), tr("Quality"), tr("Balanced"), tr("Performance"), tr("Ultra performance")},
                [](int v) { s.draft.fsr_quality = v; }, cold("FSRQuality"));
            number(r, "fsr.sharpness", tr("Sharpness"), &c.fsr_sharpness, lo("Sharpness"), hi("Sharpness"), .05f, cold("Sharpness"));
            add_bool(r, "fsr.motion", "Object motion", &c.fsr_objmotion, cold("ObjectMotion"));
            number(r, "fsr.reactive", tr("Reactive mask"), &c.fsr_reactive, lo("Reactive"), hi("Reactive"), .05f, cold("Reactive"));
            add_bool(r, "fsr.surfaces", "Animated surfaces", &c.fsr_animsurf, cold("AnimSurf"));
        }
        break;
    case 2:
        add_choice(r, "face", "Face buttons", c.face_buttons, {tr("Position"), tr("Label")},
                   [](int v) { c.face_buttons = v; }, cold("FaceButtons"));
        add_choice(r, "menu.confirm", "Confirm button (recomp)", c.menu_confirm,
                   {tr("Original"), tr("Bottom button"), tr("Right button")},
                   [](int v) { s.draft.menu_confirm = v; }, cold("MenuConfirm"));
        add_choice(r, "icons", "Button icons", c.button_icons, {"Xbox", "Switch", "PlayStation"},
                   [](int v) { c.button_icons = v; }, cold("ButtonIcons"));
        add_bool(r, "background", "Background input", &c.background_input, cold("BackgroundInput"));
        add_choice(r, "combo", "Combo input window", c.combo_window, {tr("Lenient"), tr("Strict")},
                   [](int v) { c.combo_window = v; }, cold("ComboInputWindow"));
        number(r, "mouse", tr("Mouse sensitivity"), &c.mouse_sensitivity, lo("MouseSensitivity"), hi("MouseSensitivity"), .05f, cold("MouseSensitivity"));
        break;
    case 3:
        number(r, "port", "Host port (TCP / UDP)", &c.ldn_port, (int)lo("Port"), (int)hi("Port"), 1, cold("Port"));
        text(r, "hosts", "Search hosts", c.ldn_join, sizeof c.ldn_join, *c.ldn_join ? c.ldn_join : tr("LAN"), cold("Join"));
        text(r, "server", "Hub server", c.ldn_server, sizeof c.ldn_server,
             !*c.ldn_server ? std::string(tr("Default")) + (*LDN_DEFAULT_SERVER ? " (" LDN_DEFAULT_SERVER ")" : "")
                            : c.ldn_server,
             cold("Server"));
        text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public"), cold("Key"));
        hub_check(r);
        add_bool(r, "sync", "Sync small monsters", &c.sync_small_monsters, cold("SyncSmallMonsters"));
        add_bool(r, "village", "Village quests in Hub", &c.village_in_hub, cold("VillageQuestsInHub"));
        break;
    case 4:
        for (int i = 0; i < 4; i++)
            number(r, "hp." + std::to_string(i), std::to_string(i + 1) + " " + tr(i == 0 ? "Hunter" : "Hunters"), &c.hp.scale[i],
                   lo("1Player"), hi("1Player"), .05f, cold("1Player"));
        number(r, "hp.village", tr("Village HP multiplier"), &c.hp.village, lo("HubHP"), hi("HubHP"), .05f,
               cold("HubHP"));
        action(r, "hp.advanced", tr("Per-quest overrides"), []() { s.advanced = !s.advanced; });
        if (s.advanced) {
            for (int i = 0; i < c.hp.quests; i++) {
                auto id = quest_id(i);
                add_number(
                    r, id, tr("Quest ID"), [i]() { return (double)s.draft.hp.quest[i].quest; },
                    [i](double v) { s.draft.hp.quest[i].quest = (uint32_t)v; }, 1, 4294967295., 1, true, true,
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
                           &c.hp.quest[i].scale[n], HP_SCALE_MIN, HP_SCALE_MAX, .05f, true);
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
        break;
    case MODS_PAGE:
        mods_rows(r);
        break;
    case 6:
        add_choice(r, "language", "Language", c.language, {"English", "Español"},
                   [](int v) { c.language = v; }, cold("Language"));
        number(r, "scale", "Interface scale (%)", &c.menu_scale, (int)lo("MenuScale"), (int)hi("MenuScale"), 10, cold("MenuScale"));
        add_bool(r, "confirm.exit", "Confirm exit", &c.confirm_exit, cold("ConfirmExit"));
        add_choice(r, "updates", "Updates", c.updates, {tr("Ask"), tr("Auto"), tr("Off")},
                   [](int v) { c.updates = v; }, cold("Updates"));
        add_bool(r, "bonus", "Bonus content", &c.bonus_content, cold("BonusContent"));
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
    if (s.view == View::Choices) {
        for (int i = 0; i < (int)s.choice.names.size(); i++)
            action(r, "choice." + std::to_string(i), s.choice.names[i], [i]() {
                s.choice.set(i);
                pop();
            });
        action(r, "choice.cancel", tr("Cancel"), []() { pop(); });
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
bool pressed(ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); }
bool direction(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return pressed(a, true) || pressed(b, true) || pressed(c, true);
}
void navigate(std::vector<Row> &rows) {
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
    if (pressed(ImGuiKey_Home))
        focus(rows.front().id);
    if (pressed(ImGuiKey_End))
        focus(rows.back().id);
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
void draw_rows(std::vector<Row> &rows) {
    auto *w = ImGui::GetCurrentWindow();
    float font = ImGui::GetFontSize(), width = ImGui::GetContentRegionAvail().x;
    bool keyboard_view = s.view == View::Keyboard;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float cell = (width - 9 * gap) / 10;
    s.targets.clear();
    std::function<void()> activate;
    for (int i = 0; i < (int)rows.size(); i++) {
        Row &r = rows[i];
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
        bool selected = s.focus == r.id;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              selected ? ImVec4(.16f, .32f, .46f, 1) : ImVec4(.09f, .13f, .19f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.15f, .28f, .39f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.18f, .36f, .49f, 1));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::BeginDisabled(!r.enabled);
        bool clicked = ImGui::Button("###row", ImVec2(rw, h));
        ImGui::EndDisabled();
        // In a list of values the mouse highlights what it moves over, so quick apply shows it before a click
        ImVec2 moved = ImGui::GetIO().MouseDelta;
        if (s.view == View::Choices && r.enabled && !selected && (moved.x || moved.y) && ImGui::IsItemHovered())
            focus(r.id);
        ImGui::PopStyleColor(4);
        if (selected && r.enabled) {
            own_focus(w, id, rect);
            w->DrawList->AddRect(rect.Min, rect.Max, IM_COL32(111, 216, 255, 255), 3.f, 2.f, 0);
        }
        w->DrawList->PushClipRect(rect.Min, rect.Max, true);
        ImU32 ink = r.enabled ? IM_COL32(244, 247, 252, 255) : IM_COL32(132, 145, 160, 255);
        w->DrawList->AddText(ImGui::GetFont(), font, ImVec2(p.x + font * .5f, p.y + (h - label.y) * .5f), ink,
                             r.title.c_str(), nullptr, wrap);
        if (!r.value.empty()) {
            std::string value = r.value + (r.cold ? " *" : "");
            float available = rw * .38f - font;
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
            w->DrawList->AddText(ImVec2(p.x + rw - font * .5f - sz.x, p.y + (h - font) * .5f), ink,
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
} // namespace
void settings_panel_request_close() { request_exit.store(true); }
void settings_panel_request_back() { request_back.store(true); }
bool settings_panel_dirty() { return is_dirty.load(); }
bool settings_panel_confirming() { return is_confirming.load() || request_exit.load(); }
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
        if (s.editing)
            config_preview_end();
        s.editing = false;
        is_dirty = false;
        is_confirming = false;
        return;
    }
    if (!s.editing) {
        s.draft = s.saved = s.previewed = config_editable();
        s.editing = true;
        s.view = View::Main;
        s.origins.clear();
        s.focus.clear();
        s.focus_request = true;
        s.error.clear();
        s.saved_notice = false;
        s.floating = false;
        s.advanced = false;
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
    bool pinned_guides = s.view == View::Main;
    bool clipped_body = false;
    auto *window = ImGui::GetCurrentWindow();
    GuideLayout guides = guide_layout(ImGui::GetContentRegionAvail().x);
    s.guide_height = pinned_guides ? guides.height : 0;
    float guide_top =
        std::max(window->Pos.y + window->Size.y - ImGui::GetStyle().WindowPadding.y - guides.height,
                 window->InnerClipRect.Min.y);
    if (pinned_guides)
        ImGui::PushClipRect(
            window->InnerClipRect.Min,
            ImVec2(window->InnerClipRect.Max.x,
                   std::max(window->InnerClipRect.Min.y, guide_top - ImGui::GetStyle().ItemSpacing.y)),
            true);
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
        const char *title = s.view == View::Choices    ? s.choice.title.c_str()
                            : s.view == View::Keyboard ? s.keyboard.title.c_str()
                                                       : tr(s.view == View::Exit      ? "Exit game?"
                                                            : s.view == View::Restart ? "Restart game?"
                                                            : s.view == View::Reset   ? "Reset this tab?"
                                                            : s.view == View::Move    ? "Move menu"
                                                            : s.view == View::Conflicts ? "Mod conflicts"
                                                                                      : "Unsaved changes");
        ImGui::TextWrapped("%s", title);
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
    if (s.view != View::Move)
        navigate(rows);
    else
        focus("move.accept");
    draw_rows(rows);
    if (s.command != Command::None) {
        auto cmd = s.command;
        s.command = Command::None;
        if (cmd == Command::Save && save())
            focus("reset");
    }
    // Quick apply: the hot settings show at once, in an open list of values the highlighted one; Save keeps them
    Config live = live_draft();
    if (!equal(live, s.previewed)) {
        config_preview(&live);
        s.previewed = live;
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
        ImGui::Dummy(ImVec2(1, guides.height + ImGui::GetStyle().ItemSpacing.y));
    }
    if (clipped_body)
        ImGui::PopClipRect();
    if (pinned_guides)
        ImGui::PopClipRect();
    if (pinned_guides && s.view == View::Main)
        draw_guides(window, guides, guide_top);
    ImGui::End();
    ImGui::PopStyleColor(2);
    is_dirty = *open && (dirty() || pending_text());
    is_confirming = *open && confirmation();
    if (!*open) {
        config_preview_end();
        s.editing = false;
    }
}
