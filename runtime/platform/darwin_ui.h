// The macOS settings UI's shared actions (runtime/platform/darwin_menu.m), for the Settings menu and the Settings window
// (runtime/platform/darwin_settings.m).
#pragma once

#ifdef __OBJC__
#import <Foundation/Foundation.h>
static inline NSString *str(const char *s) { return [NSString stringWithUTF8String:s ? s : ""]; }
// A setting's choice values or labels ('|' between them, runtime/config.h)
static inline NSArray<NSString *> *split(const char *s) { return s ? [str(s) componentsSeparatedByString:@"|"] : @[]; }
#endif

void plat_edit_settings(void);      // the settings file in TextEdit
void plat_open_home(void);          // the settings folder (the game's home: mods/, cheats/, log.txt...) in Finder
const char *plat_home(void);        // that folder
void plat_settings_show(void);      // the Settings window (darwin_settings.m)
void plat_settings_refresh(void);   // after a change elsewhere (the menu): the window shows the file's values again
const char *plat_screen_size(void); // this screen's size in pixels below the notch, "WxH" (NULL: unknown)
void plat_settings_snapshot(const char *dir);  // (debug) each tab of the window as <dir>/settings_<n>.png
