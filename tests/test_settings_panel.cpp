// Controller navigation exercised against the real ImGui frame loop.
#include "settings_panel.h"
#include "config.h"
#include "app_actions.h"
#include "overlay.h"
#include "modorder.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
static int requested;
extern "C" const char *g_game_dir = "obj/settings-test/game";  // (modorder.c compares archives with the game's)
extern "C" void ui_request_action(bool restart) { requested = restart ? 2 : 1; }
extern "C" bool ui_action_failed(void) { return false; }
extern "C" int ldn_hub_status(const char *, const char *, int *, int *) { return 0; }
extern "C" void ldn_hub_check(const char *, const char *) {}
extern "C" int overlay_face_position(int semantic) {
    const int p[] = {1, 0, 3, 2};
    return p[semantic];
}
static bool opened = true;
static void frame() {
    settings_panel_before_frame(opened);
    ImGui::NewFrame();
    settings_panel(&opened);
    ImGui::Render();
}
static void idle(int n = 3) {
    while (n--)
        frame();
}
static void press(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    idle();
    ImGui::GetIO().AddKeyEvent(key, false);
    idle();
}
static ImGuiID id(const char *name) {
    auto *w = ImGui::FindWindowByName("Settings##main");
    return ImHashStr("###row", 0, ImHashStr(name, 0, w->ID));
}
static void selected(const char *name) {
    auto &g = *ImGui::GetCurrentContext();
    if (g.NavId != id(name))
        fprintf(stderr, "focus %s: %x != %x\n", name, g.NavId, id(name));
    assert(g.NavId == id(name));
    assert(g.NavCursorVisible);
}
static float pinned_content_bottom() {
    auto *w = ImGui::FindWindowByName("Settings##main");
    float bottom = 0;
    for (const auto &command : w->DrawList->CmdBuffer) {
        auto clip = command.ClipRect;
        if (command.ElemCount && clip.x == w->InnerClipRect.Min.x && clip.z == w->InnerClipRect.Max.x &&
            clip.w < w->InnerClipRect.Max.y - 1 && clip.w > bottom)
            bottom = clip.w;
    }
    assert(bottom > w->InnerClipRect.Min.y);
    return bottom;
}
static float pinned_content_top() {
    auto *w = ImGui::FindWindowByName("Settings##main");
    float top = 0;
    for (const auto &command : w->DrawList->CmdBuffer) {
        auto clip = command.ClipRect;
        if (command.ElemCount && clip.x == w->InnerClipRect.Min.x && clip.z == w->InnerClipRect.Max.x &&
            clip.w < w->InnerClipRect.Max.y - 1 && clip.y > top)
            top = clip.y;
    }
    assert(top > w->InnerClipRect.Min.y);
    return top;
}
static void down(int n = 1) {
    while (n--)
        press(ImGuiKey_GamepadDpadDown);
}
static void tab() { press(ImGuiKey_GamepadR1); }
static void back() { press(ImGuiKey_GamepadFaceRight); }
static void confirm() { press(ImGuiKey_GamepadFaceDown); }
int main() {
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    // Mods tab fixtures: Aaa (two files) and Bbb (one) both replace a.bin, Ccc stands alone, Ddd is off ('_')
    namespace fs = std::filesystem;
    fs::remove_all("obj/settings-test/mods");
    auto put = [](const char *path, const char *text) {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path, std::ios::binary) << text;
    };
    put("obj/settings-test/mods/Aaa/romfs/a.bin", "1");
    put("obj/settings-test/mods/Aaa/romfs/b.bin", "bb");
    put("obj/settings-test/mods/Bbb/romfs/a.bin", "22");
    put("obj/settings-test/mods/Ccc/romfs/c.bin", "3");
    put("obj/settings-test/mods/_Ddd/romfs/d.bin", "4");
#ifdef _WIN32
    _putenv_s("MHGU_MODS_DIR", "obj/settings-test/mods");
