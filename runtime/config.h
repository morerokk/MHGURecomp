// Player settings: mhgurecomp.ini in the game folder (written with the defaults when it is missing).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DISPLAY_WINDOWED, DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS };
enum { FACE_POSITION, FACE_LABEL };
enum { COMBO_LENIENT, COMBO_STRICT };  // [Controls] ComboInputWindow (runtime/hle/combowin.c)
enum { MENU_CONFIRM_ORIGINAL, MENU_CONFIRM_SOUTH, MENU_CONFIRM_EAST };
enum { FXAA_ORIGINAL, FXAA_311 };  // [Anti-aliasing] FXAAMode (runtime/nvn/vulkan/exec.c)

// [Keybinds]: the keyboard and mouse controls (runtime/host_input.c), in the file's order. Each takes up to two keys
// or mouse buttons: an SDL scancode, or KEY_MOUSE + SDL_BUTTON_* (0: none).
enum {
    BIND_MOVE_UP, BIND_MOVE_DOWN, BIND_MOVE_LEFT, BIND_MOVE_RIGHT, BIND_WALK,
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_DPAD_UP, BIND_DPAD_DOWN, BIND_DPAD_LEFT, BIND_DPAD_RIGHT, BIND_LSTICK, BIND_RSTICK, BINDS
};
#define BIND_KEYS 2
#define KEY_MOUSE 0x1000

// [Monster HP]: factors on the HP multiplier of Hub-type quests' large monsters
#define HP_QUESTS_MAX 256
#define HP_SCALE_MIN 0.05f
#define HP_SCALE_MAX 20.0f
typedef struct {
    float scale[4];  // by the number of hunters (index: hunters - 1)
    float village;   // Village quests posted in the Hub / Pub: this first, then scale ([Village Quests] HubHP)
    int quests;
    struct { uint32_t quest; float scale[4]; } quest[HP_QUESTS_MAX];  // per quest, in place of scale
} HpScale;

typedef struct {
    int display_mode;         // DISPLAY_*
    int width, height;        // screen / window size (Fullscreen: the monitor mode)
    int render_w, render_h;   // the game's 3D resolution (16:9)
    int fps;                  // frame rate cap: 30..240
    int vsync;                // 1: present without tearing (mailbox / FIFO, at most the refresh rate), 0: immediate
    int msaa;                 // samples of the 3D scene: 1 (off), 2, 4, 8
    int fxaa;                 // the game's own FXAA pass: 1 on, 0 off
    int face_buttons;         // FACE_*: controller face buttons by position (the Switch layout) or by their labels
    int ldn_port;             // local play: the port a hosted hub listens on (TCP; UDP for LAN search)
    char ldn_join[256];       // local play: hosts to search besides the LAN (host[:port], comma separated)
    int sync_small_monsters;  // multiplayer: small monsters synced like large ones (runtime/net/emsync.c)
    int bonus_content;        // free downloads copied from a donor save into saves without any (runtime/services/bonus.c)
    int village_in_hub;       // L / R at the Hub's / Pub's quest counter switch to Village quests (runtime/hle/villagehub.c)
    HpScale hp;               // large monster HP in Hub-type quests (runtime/hle/hpscale.c)
    float mouse_sensitivity;  // mouse look: the speed for full right stick tilt is divided by it (runtime/host_input.c)
    float aspect_ratio;       // the picture's shape (width / height), 0: the window's; never below 16:10 (config.c)
    float fov;                // 3D camera field of view factor (1: the game's)
    int ui_anchor;            // where the HUD goes on a picture wider than 16:9: UI_*
    int background_input;     // the controller keeps working while the window is in the background (runtime/ui.c)
    int volume;               // host master volume, 0..100; independent of the game's music / SFX settings
    int button_icons;         // 0: Xbox, 1: Switch, 2: PlayStation (visual guides only)
    int language;             // 0: English, 1: Spanish (host menu only)
    int menu_scale;           // host UI text/layout scale, 100..200 percent
    int menu_confirm;         // independent confirmation policy for the host settings menu
    int confirm_exit;         // ask before closing the application
    int fsr;                  // [Upscaler] FSR 3.1: 1 on (runtime/nvn/vulkan/fsr.c), 0 off
    int fsr_quality;          // FSR_*: the render scale
    float fsr_sharpness;      // FSR's RCAS sharpening, 0..1
    int fsr_objmotion;        // FSR: motion vectors of moving objects (characters, monsters): 1 on, 0 camera only
    float fsr_reactive;       // FSR: reactive mask strength on the game's moving-object mask, 0 (off) .. 1
    int fsr_animsurf;         // FSR: surfaces animated through their texture (banners, water): 1 detected, 0 off
    int fsr_upscaler;         // [Upscaler] Upscaler: FSR_UP_3 (FSR 3.1) or FSR_UP_4 (FSR 4, runtime/nvn/vulkan/fsr4.c)
    int updates;              // [Application] Updates: UPDATES_*, read by the updater (updater/updater.c), not the game
    int shader_prewarm;       // [Performance] ShaderPrewarm: translate the game's shaders at start-up (nvn/vulkan/exec.c)
    float draw_distance;      // [Graphics] DrawDistance: town NPCs' fade distance factor, 0 no limit (config.c npc_fade_dist2)
    int mute_in_background;   // [Audio] MuteInBackground: no sound while the window is not focused (config_audio_gain)
    int combo_window;         // [Controls] ComboInputWindow: COMBO_*, how long a press waits for a second button
    char ldn_server[256];     // [Multiplayer] Server: hub server for internet hubs (host[:port]); "" the default, "Off" none
    char ldn_key[64];         // [Multiplayer] Key: matchmaking key, hubs seen only by players with the same; "" public
    char chat_key[32];        // [Controls] ChatKey: the key that opens a line of chat (runtime/hle/chat.c), by SDL's
                              // name for it ("/", "Y"...); "" none
    int fxaa_mode;            // [Anti-aliasing] FXAAMode: FXAA_*, which FXAA draws while FXAA is on
    int binds[BINDS][BIND_KEYS];  // [Keybinds]: BIND_* -> its keys (an SDL scancode or KEY_MOUSE + button; 0 none)
} Config;
// The hub server used when ini Server is empty (runtime/net/ldn.c, server/): the project's public one (an Oracle
// Cloud VM; a DuckDNS name, so it can move without a release). "" = no default.
#define LDN_DEFAULT_SERVER "morerokk-mhgurecomp-ldnserver.duckdns.org"
enum { UPDATES_ASK, UPDATES_AUTO, UPDATES_OFF };
enum { UI_CENTER = 1, UI_21_9, UI_32_9, UI_EDGES };  // the middle 16:9, 21:9, 32:9, the picture's sides
enum { FSR_UP_3, FSR_UP_4 };  // [Upscaler] Upscaler
enum { FSR_NATIVE, FSR_QUALITY, FSR_BALANCED, FSR_PERFORMANCE, FSR_ULTRA_PERFORMANCE };  // output / render: 1, 1.5, 1.7, 2, 3

