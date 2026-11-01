// Player settings (config.h): mhgurecomp.ini in the game folder (next to the executable, or MHGU_HOME; with a game
// folder argument, the current folder). A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1,
//                        MHGU_RT16=0|1 (SmoothGradients), MHGU_PAD_POSITIONAL=0|1 (test runs; override the file)
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

#include "keymap.h"

#define DEFAULT_BINDS {{SDL_SCANCODE_W}, {SDL_SCANCODE_S}, {SDL_SCANCODE_A}, {SDL_SCANCODE_D}, {SDL_SCANCODE_LSHIFT},     \
    {KEY_MOUSE + SDL_BUTTON_RIGHT}, {SDL_SCANCODE_SPACE}, {KEY_MOUSE + SDL_BUTTON_LEFT},                               \
    {SDL_SCANCODE_R, KEY_MOUSE + SDL_BUTTON_MIDDLE},                                                                   \
    {SDL_SCANCODE_Q}, {SDL_SCANCODE_E}, {SDL_SCANCODE_TAB}, {SDL_SCANCODE_T}, {SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER}, \
    {SDL_SCANCODE_BACKSPACE}, {SDL_SCANCODE_UP}, {SDL_SCANCODE_DOWN}, {SDL_SCANCODE_LEFT}, {SDL_SCANCODE_RIGHT},          \
    {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL}, {SDL_SCANCODE_LALT, SDL_SCANCODE_RALT}, {KEY_WHEEL_UP}, {KEY_WHEEL_DOWN}}
#define CONFIG_DEFAULT_VALUES DISPLAY_BORDERLESS, 1920, 1080, 0, 0, 60, 1, 1, 1, FACE_POSITION, 39500, "", 0, 1, 1, {{1, 1, 1, 1}, 2}, 1.0f, 0.0f, 1.0f, UI_CENTER, 1, 100, 0, 0, 100, MENU_CONFIRM_ORIGINAL, 1, 0, FSR_NATIVE, 0.8f, 1, 0.9f, 1, FSR_UP_3, UPDATES_ASK, 1, 1.0f, 0, COMBO_LENIENT, "", "", "/", FXAA_ORIGINAL, 0, DEFAULT_BINDS, THEME_HUNTER, 1, 1
Config g_config = {CONFIG_DEFAULT_VALUES};
Config config_defaults(void) { Config c = {CONFIG_DEFAULT_VALUES}; return c; }
uint32_t g_render_size[2] = {1920, 1080};
float g_render_aspect = 16.0f / 9.0f;
float g_fov_scale = 1.0f;
uint32_t g_ui_size[2] = {1280, 720};
float g_ui_anchor_aspect = 16.0f / 9.0f;
uint32_t g_fsr_output[2];
float g_fsr_lod_bias;
const char *g_fsr4_fallback;

