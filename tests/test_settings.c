// Host-only persistence regression test. No game files or live save are used.
// clang -Iruntime tests/test_settings.c runtime/config.c runtime/config_store.c -lSDL3 -o test-settings.exe
#include "config.h"
#include "menu_confirm.h"
#include "settings_locale.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define set_env(key, value) _putenv_s(key, value)
#else
#define set_env(key, value) setenv(key, value, 1)
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    char *text = malloc(size + 1); assert(text);
    assert(fread(text, 1, size, f) == (size_t)size); text[size] = 0;
    fclose(f); return text;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *path = argv[1];
    FILE *f = fopen(path, "wb"); assert(f);
    fputs("; Keep this comment\r\n[Display]\r\nMode = Borderless Window\r\nResolution = Desktop\r\nRenderResolution = Auto\r\n"
          "AspectRatio = 21:9\r\nFOV = 1.1 ; camera comment\r\n"
          "[Performance]\r\nFPS = 60\r\nFrameRate = 60\r\n[Monster HP]\r\nQuest101 = 0.6\r\nQuest102 = 1, 2, 3, 4\r\n"
          "[Future section]\r\nFutureOption = preserve-me", f);
    fclose(f);
    set_env("MHGU_CONFIG", path);
    set_env("MHGU_WINDOW", "900x600");
    set_env("MHGU_FOV", "1.6");
    set_env("MHGU_FRAMERATE", "144.5");
    Config initial = g_config;
    config_load();
    Config c = config_editable();
    assert(c.width == 0 && c.height == 0 && c.render_w == 0 && c.render_h == 0);
    assert(fabsf(c.fov - 1.1f) < 0.001f);
    assert(g_config.width == 900 && fabsf(g_config.fov - 1.6f) < 0.001f);
    assert(c.hp.quests == 2 && c.language == 0 && c.button_icons == 0);
    assert(!config_restart_required(&c)); // Raw Auto/Desktop survives environment resolution.
    assert(fabsf(config_live_fov()-1.6f)<0.001f && fabsf(config_live_fps()-144.5f)<0.001f);
    Config volume_only=c; volume_only.volume=35;
    char error[256];
    assert(config_save(&volume_only,error,sizeof error));
    assert(fabsf(config_live_fov()-1.6f)<0.001f && fabsf(config_live_fps()-144.5f)<0.001f);
    // Quick apply: unsaved hot values are live; one back at the file's value gets the override again; closing without
    // saving puts back what was live before.
    Config preview=c; preview.fps=90; preview.msaa=8; preview.volume=10;
    config_preview(&preview);
    assert(config_live_fps()==90 && config_live_msaa()==8 && fabsf(config_audio_gain()-0.1f)<0.001f);
    preview.fps=volume_only.fps; config_preview(&preview);
    assert(fabsf(config_live_fps()-144.5f)<0.001f && config_live_msaa()==8);
    config_preview_end();
    assert(fabsf(config_live_fps()-144.5f)<0.001f && config_live_msaa()==1 && fabsf(config_audio_gain()-0.35f)<0.001f);
    float camera_before=camera_fovy(0.5f,g_render_aspect);
    c.menu_confirm = MENU_CONFIRM_SOUTH; c.menu_scale = 170; c.fps = 120; c.fov = 1.2f; c.volume = 25; c.confirm_exit = 0; c.language = 1; c.button_icons = 2;
    c.aa=0; c.msaa=4; c.vsync=0; c.face_buttons=FACE_LABEL; c.background_input=0; c.mouse_sensitivity=2.5f;
    assert(config_save(&c,error,sizeof error) && !config_restart_required(&c));
    assert(config_live_fps()==120 && fabsf(config_live_fov()-1.2f)<0.001f);
    assert(config_live_msaa()==4 && !config_live_vsync() && !config_live_aa());
    assert(config_live_face_buttons()==FACE_LABEL && !config_live_background_input());
    assert(config_live_mouse_sensitivity()==2.5f && camera_fovy(0.5f,g_render_aspect)<camera_before);
    assert(g_config.fps==60 && g_config.aa==AA_FXAA && g_config.face_buttons==FACE_POSITION); // Boot snapshot is immutable.
    c.hp.quest[0] = c.hp.quest[1]; c.hp.quests = 1;
    assert(config_save(&c, error, sizeof error));
    assert(fabsf(config_audio_gain() - 0.25f) < 0.001f && !config_confirm_exit());
    assert(config_language() == 1);
    assert(config_restart_required(&c));
    Config restored=c; restored.hp=initial.hp; // Reconstruct the loaded quest overrides, not compiled defaults.
    restored.hp.quests=2; restored.hp.quest[0].quest=101;
    for(int i=0;i<4;i++) restored.hp.quest[0].scale[i]=0.6f;
    restored.hp.quest[1]=c.hp.quest[0];
    assert(!config_restart_required(&restored));
    assert(!strcmp(settings_text(1, "Language"), "Idioma"));
    assert(!strcmp(settings_text(0, "Language"), "Language"));
    assert(!strcmp(settings_text(1, "Duplicate quest ID."), "ID de misión repetido."));
    char *text = read_file(path);
    assert(strstr(text, "; Keep this comment\r\n"));
    assert(strstr(text, "; camera comment"));
    assert(strstr(text, "FutureOption = preserve-me"));
    assert(strstr(text, "Resolution = Desktop") && strstr(text, "RenderResolution = Auto"));
    // Unchanged values keep the player's spelling; changed numbers are written short.
    assert(strstr(text, "Mode = Borderless Window\r\n") && strstr(text, "AspectRatio = 21:9\r\n"));
    assert(strstr(text, "FOV = 1.2 ; camera comment") && strstr(text, "MouseSensitivity = 2.5\r\n"));
    assert(!strstr(text, "Quest101 ="));
    assert(strstr(text, "Quest102 = 1, 2, 3, 4"));
    assert(strstr(text, "FPS = 120") && strstr(text, "FrameRate = 120"));
    assert(strstr(text, "Volume = 25") && strstr(text, "ConfirmExit = Off"));
    assert(strstr(text, "Language = Spanish") && strstr(text, "ButtonIcons = PlayStation"));
    // Invalid edits must leave both the file and the live audio setting intact.
    Config bad=c;bad.menu_confirm=3;assert(!config_save(&bad,error,sizeof error));
    bad=c;bad.menu_scale=99;assert(!config_save(&bad,error,sizeof error));
    bad.menu_scale=201;assert(!config_save(&bad,error,sizeof error));
    bad = c; bad.button_icons = 3;
    assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.language = 2;
    assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.fov = NAN;
    assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.ldn_port = 0; assert(!config_save(&bad, error, sizeof error));
    bad = c; strcpy(bad.ldn_join, "host\nFPS=30"); assert(!config_save(&bad, error, sizeof error));
    bad = c; bad.hp.quests = 2; bad.hp.quest[1] = bad.hp.quest[0];
    assert(!config_save(&bad, error, sizeof error));
    char *after = read_file(path); assert(!strcmp(text, after)); free(after);
    set_env("MHGU_CONFIG", "missing-settings-parent/settings.ini");
    bad = c; bad.volume = 90; bad.language = 0; bad.fps=30; bad.fov=2; bad.msaa=1; bad.vsync=1; bad.aa=1; bad.face_buttons=FACE_POSITION; bad.background_input=1; bad.mouse_sensitivity=9;
    assert(!config_save(&bad, error, sizeof error));
    assert(fabsf(config_audio_gain() - 0.25f) < 0.001f);
    assert(config_live_fps()==120 && config_live_msaa()==4 && !config_live_vsync() && !config_live_aa());
    assert(config_live_face_buttons()==FACE_LABEL && !config_live_background_input() && config_live_mouse_sensitivity()==2.5f);
    assert(fabsf(config_live_fov()-1.2f)<0.001f);
    assert(config_language() == 1); // Failed writes must not change the UI language.
    set_env("MHGU_CONFIG", path);
    set_env("MHGU_WINDOW", ""); set_env("MHGU_FOV", ""); set_env("MHGU_FRAMERATE", "");
    g_config = initial; config_load();
    Config loaded = config_editable();
    assert(loaded.language == 1 && config_language() == 1 && loaded.button_icons == 2);
    assert(loaded.menu_scale == 170 && loaded.menu_confirm == MENU_CONFIRM_SOUTH);
    assert(config_live_menu_confirm() == MENU_CONFIRM_SOUTH);
    assert(loaded.fps == 120 && loaded.volume == 25 && loaded.hp.quests == 1);
    assert(loaded.hp.quest[0].quest == 102 && loaded.hp.quest[0].scale[3] == 4);
    assert(loaded.width == 0 && loaded.render_w == 0);
    assert(!loaded.confirm_exit && fabsf(loaded.fov - 1.2f) < 0.001f);
    assert(!config_restart_required(&loaded));
    assert(config_live_fps()==120 && config_live_msaa()==4 && config_live_face_buttons()==FACE_LABEL);
    assert(config_restart_key("RenderResolution") && !config_restart_key("FOV"));
    assert(!config_restart_key("MSAA") && !config_restart_key("VSync") && !config_restart_key("FaceButtons"));
    assert(config_restart_key("framerate") == config_restart_key("FPS") && !config_restart_key("NoSuchKey"));
    // A second save must not append duplicate sections or change resolved Auto settings.
    assert(config_save(&loaded, error, sizeof error));
    after = read_file(path); assert(!strcmp(text, after)); free(text); free(after);
    loaded.language = 0; loaded.button_icons = 1;
    assert(config_save(&loaded, error, sizeof error) && config_language() == 0);
    g_config = initial; config_load();
    assert(config_editable().language == 0 && config_language() == 0 && config_editable().button_icons == 1);
    bool waiting=true;
    assert(menu_release_buttons(0x41,0xf,&waiting)==0x40 && waiting);
    assert(menu_release_buttons(0x42,0xf,&waiting)==0x40 && waiting);
    assert(menu_release_buttons(0x40,0xf,&waiting)==0x40 && !waiting);
    assert(menu_release_buttons(0x42,0xf,&waiting)==0x42);
    int confirm, cancel;
    menu_confirm_buttons(MENU_CONFIRM_ORIGINAL,1,0,1,0,&confirm,&cancel);assert(confirm&&!cancel);
    menu_confirm_buttons(MENU_CONFIRM_SOUTH,1,0,0,1,&confirm,&cancel);assert(confirm&&!cancel);
    menu_confirm_buttons(MENU_CONFIRM_SOUTH,1,0,1,0,&confirm,&cancel);assert(!confirm&&cancel);
    menu_confirm_buttons(MENU_CONFIRM_EAST,0,1,0,1,&confirm,&cancel);assert(confirm&&!cancel);
    assert(menu_confirm_position(MENU_CONFIRM_SOUTH,1,0,0)==0);
    assert(menu_confirm_position(MENU_CONFIRM_SOUTH,1,0,1)==1);
    Config fsr_config=config_editable();
    assert(config_defaults().sync_small_monsters==0);
    assert(config_defaults().shader_prewarm==1);
    assert(config_defaults().draw_distance==1.0f);
    assert(config_defaults().mute_in_background==0);
    assert(config_defaults().combo_window==COMBO_LENIENT);
    assert(config_defaults().aa==AA_FXAA && config_defaults().fxaa_quality==5 && config_defaults().fxaa_subpix==0.0f);
    assert(config_defaults().fxaa_edge_threshold==0.063f && config_defaults().fxaa_edge_threshold_min==0.0312f);
    assert(config_defaults().smaa_quality==SMAA_ULTRA);
    {   // AntiAliasing's spellings, the older FXAA key's (On was FXAA 3.11) and FXAA 3.11's settings' ranges
        Config x=config_defaults();
        assert(config_setting("FXAA")==config_setting("AntiAliasing"));
        assert(config_parse(config_setting("AntiAliasing"),"SMAA",&x) && x.aa==AA_SMAA);
        assert(config_parse(config_setting("AntiAliasing"),"SMAAT2x",&x) && x.aa==AA_SMAA_T2X);
        assert(config_parse(config_setting("AntiAliasing"),"SMAA4x",&x) && x.aa==AA_SMAA_4X);
        assert(config_parse(config_setting("AntiAliasing"),"FXAA",&x) && x.aa==AA_FXAA);
        assert(config_parse(config_setting("FXAA"),"Game",&x) && x.aa==AA_GAME);
        assert(config_parse(config_setting("FXAA"),"off",&x) && x.aa==AA_OFF);
        assert(config_parse(config_setting("FXAA"),"On",&x) && x.aa==AA_FXAA);
        assert(config_parse(config_setting("SMAAQuality"),"high",&x) && x.smaa_quality==SMAA_HIGH);
        assert(!config_restart_key("SMAAQuality"));
        assert(config_parse(config_setting("FXAAQuality"),"3",&x) && x.fxaa_quality==3);
        assert(!config_parse(config_setting("FXAAQuality"),"0",&x) && !config_parse(config_setting("FXAASubpix"),"1.5",&x));
        assert(config_parse(config_setting("FXAAEdgeThresholdMin"),"0.0625",&x) && x.fxaa_edge_threshold_min==0.0625f);
        assert(config_restart_key("FXAAQuality") && !config_restart_key("FXAA"));
    }
    fsr_config.fsr=1;fsr_config.fsr_upscaler=FSR_UP_4;fsr_config.fsr_quality=FSR_BALANCED;
    fsr_config.fsr_sharpness=.65f;fsr_config.fsr_objmotion=0;fsr_config.fsr_reactive=.35f;fsr_config.fsr_animsurf=0;
    fsr_config.msaa=8;fsr_config.aa=1;
    assert(config_save(&fsr_config,error,sizeof error));assert(config_restart_required(&fsr_config));
    text = read_file(path);
    assert(strstr(text, "Sharpness = 0.65\r\n") && strstr(text, "Reactive = 0.35\r\n") && strstr(text, "Upscaler = FSR4\r\n"));
    free(text);
    g_config=initial;config_load();Config fsr_saved=config_editable();
    assert(fsr_saved.fsr && fsr_saved.fsr_upscaler==FSR_UP_4 && fsr_saved.fsr_quality==FSR_BALANCED);
    assert(fabsf(fsr_saved.fsr_sharpness-.65f)<.001f && fabsf(fsr_saved.fsr_reactive-.35f)<.001f);
    assert(!fsr_saved.fsr_objmotion && !fsr_saved.fsr_animsurf);
    assert(config_live_msaa()==1 && config_live_aa()==0);
    fsr_saved.volume=37;assert(config_save(&fsr_saved,error,sizeof error));
    assert(config_editable().msaa==8 && config_editable().aa==1);
    assert(config_live_msaa()==1 && config_live_aa()==0); // Saving cannot undo FSR's AA override.
    bad=fsr_saved;bad.fsr_quality=5;assert(!config_save(&bad,error,sizeof error));
    bad=fsr_saved;bad.fsr_upscaler=2;assert(!config_save(&bad,error,sizeof error));
    bad=fsr_saved;bad.fsr_sharpness=NAN;assert(!config_save(&bad,error,sizeof error));
    bad=fsr_saved;bad.fsr_reactive=1.1f;assert(!config_save(&bad,error,sizeof error));
    bad=fsr_saved;bad.fsr_objmotion=2;assert(!config_save(&bad,error,sizeof error));
    fsr_saved.hp.quests=HP_QUESTS_MAX;
    for(int i=0;i<HP_QUESTS_MAX;i++){fsr_saved.hp.quest[i].quest=50000+i;for(int j=0;j<4;j++)fsr_saved.hp.quest[i].scale[j]=1;}
    for(int quality=FSR_NATIVE;quality<=FSR_ULTRA_PERFORMANCE;quality++){
        fsr_saved.fsr_quality=quality;assert(config_save(&fsr_saved,error,sizeof error));
        g_config=initial;config_load();assert(config_editable().fsr_quality==quality);
        assert(config_editable().hp.quests==HP_QUESTS_MAX && config_editable().hp.quest[255].quest==50255);
    }
    fsr_saved.fsr=0;assert(config_save(&fsr_saved,error,sizeof error));
    assert(config_live_msaa()==1 && config_live_aa()==0); // Remains active until restart.
    g_config=initial;config_load();assert(config_live_msaa()==8 && config_live_aa()==1);
    remove(path);
    puts("PASS: INI round trip, Auto/Desktop, environment overrides, comments/unknown keys, duplicate aliases, quest removal, invalid input, atomic write failure, live volume and English/Spanish, icons, hot settings and restart classification.");
    return 0;
}