extern Config g_config;
extern uint32_t g_render_size[2];  // read by the game code (tools/gen.py PATCHES: its docked render size)
extern float g_render_aspect;      // the picture's shape, 16:10 or wider: the 3D cameras' aspect (tools/gen.py PATCHES)
extern float g_fov_scale;          // boot diagnostic value; camera_fovy reads config_live_fov()
extern uint32_t g_ui_size[2];      // the game's 2D canvas size (tools/gen.py PATCHES: 0xdbcd48, 0xdbcd54)
extern float g_ui_anchor_aspect;   // the HUD's width / height, 16:9 .. g_render_aspect (nvn/vulkan/exec.c)
extern uint32_t g_fsr_output[2];   // FSR on: the size it upscales to (what g_render_size would be without it), else 0
extern float g_fsr_lod_bias;       // FSR on: texture LOD bias for the lower render size, log2(render / output); else 0
float camera_fovy(float fovy, float aspect);  // a 3D camera's vertical field of view for its aspect (gen.py PATCHES)

void config_load(void);  // before the window and the guest start
void config_path(char *buf, size_t size);  // the settings file (MHGU_CONFIG, else mhgurecomp.ini)

// The settings of the file, one row each (config.c g_settings): what reads, writes and checks them, and what the
// settings menu (settings_panel.cpp) marks as needing a restart. Quest<number> entries ([Monster HP]) are apart: hp.quest.
enum { SET_BOOL, SET_INT, SET_FLOAT, SET_FACTOR, SET_CHOICE, SET_SIZE, SET_ASPECT, SET_TEXT, SET_KEYS };
typedef struct {
    const char *section, *key;  // [section] and key; "FPS|FrameRate": other spellings of the key it reads too
    int kind;                   // SET_*
    size_t field, field2;       // offsetof(Config, ...): the value (SET_SIZE: width; field2 its height)
    float min, max;             // SET_INT, SET_FLOAT, SET_FACTOR: the range
    // SET_CHOICE: the values as the file writes them, ',' between them, each with other spellings it reads after '|'
    // ('*' at the end: anything starting so); SET_SIZE: the spelling of 0x0 ("Desktop|Auto")
    const char *choices;
    const int *values;          // SET_CHOICE: each choice's value (NULL: 0, 1, 2...)
    bool restart;               // takes effect at the next start, not at once
    const char *error;          // config_validate's message for a bad value (English: settings_locale.h translates)
} ConfigSetting;
extern const ConfigSetting g_settings[];
extern const int g_nsettings;
const ConfigSetting *config_setting(const char *key);  // by key or another spelling of it; NULL: none
bool config_parse(const ConfigSetting *s, const char *value, Config *c);  // a value as the file has it into *c
void config_format(const ConfigSetting *s, const Config *c, char *buf, size_t size);  // as the file writes it
bool config_valid(const ConfigSetting *s, const Config *c);
bool config_equal(const ConfigSetting *s, const Config *a, const Config *b);
bool config_restart_key(const char *key);  // the setting takes effect at the next start
bool config_range(const char *key, float *min, float *max);  // a number setting's range
bool config_help(const char *key, char *buf, size_t size);  // what the default ini's comments say of it (the menu's help)
// [Keybinds]: a key's or mouse button's name as the file writes it and the menu shows it (SDL's, "Mouse Left"...;
// "" for one that has none), and the code a name is for (0: none of them)
const char *config_key_name(int code);
int config_key_code(const char *name);
const ConfigSetting *config_bind_setting(int bind);  // the [Keybinds] row of BIND_*