static const char DEFAULT_INI[] =
    "; MHGU Recomp settings. Delete this file to reset to default settings, the file is regenerated.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed, Borderless\n"
    "Mode = Borderless\n"
    "\n"
    "; Resolution (WxH)\n"
    "Resolution = 1920x1080\n"
    "\n"
    "; Rendering resolution (WxH)\n"
    "; Higher = sharper (for supersampling)\n"
    "; Default: Auto\n"
    "RenderResolution = Auto\n"
    "\n"
    "; Set to 16:9 to preserve the game's native aspect ratio\n"
    "; Default: Auto\n"
    "AspectRatio = Auto\n"
    "\n"
    "; Field of view\n"
    "; Between 0.5 and 2\n"
    "; Default: 1.0\n"
    "FOV = 1.0\n"
    "\n"
    "; At resolutions wider than 16:9, determines where the HUD goes\n"
    "; Center, Edges\n"
    "; Default: Center\n"
    "UIAnchor = Center\n"
    "\n"
    "[Anti-aliasing]\n"
    "; Multi-Sample Anti Aliasing, cycle modes with F7 key\n"
    "; Off, 2x, 4x, 8x\n"
    "; Default: Off\n"
    "MSAA = Off\n"
    "\n"
    "; GU's default FXAA\n"
    "; Default: On\n"
    "FXAA = On\n"
    "\n"
    "; Which FXAA: Original is GU's own, 3_11 is FXAA 3.11, which keeps textures sharper\n"
    "; Original, 3_11\n"
    "; Default: Original\n"
    "FXAAMode = Original\n"
    "\n"
    "[Upscaler]\n"
    "; FSR temporal upscaling\n"
    "; Default: Off\n"
    "FSR = Off\n"
    "\n"
    "; Which upscaler to use (FSR3 is FSR 3.1)\n"
    "; FSR3, FSR4\n"
    "; Default: FSR3\n"
    "Upscaler = FSR3\n"
    "\n"
    "; FSR quality\n"
    "; NativeAA, Quality, Balanced, Performance, UltraPerformance\n"
    "; Default: NativeAA\n"
    "FSRQuality = NativeAA\n"
    "\n"
    "; FSR's sharpening filter strength\n"
    "; Between 0 to 1\n"
    "; Default: 0.8\n"
    "Sharpness = 0.8\n"
    "\n"
    "; FSR Object Motion, reduces smearing\n"
    "; Default: On\n"
    "ObjectMotion = On\n"
    "\n"
    "; Reactive anti-ghosting\n"
    "; Between 0 and 1\n"
    "; Default: 0.9\n"
    "Reactive = 0.9\n"
    "\n"
    "; Animated surface textures skip FSR's frame history to avoid smearing\n"
    "; Default: On\n"
    "AnimSurf = On\n"
    "\n"
    "[Graphics]\n"
    "; How far away people in the villages and the Hubs are still drawn: 1 is the game's distance, 2 twice as far,\n"
    "; 0.5 half as far, 0 no limit\n"
    "; Between 0 and 10\n"
    "; Default: 1.0\n"
    "DrawDistance = 1.0\n"
    "\n"
    "; Renders the picture with 16 bits per colour instead of 8, so dark skies and soft glows show no colour bands.\n"
    "; Costs GPU time: about 1 ms a frame at 1600x1000, more at higher resolutions\n"
    "; Default: Off\n"
    "SmoothGradients = Off\n"
    "\n"
    "[Performance]\n"
    "; Framerate cap\n"
    "; Between 30 and 240\n"
    "; Default: 60\n"
    "FPS = 60\n"
    "\n"
    "; VSync\n"
    "VSync = On\n"
    "\n"
    "; Translate all of the game's shaders in the background at startup, so fewer things are drawn black the first time\n"
    "; they appear. The first start does this in about 20 seconds while the logos play; it needs about 340 MB in the cache folder.\n"
    "; Default: On\n"
    "ShaderPrewarm = On\n"
    "\n"
    "[Controls]\n"
    "; Controller face button layout. Set to \"Label\" to reverse A/B and X/Y on XBox controllers to match the button prompts (not recommended)\n"
    "; Position, Label\n"
    "; Default: Position\n"
    "FaceButtons = Position\n"
    "\n"
    "; Confirm button in the settings menu (F1, or Minus + Plus on a controller). The game's own menus are unchanged.\n"
    "; Original, South (bottom button), East (right button)\n"
    "; Default: Original\n"
    "MenuConfirm = Original\n"
    "\n"
    "; Controller input is also registered if the window is out of focus\n"
    "; Default: On\n"
    "BackgroundInput = On\n"
    "\n"
    "; How long the game waits for a second button after the first, for inputs like X+A. Lenient: as long as at 30 fps\n"
    "; (on the Switch) at any frame rate. Strict: a fixed number of frames, so the higher the frame rate, the closer\n"
    "; together both buttons have to be pressed\n"
    "; Lenient, Strict\n"
    "; Default: Lenient\n"
    "ComboInputWindow = Lenient\n"
    "\n"
    "; The key that opens a line of chat, anywhere (in a quest too): a key's name as the settings menu shows it, or\n"
    "; nothing for none\n"
    "; Default: /\n"
    "ChatKey = /\n"
    "\n"
    "[Mouse]\n"
    "; Mouse sensitivity\n"
    "; Default: 1.0\n"
    "MouseSensitivity = 1.0\n"
    "\n"
    "[Keybinds]\n"
    "; The keys or mouse buttons for each Switch button, for moving (the left stick) and for turning the item bar\n"
    "; (ItemLeft / ItemRight: one item per press, in a hunt, without holding L).\n"
    "; Up to two each, separated by a comma, or None.\n"
    "; Mouse buttons: Mouse Left, Mouse Right, Mouse Middle, Mouse 4, Mouse 5, Wheel Up, Wheel Down.\n"
    "; Esc, Insert, F1 and F7 to F12 are the recomp's own keys.\n"
    "MoveUp = W\n"
    "MoveDown = S\n"
    "MoveLeft = A\n"
    "MoveRight = D\n"
    "Walk = Left Shift\n"
    "A = Mouse Right\n"
    "B = Space\n"
    "X = Mouse Left\n"
    "Y = R, Mouse Middle\n"
    "L = Q\n"
    "R = E\n"
    "ZL = Tab\n"
    "ZR = T\n"
    "Plus = Return, Keypad Enter\n"
    "Minus = Backspace\n"
    "DpadUp = Up\n"
    "DpadDown = Down\n"
    "DpadLeft = Left\n"
    "DpadRight = Right\n"
    "LeftStickClick = Left Ctrl, Right Ctrl\n"
    "RightStickClick = Left Alt, Right Alt\n"
    "ItemLeft = Wheel Up\n"
    "ItemRight = Wheel Down\n"
    "\n"
    "[Multiplayer]\n"
    "; Which port to host/join hubs on (TCP and UDP)\n"
    "; Default: 39500\n"
    "Port = 39500\n"
    "\n"
    "; IP Address of the host to join. Use IP:Port if it doesn't match the port above.\n"
    "; Can be multiple IP addresses, comma-separated\n"
    "; Default:\n"
    "Join =\n"
    "\n"
    "; Hub server for playing over the internet without port forwarding: hubs you create are listed there, and\n"
    "; Search Hubs shows its hubs next to the LAN's. host or host:port (default port 39510).\n"
    "; Empty: the default server. Off: no internet hubs.\n"
    "; Default:\n"
    "Server =\n"
    "\n"
    "; Matchmaking key: hubs are only seen by players with the same key, so friends who all enter the same key\n"
    "; only see each other's hubs. Empty: public, your hubs are seen by (and you see) everyone without a key.\n"
    "; Default:\n"
    "Key =\n"
    "\n"
    "; Sync small monsters between players in multiplayer.\n"
    "; (Currently off by default, pending a bug investigation where small monsters may refuse to attack players)\n"
    "; Default: Off\n"
    "SyncSmallMonsters = Off\n"
    "\n"
    "[Monster HP]\n"
    "; Playercount-specific HP multipliers for monsters in Hub quests.\n"
    "; Only affects large monster health.\n"
    "; When joining a hub, the host's scaling settings are always used.\n"
    "; Default: 1.0\n"
    "1Player = 1.0\n"
    "2Players = 1.0\n"
    "3Players = 1.0\n"
    "4Players = 1.0\n"
    "\n"
    "; (Optional: quest-specific HP multipliers go here. Comma-separated per playercount, 1 line per quest. Example:)\n"
    "; Quest10107 = 1.0, 1.0, 1.0, 1.0\n"
    "\n"
    "[Village Quests]\n"
    "; Press L/R in the Hub while selecting a quest to swap to the Village Quest list instead, letting you play Village quests in multiplayer.\n"
    "; Default: On\n"
    "VillageQuestsInHub = On\n"
    "\n"
    "; When playing a Village Quest from the Hub, first multiply the monster HP by this amount.\n"
    "; This amount is then scaled again by the [Monster HP] settings.\n"
    "; Default: 2.0\n"
    "HubHP = 2.0\n"
    "\n"
    "[Discord]\n"
    "; Show what you're doing in the game on your Discord profile (Discord must be running on this computer):\n"
    "; where you are, the quest's monsters and how many hunters are with you\n"
    "; Default: On\n"
    "RichPresence = On\n"
    "\n"
    "; Show your hunter's name and Hunter Rank there too\n"
    "; Default: On\n"
    "ShowHunter = On\n"
    "\n"
    "[Bonus Content]\n"
    "; If enabled, the free online downloadable content (Event Quests, item packs, Palicoes) are automatically copied from the provided savegame inside the `bonus/` folder into your own savegame.\n"
    "; This only happens if your savegame hasn't received any downloadable content yet.\n"
    "; Default: On\n"
    "BonusContent = On\n"
    "\n"
    "[Audio]\n"
    "; Volume of the whole game\n"
    "; Between 0 (mute) and 100\n"
    "; Default: 100\n"
    "Volume = 100\n"
    "\n"
    "; Mute the game while its window is in the background (not focused)\n"
    "; Default: Off\n"
    "MuteInBackground = Off\n"
    "\n"
    "[Application]\n"
    "; Ask before the game window closes (closing does not save the game)\n"
    "; Default: On\n"
    "ConfirmExit = On\n"
    "\n"
    "; Language of the settings menu (F1, or Minus + Plus on a controller)\n"
    "; English, Spanish\n"
    "; Default: English\n"
    "Language = English\n"
    "\n"
    "; Button icons in the settings menu\n"
    "; Xbox, Switch, PlayStation\n"
    "; Default: Xbox\n"
    "ButtonIcons = Xbox\n"
    "\n"
    "; Colours of the settings menu\n"
    "; Gold, Blue, Hunter\n"
    "; Default: Hunter\n"
    "Theme = Hunter\n"
    "\n"
    "; Size of the settings menu, in percent\n"
    "; Between 50 and 200\n"
    "; Default: 100\n"
    "MenuScale = 100\n"
    "\n"
    "; What mhgurecomp-updater does when it is started: check GitHub for a newer release and ask before installing it,\n"
    "; install it without asking, or only start the game. Starting mhgurecomp itself never checks or updates.\n"
    "; Ask, Auto, Off\n"
    "; Default: Ask\n"
    "Updates = Ask\n";