#else
    setenv("MHGU_MODS_DIR", "obj/settings-test/mods", 1);
#endif
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    Config baseline = config_defaults();
    config_store_loaded(&baseline);
    config_runtime_init(&baseline);

    idle();
    selected("display.mode");
    // A click outside the menu unfocuses every window; the menu takes its focus back (it used to assert in SetNavID)
    io.AddMousePosEvent(4, 4);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    idle();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    idle();
    selected("display.mode");
    confirm();
    selected("choice.2");
    back();
    selected("display.mode");
    down(2);
    selected("display.width");
    confirm();
    selected("display.width");
    assert(opened && !settings_panel_confirming() &&
           !settings_panel_dirty()); // Numeric activation never opens an editor.
    press(ImGuiKey_GamepadDpadRight);
    assert(settings_panel_dirty());
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    frame();
    opened = true;
    idle();
    selected("display.mode");
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    idle(35);
    selected("fps");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    idle();
    tab();
    selected("face");
    down();
    selected("menu.confirm");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_SOUTH);
    assert(config_editable().face_buttons == FACE_POSITION);
    assert(!config_restart_required(&baseline));
    press(ImGuiKey_Home);
    down();
    selected("menu.confirm");
    press(ImGuiKey_GamepadDpadLeft);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    confirm();
    assert(config_live_menu_confirm() == MENU_CONFIRM_ORIGINAL);
    tab();
    selected("port");
    press(ImGuiKey_GamepadL1);
    selected("face");
    tab();
    tab();
    selected("hp.0");
    tab();
    selected("mod.Aaa");
    // The Mods tab: the conflict scan runs on another thread; Aaa and Bbb both replace a.bin
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    idle();
    down(4);  // (Aaa, Bbb, Ccc, then _Ddd that is off)
    selected("mods.conflicts");  // one row for them all
    down();
    selected("mods.rescan");  // (the suggested order is the one it has: Aaa changes more files, so Bbb comes after)
    press(ImGuiKey_Home);
    down();
    selected("mod.Bbb");
    confirm();
    down();  // Move up
    confirm();
    selected("mod.Bbb");
    assert(settings_panel_dirty());
    press(ImGuiKey_Home);
    selected("mod.Bbb");
    down(5);  // (Bbb, Aaa, Ccc, _Ddd, the conflict)
    selected("mods.suggest");  // Bbb now wins over the bigger Aaa: the order is suggested back
    confirm();
    press(ImGuiKey_Home);
    selected("mod.Aaa");
    down(2);
    selected("mod.Ccc");
    press(ImGuiKey_GamepadDpadLeft);  // off
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!settings_panel_dirty());
    {
        std::ifstream in("obj/settings-test/mods/load_order.txt");
        std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(list.find("\nAaa\nBbb\n") != std::string::npos);
        assert(list.find("\nCcc\n") == std::string::npos && list.find("# off: Ccc\n") != std::string::npos);
        assert(list.find("\n_Ddd\n") == std::string::npos);
    }
    // Choosing which of two conflicting mods is used moves it below the other
    press(ImGuiKey_Home);
    down(4);  // (Aaa, Bbb, then the off ones _Ddd and Ccc)
    selected("mods.conflicts");
    confirm();
    selected("mods.conflict.0");
    confirm();
    selected("choice.1");  // (Use Bbb, the one used now)
    press(ImGuiKey_GamepadDpadUp);
    selected("choice.0");  // Use Aaa
    confirm();
    selected("mods.conflict.0");
    down();
    selected("conflicts.back");
    confirm();
    selected("mods.conflicts");
    assert(settings_panel_dirty());
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    {
        std::ifstream in("obj/settings-test/mods/load_order.txt");
        std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(list.find("\nBbb\nAaa\n") != std::string::npos);
    }
    // A mod folder added after the Mods tab saved loads (on), after the listed ones; the ones turned off stay off
    put("obj/settings-test/mods/Eee/romfs/e.bin", "5");
    {
        ModEntry list[MODS_MAX];
        int n = mods_installed(list, MODS_MAX);
        assert(n == 5);
        const char *want[] = {"Bbb", "Aaa", "Eee", "_Ddd", "Ccc"};
        for (int i = 0; i < n; i++)
            assert(!strcmp(list[i].name, want[i]) && list[i].on == (i < 3));
    }
    fs::remove_all("obj/settings-test/mods/Eee");
    tab();
    selected("language");
    tab();
    selected("display.mode");
    tab();
    down(3);  // (fps, vsync, prewarm)
    selected("msaa");
    // Quick apply in a list of values: the highlighted one is live before it is chosen; Cancel puts it back
    int msaa_before = config_live_msaa();
    confirm();
    selected("choice.0");
    down();
    selected("choice.1");
    assert(config_live_msaa() == 2 && !settings_panel_dirty());
    back();
    selected("msaa");
    assert(config_live_msaa() == msaa_before && !settings_panel_dirty());
    down(3);  // (msaa, fxaa, drawdist)
    selected("volume");
    confirm();
    selected("volume");
    press(ImGuiKey_GamepadDpadLeft);
    assert(settings_panel_dirty());
    settings_panel_request_back();
    idle();
    selected("dialog.cancel");
    back();
    selected("volume");
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    assert(!requested);
    back();
    selected("volume");
    // Former save/discard shortcuts do nothing, including while held.
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    idle(50);
    assert(!ImGui::GetCurrentContext()->NavWindowingTarget);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    idle();
    assert(settings_panel_dirty());
    press(ImGuiKey_GamepadFaceUp);
    assert(settings_panel_dirty());
    assert(config_editable().volume == 100);
    press(ImGuiKey_End);
    selected("reset");
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(!settings_panel_dirty());
    assert(config_editable().volume == 99);
    press(ImGuiKey_Home);
    down(6);
    selected("volume");
    press(ImGuiKey_GamepadDpadRight);
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    assert(config_editable().volume == 99);
    frame();
    opened = true;
    idle();
    selected("fps");
    tab();
    tab();
    selected("port");
    confirm();
    selected("port");
    press(ImGuiKey_GamepadDpadRight);
    assert(settings_panel_dirty());
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "missing-settings-parent/panel.ini");
#else
    setenv("MHGU_CONFIG", "missing-settings-parent/panel.ini", 1);