// File values, before environment overrides and automatic resolution are resolved.
void config_store_loaded(const Config *config);
Config config_editable(void);
Config config_defaults(void);
bool config_save(const Config *config, char *error, size_t error_size);
bool config_validate(const Config *config, char *error, size_t error_size);
// Immutable boot settings stay in g_config. These getters are atomic live values.
void config_runtime_init(const Config *effective); // After environment/Auto resolution at startup.
float config_live_fps(void);
float config_live_fov(void);
float config_live_mouse_sensitivity(void);
float config_live_draw_distance(void);
int config_live_fxaa(void);
int config_live_msaa(void);
int config_live_vsync(void);
int config_live_face_buttons(void);
int config_live_menu_confirm(void);
int config_live_background_input(void);
int config_live_combo_window(void);
int config_live_chat_key(void);  // [Controls] ChatKey as an SDL scancode (0: none)
int config_live_bind(int bind, int slot);  // [Keybinds]: one of BIND_*'s keys (Config.binds)
int config_live_fxaa_mode(void);
void config_live_ldn(char *server, size_t server_size, char *key, size_t key_size);  // [Multiplayer] Server, Key
bool config_restart_required(const Config *draft); // Raw cold fields vs this process's startup INI values.
float config_audio_gain(void);  // atomic, safe on audio threads; 0 while muted in the background
void config_window_focus(bool focused);  // the window got / lost the focus (runtime/host_input.c)
int config_language(void);     // last saved UI language, atomic across host threads
bool config_confirm_exit(void); // atomic, safe on the host main thread

// The macOS Settings window (runtime/platform/darwin_settings.m) and Settings menu (runtime/platform/darwin_menu.m):
// a row for every setting of the file they show, in file order (config.c g_setting_rows), and what reads and writes one
// setting by its key on top of the table above. The per-quest Quest<number> entries go through the same calls.
enum { SETTING_CHOICE, SETTING_VALUE, SETTING_TEXT };  // one of choices / a value (choices suggested) / free text
typedef struct {
    const char *tab, *section, *key, *title;  // the window's tab, its ini section and key, its title
    int kind;                                 // SETTING_*
    const char *choices;                      // the values offered, as written in the ini, '|' between them
    const char *labels;                       // their names, '|' between them (NULL: the values)
    bool live;                                // takes effect at once (config_set); otherwise at the next start
    bool menu;                                // offered in the Settings menu too
} SettingRow;
extern const SettingRow g_setting_rows[];
extern const int g_nsetting_rows;
// A setting's value as the file has it now (its default when the file lacks it), into buf; false: no such key.
bool config_get(const char *key, char *buf, size_t size);
// Two values of a setting that the parser reads the same ("1.0" == "1", case, UIAnchor "1" == "Center"); key NULL: as
// text and numbers only.
bool config_same_value(const char *key, const char *a, const char *b);
bool config_restart_pending(const char *key);  // its value in the file differs from the one the game started with
                                                // (NULL: any setting that applies at the next start)
// Writes the value into the file (in place: comments and other settings stay), into the saved values (config_editable,
// the live values) and, for a live setting, into g_config. The caller applies a live one where needed (runtime/ui.c
// ui_setting_changed). false: not a value the setting takes (nothing written), or the file could not be written. Also
// Quest<number> keys (Monster HP per quest).
bool config_set(const char *key, const char *value);
bool config_check(const char *key, const char *value);  // a value the setting takes? (g_config unchanged)
bool config_live(const char *key);  // takes effect at once
// The Quest<number> entries of the file, in order: fn(quest number, value, ctx) for each.
void config_quests(void (*fn)(unsigned quest, const char *value, void *ctx), void *ctx);
bool config_remove(const char *key);  // removes a Quest<number> entry (from the file, and from g_config)

#ifdef __cplusplus
}
#endif