void config_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    snprintf(buf, size, "mhgurecomp.ini");  // (started on its own the game runs in its folder, or MHGU_HOME: main.c)
}

// lower case, spaces and '_' / '-' removed: "Borderless Window" == "borderlesswindow"
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') *d++ = (char)tolower((unsigned char)*s);
    *d = 0;
}

static bool parse_size(const char *v, int *w, int *h) {
    int a, b;
    char x;
    if (sscanf(v, "%d %c %d", &a, &x, &b) != 3 || (x != 'x' && x != 'X' && x != '*')) return false;
    if (a < 320 || b < 180 || a > 16384 || b > 16384) return false;
    *w = a, *h = b;
    return true;
}

// [Monster HP] factors: "0.6", "0.6x", or a comma separated list of up to four (squashed: no spaces). The count read,
// 0 when one is not a number in HP_SCALE_MIN..HP_SCALE_MAX.
static int parse_factors(const char *v, float f[4]) {
    int n = 0;
    while (*v && n < 4) {
        char *end;
        float x = strtof(v, &end);
        if (end == v || !(x >= HP_SCALE_MIN && x <= HP_SCALE_MAX)) return 0;
        f[n++] = x;
        v = end + (*end == 'x');
        if (*v == ',') v++;
        else if (*v) return 0;
    }
    return *v ? 0 : n;
}

// The vertical field of view (radians) a camera of this aspect gets (tools/gen.py PATCHES: the three callers of the
// perspective matrix that load a camera's). One made for the screen (its 16:9 aspect replaced with g_render_aspect)
// gets the FOV factor and, on a picture narrower than 16:9 (16:10), the height that keeps 16:9's width (Vert+): more
// above and below instead of less to the sides. Other cameras keep their own.
float camera_fovy(float fovy, float aspect) {
    if (aspect != g_render_aspect) return fovy;
    fovy *= config_live_fov();
    if (aspect < 16.0f / 9.0f) fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}

// Town NPCs (uLobbyNpc: villages, Hubs) fade in within a distance from the game's table (2500 units, 1800 for some)
// and out beyond it; tools/gen.py PATCHES hands its square here. [Graphics] DrawDistance scales it, 0 is no limit.
float npc_fade_dist2(float d2) {
    float m = config_live_draw_distance();
    return m > 0 ? d2 * m * m : FLT_MAX;
}

// A picture shape: "Auto" (0), "21:9" / "21/9", or a number (0.5 .. 10).
static bool parse_aspect(const char *v, float *out) {
    float n, d, a;
    char sep, *end;
    if (!strcasecmp(v, "auto")) { *out = 0; return true; }
    if (sscanf(v, "%f %c %f", &n, &sep, &d) == 3 && (sep == ':' || sep == '/') && n > 0 && d > 0) {
        a = n / d;
    } else {
        a = strtof(v, &end);
        if (end == v || *end) return false;
    }
    if (a < 0.5f || a > 10.0f) return false;
    *out = a;
    return true;
}

