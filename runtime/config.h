// Player settings: mhgurecomp.ini in the game folder (written with the defaults when it is missing).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DISPLAY_WINDOWED, DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS };
enum { FACE_POSITION, FACE_LABEL };
enum { COMBO_LENIENT, COMBO_STRICT };  // [Controls] ComboInputWindow (runtime/hle/combowin.c)
enum { MENU_CONFIRM_ORIGINAL, MENU_CONFIRM_SOUTH, MENU_CONFIRM_EAST };
// [Anti-aliasing] AntiAliasing: what draws in place of the game's FXAA pass (nvn/vulkan/exec.c): nothing (its input
// copied), the pass itself, FXAA 3.11, CMAA2 (nvn/vulkan/cmaa2.c), SMAA 1x (nvn/vulkan/smaa.c); SMAA T2x: nothing
// there, the jittered camera's frames resolved at the start of the HUD instead (nvn/vulkan/fsr.c); SMAA 4x: as T2x,
// with the scene in MSAA 2x (whatever MSAA says) resolved by SMAA S2x
enum { AA_OFF, AA_GAME, AA_FXAA, AA_CMAA2, AA_SMAA, AA_SMAA_T2X, AA_SMAA_4X };
enum { SMAA_LOW, SMAA_MEDIUM, SMAA_HIGH, SMAA_ULTRA };      // [Anti-aliasing] SMAAQuality: SMAA's presets
enum { CMAA2_LOW, CMAA2_MEDIUM, CMAA2_HIGH, CMAA2_ULTRA };  // [Anti-aliasing] CMAA2Quality: CMAA2's presets

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
    int aa;                   // AA_*: the game's FXAA pass as it is, skipped, or replaced by FXAA 3.11 or SMAA
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
    // [Anti-aliasing] AntiAliasing = FXAA: FXAA 3.11 Quality's preset (1: 10, 2: 13, 3: 15, 4: 29, 5: 39, as UE5's
    // r.FXAA.Quality), sub-pixel blur, and the local contrast an edge needs (relative, and below which dark edges stay)
    int fxaa_quality;
    float fxaa_subpix, fxaa_edge_threshold, fxaa_edge_threshold_min;
    int smaa_quality;  // [Anti-aliasing] AntiAliasing = SMAA: SMAA_*
    // [Anti-aliasing] AntiAliasing = CMAA2: CMAA2_*, and its variant that keeps more of text and fine detail
    int cmaa2_quality, cmaa2_extra_sharpness;
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
enum { SET_BOOL, SET_INT, SET_FLOAT, SET_FACTOR, SET_CHOICE, SET_SIZE, SET_ASPECT, SET_TEXT };
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
int config_live_aa(void);
int config_live_smaa_quality(void);
int config_live_cmaa2_quality(void);
int config_live_cmaa2_extra_sharpness(void);
int config_live_msaa(void);
int config_live_vsync(void);
int config_live_face_buttons(void);
int config_live_menu_confirm(void);
int config_live_background_input(void);
int config_live_combo_window(void);
void config_live_ldn(char *server, size_t server_size, char *key, size_t key_size);  // [Multiplayer] Server, Key
bool config_restart_required(const Config *draft); // Raw cold fields vs this process's startup INI values.
// Quick apply: the settings menu's unsaved hot values made live (config_preview), and the live values from before
// it put back when the menu closes without saving them (config_preview_end; nothing to do after a save).
void config_preview(const Config *draft);
void config_preview_end(void);
float config_audio_gain(void);  // atomic, safe on audio threads; 0 while muted in the background
void config_window_focus(bool focused);  // the window got / lost the focus (runtime/host_input.c)
int config_language(void);     // last saved UI language, atomic across host threads
bool config_confirm_exit(void); // atomic, safe on the host main thread

#ifdef __cplusplus
}
#endif