#endif
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(settings_panel_dirty());
    assert(config_editable().ldn_port == 39500);
#ifdef _WIN32
    _putenv_s("MHGU_CONFIG", "obj/settings-test/panel-settings.ini");
#else
    setenv("MHGU_CONFIG", "obj/settings-test/panel-settings.ini", 1);
#endif
    back();
    selected("dialog.cancel");
    down(2);
    confirm();
    assert(!opened);
    frame();
    opened = true;
    idle();
    selected("port");
    down();
    selected("hosts");
    confirm();
    selected("key.0");
    io.AddInputCharactersUTF8("localhost:39500");
    idle();
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    assert(settings_panel_dirty());
    back();
    selected("key.0");
    back();
    selected("hosts");
    assert(!settings_panel_dirty());
    settings_panel_request_close();
    idle();
    selected("dialog.cancel");
    down();
    confirm();
    assert(requested == 1);
    back();
    selected("hosts");
    requested = 0;
    tab();
    tab();
    tab();
    selected("language");
    down(6);
    selected("restart");
    confirm();
    selected("dialog.cancel");
    back();
    selected("restart");
    confirm();
    down();
    confirm();
    assert(requested == 2);
    back();
    requested = 0;
    opened = false;
    frame();
    baseline = config_editable();
    io.DisplaySize = ImVec2(640, 480);
    baseline.menu_scale = 200;
    baseline.language = 1;
    config_store_loaded(&baseline);
    opened = true;
    idle();
    auto *w = ImGui::FindWindowByName("Settings##main");
    assert(w->Size.x <= io.DisplaySize.x);
    assert(w->ContentSize.x <= w->InnerRect.GetWidth() + 1);
    float top = w->Pos.y;
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    selected("hp.0");
    down(5);
    confirm();
    idle();
    assert(w->Pos.y == top);
    press(ImGuiKey_End);
    selected("reset");
    idle(12);
    assert(w->Scroll.y > 0);
    press(ImGuiKey_GamepadDpadUp);
    selected("hp.add");
    confirm();
    assert(settings_panel_dirty());
    down(5);
    confirm();
    selected("hp.add");
    assert(!settings_panel_dirty());
    press(ImGuiKey_Home);
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    selected("reset");
    confirm();
    selected("dialog.cancel");
    down();
    confirm();
    selected("reset");
    assert(!settings_panel_dirty());
    io.DisplaySize = ImVec2(640, 480);
    idle(12);
    assert(w->Size.x <= 640);
    assert(w->Size.y <= 480);
    assert(w->ContentSize.x <= w->InnerRect.GetWidth() + 1);
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    press(ImGuiKey_GamepadL1);
    selected("fps");
    down(8);  // (vsync, prewarm, msaa, fxaa, drawdist, volume, mutebg, fsr)
    selected("fsr");
    confirm();
    assert(settings_panel_dirty());
    down();
    selected("fsr.provider");
    press(ImGuiKey_GamepadDpadRight);
    down();
    selected("fsr.quality");
    press(ImGuiKey_GamepadDpadRight);
    press(ImGuiKey_End);
    press(ImGuiKey_GamepadDpadUp);
    selected("save");
    confirm();
    assert(config_editable().fsr == 1 && config_editable().fsr_upscaler == FSR_UP_4);
    assert(config_editable().fsr_quality == FSR_QUALITY);
    Config fsr_saved = config_editable();
    assert(config_restart_required(&fsr_saved));
    opened = false;
    frame();
    g_config.fsr = 1;
    opened = true;
    idle();
    selected("fps");
    down(4);
    selected("volume");
    assert(!settings_panel_dirty()); // FSR-owned AA rows are disabled, not misleading live toggles.
    idle(20);
    float pinned_bottom = pinned_content_bottom();
    float pinned_top = pinned_content_top();
    press(ImGuiKey_End);
    idle(40);
    selected("reset");
    auto *root = ImGui::FindWindowByName("Settings##main");
    assert(root->Scroll.y > 0);
    assert(pinned_content_bottom() == pinned_bottom); // Guide position does not follow the scroll.
    assert(pinned_content_top() == pinned_top);
    assert(root->NavRectRel[ImGuiNavLayer_Main].Max.y + root->Pos.y <= pinned_bottom + 1);
    press(ImGuiKey_Home);
    idle(40);
    selected("fps");
    assert(pinned_content_bottom() == pinned_bottom);
    assert(pinned_content_top() == pinned_top);
    assert(root->NavRectRel[ImGuiNavLayer_Main].Min.y + root->Pos.y >= pinned_top - 1);
    press(ImGuiKey_End);
    idle(20);
    float font = root->FontRefSize;
    io.AddMousePosEvent(root->Pos.x + ImGui::GetStyle().WindowPadding.x + font * 2.6f,
                        root->Pos.y + ImGui::GetStyle().WindowPadding.y + font * .8f);
    idle();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    idle(8);
    selected("display.mode"); // Pinned tabs remain clickable when the option list is scrolled.
    press(ImGuiKey_GamepadR1);
    selected("fps");
    opened = false;
    frame();
    assert(!settings_panel_dirty());
    ImGui::DestroyContext();
    remove("obj/settings-test/panel-settings.ini");
    puts("settings panel: inline numbers, explicit Save/Reset, Back guide, dirty discard, focus and layout "
         "OK");
}