// ---- the settings of the file (config.h ConfigSetting): one row each, in the file's order
// config_validate's messages (settings_locale.h has them in Spanish; tests/test_settings_locale.py checks)
#define ERR_SIZE "Resolution must be Desktop/Auto or 320x180 to 16384x16384."
#define ERR_ASPECT "Aspect ratio must be Auto or between 0.5 and 10."
#define ERR_VIEW "FOV must be 0.5-2.0 and mouse sensitivity 0.1-10."
#define ERR_MISC "Invalid frame cap, anti-aliasing, controller, HUD or volume setting."
#define ERR_TOGGLE "Invalid On/Off value."
#define ERR_FSR "Invalid FSR quality, provider, sharpness or reactive mask."
#define ERR_NET "Invalid port or Join address list (no newlines, ';' or '#')."
#define ERR_SERVER "Invalid hub server or matchmaking key (no newlines, ';' or '#')."
#define ERR_HP "HP factors must be 0.05-20."
#define ERR_VILLAGE "Invalid village HP or quest override count."
#define ERR_CONFIRM "Invalid menu confirmation policy."
#define ERR_SCALE "Invalid interface scale."
#define ERR_ICONS "Invalid button icon layout."
#define ERR_LANGUAGE "Invalid menu language."
#define ERR_UPDATES "Invalid update setting."
#define ERR_DRAW "Draw distance must be 0-10 (0: no limit)."
#define ERR_KEYS "Invalid keybind."
#define F(x) offsetof(Config, x)
#define ON_OFF(section, key, field, restart) {section, key, SET_BOOL, F(field), 0, 0, 0, NULL, NULL, restart, ERR_TOGGLE}
#define KEYS(key, bind) {"Keybinds", key, SET_KEYS, F(binds[bind]), 0, 0, 0, NULL, NULL, false, ERR_KEYS}
static const int anchor_values[] = {UI_CENTER, UI_21_9, UI_32_9, UI_EDGES}, msaa_values[] = {1, 2, 4, 8};
const ConfigSetting g_settings[] = {
    {"Display", "Mode", SET_CHOICE, F(display_mode), 0, 0, 0,
     "Windowed|window,Fullscreen|exclusive|exclusivefullscreen,Borderless|borderless*", NULL, true, ERR_SIZE},
    {"Display", "Resolution", SET_SIZE, F(width), F(height), 0, 0, "Desktop|auto", NULL, true, ERR_SIZE},
    {"Display", "RenderResolution", SET_SIZE, F(render_w), F(render_h), 0, 0, "Auto", NULL, true, ERR_SIZE},
    {"Display", "AspectRatio", SET_ASPECT, F(aspect_ratio), 0, 0.5f, 10.0f, NULL, NULL, true, ERR_ASPECT},
    {"Display", "FOV", SET_FLOAT, F(fov), 0, 0.5f, 2.0f, NULL, NULL, false, ERR_VIEW},
    {"Display", "UIAnchor", SET_CHOICE, F(ui_anchor), 0, 0, 0, "Center|1|16:9,21:9|2,32:9|3,Edges", anchor_values, true,
     ERR_MISC},
    {"Anti-aliasing", "MSAA", SET_CHOICE, F(msaa), 0, 0, 0, "Off|none|0|1|1x,2x|2,4x|4,8x|8", msaa_values, false, ERR_MISC},
    ON_OFF("Anti-aliasing", "FXAA", fxaa, false),
    {"Anti-aliasing", "FXAAMode", SET_CHOICE, F(fxaa_mode), 0, 0, 0, "Original,3_11", NULL, false, ERR_MISC},
    ON_OFF("Upscaler", "FSR", fsr, true),
    {"Upscaler", "Upscaler", SET_CHOICE, F(fsr_upscaler), 0, 0, 0, "FSR3|fsr3.1|3,FSR4|4", NULL, true, ERR_FSR},
    {"Upscaler", "FSRQuality", SET_CHOICE, F(fsr_quality), 0, 0, 0,
     "NativeAA|native|aa,Quality,Balanced,Performance,UltraPerformance|ultraperf", NULL, true, ERR_FSR},
    {"Upscaler", "Sharpness", SET_FLOAT, F(fsr_sharpness), 0, 0.0f, 1.0f, NULL, NULL, true, ERR_FSR},
    ON_OFF("Upscaler", "ObjectMotion", fsr_objmotion, true),
    {"Upscaler", "Reactive", SET_FLOAT, F(fsr_reactive), 0, 0.0f, 1.0f, NULL, NULL, true, ERR_FSR},
    ON_OFF("Upscaler", "AnimSurf", fsr_animsurf, true),
    {"Graphics", "DrawDistance", SET_FLOAT, F(draw_distance), 0, 0.0f, 10.0f, NULL, NULL, false, ERR_DRAW},
    ON_OFF("Graphics", "SmoothGradients", smooth_gradients, false),
    {"Performance", "FPS|FrameRate", SET_INT, F(fps), 0, 30, 240, NULL, NULL, false, ERR_MISC},
    ON_OFF("Performance", "VSync", vsync, false),
    ON_OFF("Performance", "ShaderPrewarm", shader_prewarm, true),
    {"Controls", "FaceButtons", SET_CHOICE, F(face_buttons), 0, 0, 0, "Position|positional|nintendo,Label|labels|xbox",
     NULL, false, ERR_MISC},
    {"Controls", "MenuConfirm", SET_CHOICE, F(menu_confirm), 0, 0, 0, "Original,South,East", NULL, false, ERR_CONFIRM},
    ON_OFF("Controls", "BackgroundInput", background_input, false),
    {"Controls", "ComboInputWindow", SET_CHOICE, F(combo_window), 0, 0, 0, "Lenient,Strict", NULL, false, ERR_MISC},
    {"Controls", "ChatKey", SET_TEXT, F(chat_key), sizeof g_config.chat_key, 0, 0, NULL, NULL, false, ERR_MISC},
    {"Mouse", "MouseSensitivity", SET_FLOAT, F(mouse_sensitivity), 0, 0.1f, 10.0f, NULL, NULL, false, ERR_VIEW},
    KEYS("MoveUp", BIND_MOVE_UP), KEYS("MoveDown", BIND_MOVE_DOWN), KEYS("MoveLeft", BIND_MOVE_LEFT),
    KEYS("MoveRight", BIND_MOVE_RIGHT), KEYS("Walk", BIND_WALK), KEYS("A", BIND_A), KEYS("B", BIND_B), KEYS("X", BIND_X),
    KEYS("Y", BIND_Y), KEYS("L", BIND_L), KEYS("R", BIND_R), KEYS("ZL", BIND_ZL), KEYS("ZR", BIND_ZR),
    KEYS("Plus", BIND_PLUS), KEYS("Minus", BIND_MINUS), KEYS("DpadUp", BIND_DPAD_UP), KEYS("DpadDown", BIND_DPAD_DOWN),
    KEYS("DpadLeft", BIND_DPAD_LEFT), KEYS("DpadRight", BIND_DPAD_RIGHT), KEYS("LeftStickClick", BIND_LSTICK),
    KEYS("RightStickClick", BIND_RSTICK), KEYS("ItemLeft", BIND_ITEM_LEFT), KEYS("ItemRight", BIND_ITEM_RIGHT),
    {"Multiplayer", "Port", SET_INT, F(ldn_port), 0, 1, 65535, NULL, NULL, true, ERR_NET},
    {"Multiplayer", "Join", SET_TEXT, F(ldn_join), sizeof g_config.ldn_join, 0, 0, NULL, NULL, true, ERR_NET},
    {"Multiplayer", "Server", SET_TEXT, F(ldn_server), sizeof g_config.ldn_server, 0, 0, NULL, NULL, false, ERR_SERVER},
    {"Multiplayer", "Key", SET_TEXT, F(ldn_key), sizeof g_config.ldn_key, 0, 0, NULL, NULL, false, ERR_SERVER},
    ON_OFF("Multiplayer", "SyncSmallMonsters", sync_small_monsters, true),
    {"Monster HP", "1Player|1Players", SET_FACTOR, F(hp.scale[0]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "2Players|2Player", SET_FACTOR, F(hp.scale[1]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "3Players|3Player", SET_FACTOR, F(hp.scale[2]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "4Players|4Player", SET_FACTOR, F(hp.scale[3]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    ON_OFF("Village Quests", "VillageQuestsInHub", village_in_hub, true),
    {"Village Quests", "HubHP", SET_FACTOR, F(hp.village), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_VILLAGE},
    ON_OFF("Discord", "RichPresence", discord, false),
    ON_OFF("Discord", "ShowHunter", discord_hunter, false),
    ON_OFF("Bonus Content", "BonusContent", bonus_content, true),
    {"Audio", "Volume", SET_INT, F(volume), 0, 0, 100, NULL, NULL, false, ERR_MISC},
    ON_OFF("Audio", "MuteInBackground", mute_in_background, false),
    ON_OFF("Application", "ConfirmExit", confirm_exit, false),
    {"Application", "Language", SET_CHOICE, F(language), 0, 0, 0, "English|en,Spanish|es", NULL, false, ERR_LANGUAGE},
    {"Application", "ButtonIcons", SET_CHOICE, F(button_icons), 0, 0, 0, "Xbox,Switch,PlayStation", NULL, false, ERR_ICONS},
    {"Application", "Theme", SET_CHOICE, F(menu_theme), 0, 0, 0, "Gold,Blue,Hunter", NULL, false, ERR_MISC},
    {"Application", "MenuScale", SET_INT, F(menu_scale), 0, 50, 200, NULL, NULL, false, ERR_SCALE},
    {"Application", "Updates", SET_CHOICE, F(updates), 0, 0, 0, "Ask,Auto|automatic,Off|never", NULL, false, ERR_UPDATES},
};
#undef ON_OFF
#undef KEYS
const int g_nsettings = (int)(sizeof g_settings / sizeof g_settings[0]);

const ConfigSetting *config_bind_setting(int bind) {
    for (int i = 0; i < g_nsettings; i++)
        if (g_settings[i].kind == SET_KEYS && g_settings[i].field == F(binds[bind])) return &g_settings[i];
    return NULL;
}
#undef F

// [Keybinds] names: SDL's for a key, but its own for the ones whose name has a ',' (the list's separator) or a
// ';' / '#' (a comment in the file), and for the mouse buttons and wheel
static const struct { int code; const char *name; } key_names[] = {
    {SDL_SCANCODE_COMMA, "Comma"}, {SDL_SCANCODE_KP_COMMA, "Keypad Comma"}, {SDL_SCANCODE_SEMICOLON, "Semicolon"},
    {SDL_SCANCODE_KP_HASH, "Keypad Hash"}, {KEY_MOUSE + SDL_BUTTON_LEFT, "Mouse Left"},
    {KEY_MOUSE + SDL_BUTTON_RIGHT, "Mouse Right"}, {KEY_MOUSE + SDL_BUTTON_MIDDLE, "Mouse Middle"},
    {KEY_MOUSE + SDL_BUTTON_X1, "Mouse 4"}, {KEY_MOUSE + SDL_BUTTON_X2, "Mouse 5"}, {KEY_WHEEL_UP, "Wheel Up"},
    {KEY_WHEEL_DOWN, "Wheel Down"},
};
const char *config_key_name(int code) {
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (key_names[i].code == code) return key_names[i].name;
    const char *n = code > 0 && code < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)code) : "";
    return n && !strpbrk(n, ",;#") ? n : "";
}
int config_key_code(const char *name) {
    if (!*name) return 0;
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (!strcasecmp(key_names[i].name, name)) return key_names[i].code;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc != SDL_SCANCODE_UNKNOWN && *config_key_name((int)sc) ? (int)sc : 0;
}
// One of a [Keybinds] row's keys: a key or button with a name, not one the recomp keeps for itself (keymap.h)
static bool bind_key_valid(int code) { return !code || (*config_key_name(code) && !key_reserved(code)); }

// Is name (squashed) one of the '|'-separated spellings in list (from list, up to its end or a ',')? With *index, the
// number of the ','-separated item it matched.
static bool spelled(const char *list, const char *name, int *index) {
    int item = 0;
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, "|,");
        char one[64];
        snprintf(one, sizeof one, "%.*s", (int)n, p);
        squash(one);
        size_t len = strlen(one);
        bool prefix = len && one[len - 1] == '*';
        if (prefix ? !strncmp(name, one, len - 1) : !strcmp(name, one)) {
            if (index) *index = item;
            return true;
        }
        p += n;
        if (*p == ',') item++;
        if (*p) p++;
    }
    return false;
}

const ConfigSetting *config_setting(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    squash(k);
    for (int i = 0; i < g_nsettings; i++)
        if (spelled(g_settings[i].key, k, NULL)) return &g_settings[i];
    return NULL;
}

// The first spelling of a choice (as the file writes it)
static void choice_name(const char *list, int index, char *buf, size_t size) {
    const char *p = list;
    for (int i = 0; i < index && p; i++) if ((p = strchr(p, ','))) p++;
    if (!p) p = list;
    snprintf(buf, size, "%.*s", (int)strcspn(p, "|,"), p);
}

static int *field_int(const Config *c, size_t at) { return (int *)((char *)c + at); }
static float *field_float(const ConfigSetting *s, const Config *c) { return (float *)((char *)c + s->field); }
static int choice_value(const ConfigSetting *s, int index) { return s->values ? s->values[index] : index; }
static int nchoices(const ConfigSetting *s) {
    int n = 1;
    for (const char *p = s->choices; *p; p++) n += *p == ',';
    return n;
}

static bool parse_bool(const char *v, int *out) {
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) *out = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) *out = 0;
    else return false;
    return true;
}

bool config_parse(const ConfigSetting *s, const char *value, Config *c) {
    char v[256], *end;
    snprintf(v, sizeof v, "%s", value);
    squash(v);
    int index;
    float f[4];
    switch (s->kind) {
    case SET_BOOL: return parse_bool(v, field_int(c, s->field));
    case SET_INT: {
        double d = strtod(v, &end);
        if (end == v || *end || !(d >= s->min && d <= s->max)) return false;
        *field_int(c, s->field) = (int)d;
        return true;
    }
    case SET_FLOAT: {
        float x = strtof(v, &end);
        if (end == v || *end || !(x >= s->min && x <= s->max)) return false;
        *field_float(s, c) = x;
        return true;
    }
    case SET_FACTOR:
        if (parse_factors(v, f) != 1) return false;
        *field_float(s, c) = f[0];
        return true;
    case SET_CHOICE:
        if (!spelled(s->choices, v, &index)) return false;
        *field_int(c, s->field) = choice_value(s, index);
        return true;
    case SET_SIZE:
        if (spelled(s->choices, v, NULL)) {
            *field_int(c, s->field) = *field_int(c, s->field2) = 0;
            return true;
        }
        return parse_size(v, field_int(c, s->field), field_int(c, s->field2));
    case SET_ASPECT: return parse_aspect(v, field_float(s, c));
    case SET_TEXT:  // (as written: host names keep their dashes)
        snprintf((char *)c + s->field, s->field2, "%s", value);
        return true;
    case SET_KEYS: {  // names separated by ',' ("Return, Keypad Enter"), or None
        int keys[BIND_KEYS] = {0}, n = 0;
        if (strcmp(v, "none") && *v)
            for (const char *p = value;; p++) {
                size_t len = strcspn(p, ",");
                char one[64];
                snprintf(one, sizeof one, "%.*s", (int)len, p);
                char *a = one, *b = one + strlen(one);
                while (isspace((unsigned char)*a)) a++;
                while (b > a && isspace((unsigned char)b[-1])) *--b = 0;
                int code = config_key_code(a);
                if (!code || !bind_key_valid(code) || n == BIND_KEYS) return false;
                if (!n || keys[0] != code) keys[n++] = code;
                p += len;
                if (!*p) break;
            }
        memcpy(field_int(c, s->field), keys, sizeof keys);
        return true;
    }
    }
    return false;
}

void config_format(const ConfigSetting *s, const Config *c, char *buf, size_t size) {
    int iv = s->kind == SET_FLOAT || s->kind == SET_FACTOR || s->kind == SET_ASPECT || s->kind == SET_TEXT
                 ? 0 : *field_int(c, s->field);
    float fv = s->kind == SET_FLOAT || s->kind == SET_FACTOR || s->kind == SET_ASPECT ? *field_float(s, c) : 0;
    switch (s->kind) {
    case SET_BOOL: snprintf(buf, size, "%s", iv ? "On" : "Off"); break;
    case SET_INT: snprintf(buf, size, "%d", iv); break;
    case SET_FLOAT:
    case SET_FACTOR: snprintf(buf, size, "%g", (double)fv); break;
    case SET_CHOICE:
        for (int i = 0, n = nchoices(s); i < n; i++)
            if (choice_value(s, i) == iv) { choice_name(s->choices, i, buf, size); return; }
        choice_name(s->choices, 0, buf, size);
        break;
    case SET_SIZE:
        if (iv) snprintf(buf, size, "%dx%d", iv, *field_int(c, s->field2));
        else choice_name(s->choices, 0, buf, size);
        break;
    case SET_ASPECT:
        if (fv == 0) snprintf(buf, size, "Auto");
        else snprintf(buf, size, "%g", (double)fv);
        break;
    case SET_TEXT: snprintf(buf, size, "%s", (const char *)c + s->field); break;
    case SET_KEYS: {
        const int *k = field_int(c, s->field);
        size_t n = 0;
        if (size) *buf = 0;
        for (int i = 0; i < BIND_KEYS; i++)
            if (k[i] && n < size) n += (size_t)snprintf(buf + n, size - n, "%s%s", n ? ", " : "", config_key_name(k[i]));
        if (!n) snprintf(buf, size, "None");
        break;
    }
    }
}

bool config_valid(const ConfigSetting *s, const Config *c) {
    switch (s->kind) {
    case SET_BOOL: {
        int v = *field_int(c, s->field);
        return v == 0 || v == 1;
    }
    case SET_INT: {
        int v = *field_int(c, s->field);
        return v >= s->min && v <= s->max;
    }
    case SET_FLOAT:
    case SET_FACTOR: {
        float v = *field_float(s, c);
        return isfinite(v) && v >= s->min && v <= s->max;
    }
    case SET_CHOICE:
        for (int i = 0, n = nchoices(s); i < n; i++)
            if (choice_value(s, i) == *field_int(c, s->field)) return true;
        return false;
    case SET_SIZE: {
        int w = *field_int(c, s->field), h = *field_int(c, s->field2);
        return (!w && !h) || (w >= 320 && w <= 16384 && h >= 180 && h <= 16384);
    }
    case SET_ASPECT: {
        float v = *field_float(s, c);
        return isfinite(v) && (v == 0 || (v >= s->min && v <= s->max));
    }
    case SET_TEXT: {  // (one line, and no comment character: the file would read it back cut short)
        const char *t = (const char *)c + s->field;
        return memchr(t, 0, s->field2) && !strpbrk(t, "\r\n;#");
    }
    case SET_KEYS: {
        const int *k = field_int(c, s->field);
        for (int i = 0; i < BIND_KEYS; i++)
            if (!bind_key_valid(k[i])) return false;
        return true;
    }
    }
    return false;
}

bool config_equal(const ConfigSetting *s, const Config *a, const Config *b) {
    switch (s->kind) {
    case SET_FLOAT:
    case SET_FACTOR:
    case SET_ASPECT: return *field_float(s, a) == *field_float(s, b);
    case SET_SIZE:
        return *field_int(a, s->field) == *field_int(b, s->field) &&
               *field_int(a, s->field2) == *field_int(b, s->field2);
    case SET_TEXT: return !strcmp((const char *)a + s->field, (const char *)b + s->field);
    case SET_KEYS: return !memcmp(field_int(a, s->field), field_int(b, s->field), sizeof(int) * BIND_KEYS);
    default: return *field_int(a, s->field) == *field_int(b, s->field);
    }
}

bool config_restart_key(const char *key) {
    const ConfigSetting *s = config_setting(key);
    return s && s->restart;
}

bool config_range(const char *key, float *min, float *max) {
    const ConfigSetting *s = config_setting(key);
    if (!s || (s->kind != SET_INT && s->kind != SET_FLOAT && s->kind != SET_FACTOR && s->kind != SET_ASPECT)) return false;
    *min = s->min, *max = s->max;
    return true;
}

// What DEFAULT_INI says of a setting: the comment lines above its key (keys right below one another, 1Player..4Players,
// share them). A line wrapped in the file is joined to the one before (that one long, not ending a sentence);
// "Default: x" goes last, left out when x is empty.
bool config_help(const char *key, char *buf, size_t size) {
    const char *p = DEFAULT_INI, *block = NULL;  // block: the comment lines that go with the next key
    bool after_key = false;
    size_t klen = strlen(key);
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if (*p == ';') {
            if (!block || after_key) block = p;
            after_key = false;
        } else if (p == end || *p == '[') {
            block = NULL, after_key = false;
        } else {
            after_key = true;
            if (block && (size_t)(end - p) > klen && !strncmp(p, key, klen) && (p[klen] == ' ' || p[klen] == '=')) break;
        }
        p = *end ? end + 1 : end;
    }
    if (!*p || !block || !size) return false;
    size_t n = 0;
    char dflt[128] = "", prev[256] = "";
    *buf = 0;
    for (const char *l = block; *l == ';';) {
        const char *end = strchr(l, '\n');
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(end - l - 1), l + 1);
        const char *t = line + (*line == ' ');
        if (!strncmp(t, "Default:", 8)) {
            snprintf(dflt, sizeof dflt, "%s", t + 8 + (t[8] == ' '));
        } else {
            size_t pl = strlen(prev);
            bool list = strchr(t, ',') != NULL;  // (the values it takes, "Off, 2x, 4x": a line of its own)
            for (const char *c = t; *c && list; c++) list = *c != ' ' || c[-1] == ',';
            bool join = pl >= 80 && !strchr(".!?:)", prev[pl - 1]) && strncmp(t, "Between ", 8) && !list;
            if (n) n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s", join ? " " : "\n");
            n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s", t);
            snprintf(prev, sizeof prev, "%s", line);
        }
        l = end + 1;
    }
    if (*dflt) n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%sDefault: %s", n ? "\n" : "", dflt);
    return n > 0;
}

static int desktop_w, desktop_h;
static bool render_auto = true;

static void set_key(const char *key, char *val, int line) {
    char k[64], v[256];
    snprintf(k, sizeof k, "%s", key);
    snprintf(v, sizeof v, "%s", val);
    squash(k);
    squash(v);
    if (!strncmp(k, "quest", 5) && isdigit((unsigned char)k[5])) {  // [Monster HP] Quest<number> = factors
        HpScale *h = &g_config.hp;
        char *end;
        unsigned long q = strtoul(k + 5, &end, 10);
        float f[4];
        int n = parse_factors(v, f);
        if (*end || !q || (n != 1 && n != 4)) {
            LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
            return;
        }
        int i = 0;
        while (i < h->quests && h->quest[i].quest != q) i++;
        if (i == HP_QUESTS_MAX) { LOG("[config] line %d: more than %d quests set apart\n", line, HP_QUESTS_MAX); return; }
        h->quest[i].quest = (uint32_t)q;
        for (int j = 0; j < 4; j++) h->quest[i].scale[j] = f[n == 4 ? j : 0];
        if (i == h->quests) h->quests++;
        return;
    }
    const ConfigSetting *s = config_setting(key);
    if (!s) { LOG("[config] line %d: unknown setting \"%s\" (ignored)\n", line, key); return; }
    if (!config_parse(s, val, &g_config))
        LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// The largest size inside w x h matching the given aspect ratio, even numbers.
static void fit_aspect(int w, int h, float aspect, int *rw, int *rh) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if ((float)w / (float)h > aspect)
        w = (int)((float)h * aspect + 0.5f);
    else
        h = (int)((float)w / aspect + 0.5f);
    *rw = w & ~1, *rh = h & ~1;
}

// Sections of DEFAULT_INI the file lacks (written by an older build) are added with their defaults at its end; the
// player's own settings stay as they are.
static void append_missing_sections(const char *path, const char *seen) {
    FILE *o = NULL;
    for (const char *p = strstr(DEFAULT_INI, "\n["); p;) {
        const char *start = p + 1, *end = strstr(start, "\n["), *close = strchr(start, ']');
        p = end;
        char name[64], key[70];
        snprintf(name, sizeof name, "%.*s", (int)(close - start - 1), start + 1);
        for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
        snprintf(key, sizeof key, "|%s|", name);
        if (strstr(seen, key)) continue;
        if (!o && !(o = fopen(path, "a"))) return;
        fprintf(o, "\n%.*s", (int)(end ? end - start : (long)strlen(start)), start);
        LOG("[config] added the [%s] section to %s\n", name, path);
    }
    if (o) fclose(o);
}

void config_load(void) {
    char path[1100];
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *o = fopen(path, "w");
        if (o) { fputs(DEFAULT_INI, o); fclose(o); LOG("[config] wrote the default settings to %s\n", path); }
        else LOG("[config] %s is missing and could not be written; using the defaults\n", path);
    } else {
        char buf[512], seen[512] = "|";
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (*s == '[' && strchr(s, ']') && strlen(seen) + strlen(s) + 2 < sizeof seen) {
                for (char *q = s + 1; *q != ']'; q++) {
                    size_t n = strlen(seen);
                    seen[n] = (char)tolower((unsigned char)*q), seen[n + 1] = 0;
                }
                strcat(seen, "|");
            }
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
        append_missing_sections(path, seen);
    }

    // Keep Auto/Desktop and file values, rather than persisting temporary environment overrides.
    render_auto = !g_config.render_w;
    config_store_loaded(&g_config);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    desktop_w = dm ? dm->w : 1920, desktop_h = dm ? dm->h : 1080;
    if (!g_config.width) g_config.width = desktop_w, g_config.height = desktop_h;

    const char *e = getenv("MHGU_WINDOW");
    int w, h;
    if (e && parse_size(e, &w, &h)) g_config.display_mode = DISPLAY_WINDOWED, g_config.width = w, g_config.height = h;
    if (getenv("MHGU_HIDDEN")) g_config.display_mode = DISPLAY_WINDOWED;
    if ((e = getenv("MHGU_MSAA"))) g_config.msaa = atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : atoi(e) >= 2 ? 2 : 1;
    if ((e = getenv("MHGU_FXAA"))) g_config.fxaa = atoi(e) != 0;
    if ((e = getenv("MHGU_RT16"))) g_config.smooth_gradients = atoi(e) != 0;
    if ((e = getenv("MHGU_BACKGROUND_INPUT"))) g_config.background_input = atoi(e) != 0;
    if ((e = getenv("MHGU_PAD_POSITIONAL"))) g_config.face_buttons = atoi(e) ? FACE_POSITION : FACE_LABEL;
    if ((e = getenv("MHGU_LDN_PORT")) && atoi(e) > 0) g_config.ldn_port = atoi(e);
    if ((e = getenv("MHGU_SYNC_ZAKO"))) g_config.sync_small_monsters = atoi(e) != 0;
    if ((e = getenv("MHGU_LDN_JOIN"))) snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", e);
    if ((e = getenv("MHGU_LDN_SERVER"))) snprintf(g_config.ldn_server, sizeof g_config.ldn_server, "%s", e);
    if ((e = getenv("MHGU_LDN_KEY"))) snprintf(g_config.ldn_key, sizeof g_config.ldn_key, "%s", e);
    if ((e = getenv("MHGU_ASPECT"))) parse_aspect(e, &g_config.aspect_ratio);
    if ((e = getenv("MHGU_FOV")) && strtof(e, NULL) >= 0.5f && strtof(e, NULL) <= 2.0f) g_config.fov = strtof(e, NULL);
    if ((e = getenv("MHGU_UI_ANCHOR"))) config_parse(config_setting("UIAnchor"), e, &g_config);
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;
    if ((e = getenv("MHGU_FSR"))) g_config.fsr = atoi(e) != 0;  // (test runs: MHGU_FSR=0|1, MHGU_FSR_QUALITY=quality...)
    if ((e = getenv("MHGU_FSR_UPSCALER"))) g_config.fsr_upscaler = !strcmp(e, "4") || !strcasecmp(e, "fsr4") ? FSR_UP_4 : FSR_UP_3;
    if ((e = getenv("MHGU_FSR_QUALITY"))) config_parse(config_setting("FSRQuality"), e, &g_config);
    if ((e = getenv("MHGU_FSR_OBJMOTION"))) g_config.fsr_objmotion = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_ANIMSURF"))) g_config.fsr_animsurf = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_SHARPNESS")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_sharpness = strtof(e, NULL);
    if ((e = getenv("MHGU_FSR_REACTIVE")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_reactive = strtof(e, NULL);

    int tw = g_config.width, th = g_config.height;  // (Borderless shows the image at the desktop size)
    if (g_config.display_mode == DISPLAY_BORDERLESS) tw = tw < desktop_w ? tw : desktop_w, th = th < desktop_h ? th : desktop_h;
    // The picture's shape: the window's or AspectRatio's, at least 16:10. The game lays its HUD out for 16:9, which
    // still fits down to 16:10 (a little flatter, nvn/vulkan/exec.c), so a narrower window (4:3) shows 16:10 with bars.
    // Wider than 16:9 shows more to the sides, 16:9 to 16:10 more above and below (camera_fovy, gen.py PATCHES).
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : (float)tw / (float)th;
    if (aspect < 1.6f - 0.005f) {
        if (g_config.aspect_ratio > 0)
            LOG("[config] AspectRatio %.3f is narrower than 16:10, which the HUD needs: using 16:10\n", aspect);
        aspect = 1.6f;
    }
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;  // (1366x768 and the like)
    if (render_auto && g_config.fsr && g_config.display_mode == DISPLAY_BORDERLESS && (tw < desktop_w || th < desktop_h)) {
        // FSR, Borderless: it upscales straight to the size the picture is shown at (the screen, in the picture's shape)
        // instead of to Resolution, which the window would then stretch again (blurring what FSR rebuilt)
        fit_aspect(desktop_w, desktop_h, aspect, &w, &h);
        LOG("[config] FSR, Borderless: upscaling to %dx%d (the screen, in the picture's shape) instead of Resolution %dx%d\n",
            w, h, tw, th);
    } else if (render_auto) {
        fit_aspect(tw, th, aspect, &w, &h);
    } else {
        fit_aspect(g_config.render_w, g_config.render_h, aspect, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not the picture's shape (%.3f): using %dx%d\n", g_config.render_w,
                g_config.render_h, aspect, w, h);
    }
    if (g_config.fsr) {  // FSR: that size is its output; the game renders smaller (runtime/nvn/vulkan/fsr.c)
        static const float ratio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        static const char *const names[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
        g_fsr_output[0] = (uint32_t)w, g_fsr_output[1] = (uint32_t)h;
        float r = ratio[g_config.fsr_quality];
        fit_aspect((int)(w / r + 0.5f), (int)(h / r + 0.5f), aspect, &w, &h);
        if (g_config.msaa > 1) LOG("[config] FSR is on: MSAA off (FSR does the anti-aliasing)\n");
        if (g_config.fxaa) LOG("[config] FSR is on: FXAA off (FSR does the anti-aliasing)\n");
        g_config.msaa = 1, g_config.fxaa = 0;
        // textures are sampled for the output size (AMD's recommendation: log2(render / output)); MHGU_FSR_LODBIAS
        // overrides (A/B)
        g_fsr_lod_bias = log2f((float)w / (float)g_fsr_output[0]);
        if ((e = getenv("MHGU_FSR_LODBIAS"))) g_fsr_lod_bias = strtof(e, NULL);
        LOG("[config] FSR %s %s (x%.1f): the 3D picture at %dx%d, upscaled to %ux%u, sharpness %.2f, texture LOD bias "
            "%.2f\n", g_config.fsr_upscaler == FSR_UP_4 ? "4 (3.1 if it cannot start)" : "3.1", names[g_config.fsr_quality], r, w, h, g_fsr_output[0], g_fsr_output[1], g_config.fsr_sharpness,
            g_fsr_lod_bias);
    }
    g_config.render_w = w, g_config.render_h = h;
    g_render_size[0] = (uint32_t)w;
    g_render_size[1] = (uint32_t)h;
    g_render_aspect = aspect;
    g_fov_scale = g_config.fov;

    // The HUD's width: 16:9 (Center), 21:9, 32:9 or the picture's (Edges), at most the picture's; the game's 2D canvas
    // gets that width (gen.py PATCHES), exec.c places the HUD's parts in it.
    float ui = g_config.ui_anchor == UI_EDGES  ? aspect
             : g_config.ui_anchor == UI_32_9 ? 32.0f / 9.0f
             : g_config.ui_anchor == UI_21_9 ? 21.0f / 9.0f
                                             : 16.0f / 9.0f;
    g_ui_anchor_aspect = ui < aspect ? ui : aspect;
    g_ui_size[1] = 720;
    g_ui_size[0] = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
    if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;

    config_runtime_init(&g_config);

    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d (shape %.3f, FOV x%.2f, HUD %.3f wide), %d fps, vsync %s, MSAA %dx, FXAA %s, "
        "face buttons by %s, controller %s in the background\n", path, modes[g_config.display_mode], g_config.width,
        g_config.height, g_config.render_w, g_config.render_h, g_render_aspect, g_fov_scale, g_ui_anchor_aspect,
        g_config.fps, g_config.vsync ? "on" : "off",
        g_config.msaa, g_config.fxaa ? "on" : "off", g_config.face_buttons == FACE_LABEL ? "label" : "position",
        g_config.background_input ? "on" : "off");
    const HpScale *hp = &g_config.hp;
    LOG("[config] monster HP in Hub quests: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, %d quest(s) set apart\n",
        hp->scale[0], hp->scale[1], hp->scale[2], hp->scale[3], hp->quests);
    LOG("[config] Village quests at the Hub counter: %s (their HP x%.2f first)\n", g_config.village_in_hub ? "on" : "off",
        hp->village);
}
