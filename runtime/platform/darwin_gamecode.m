// The game code on a player's Mac (platform.h: plat_gamecode_ready). A release app carries none (tools/make_app.py
// --release): the first time it starts with the game files in place, and again when an update of the app changes the
// game code id (runtime/gamelib.h), it builds the library from them into <the player's folder>/gamecode/
// mhgugame-<id>.dylib with tools/build_gamecode.py (in Contents/Resources/gamecode), using macOS's own python3 and
// clang from Apple's Command Line Developer Tools: when those aren't there it offers Apple's installer and waits.
// A window shows how far it is; the builder's output goes to gamecode/build-log.txt.
#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamelib.h"
#include "../platform.h"
#include "darwin_ui.h"

// A tool's output (nil: it failed), without asking anything of the player.
static NSString *tool_output(NSString *path, NSArray<NSString *> *args) {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:path];
    t.arguments = args;
    NSPipe *out = [NSPipe pipe];
    t.standardOutput = out;
    t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil]) return nil;
    NSData *d = [out.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    if (t.terminationStatus != 0) return nil;
    return [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static NSString *xcrun(NSArray<NSString *> *args) {
    return tool_output(@"/usr/bin/xcrun", [@[ @"--sdk", @"macosx" ] arrayByAddingObjectsFromArray:args]);
}

// The Command Line Developer Tools (or Xcode): xcode-select has a folder (asked first: xcrun without one offers
// Apple's installer itself), and xcrun finds macOS's SDK, clang and python3 in it.
static NSString *python_path(void) {
    if (!tool_output(@"/usr/bin/xcode-select", @[ @"-p" ]) || !xcrun(@[ @"--show-sdk-path" ]) ||
        !xcrun(@[ @"--find", @"clang" ]))
        return nil;
    return xcrun(@[ @"--find", @"python3" ]);
}

// ---- the window
static NSWindow *g_win;
static NSTextField *g_status;
static NSProgressIndicator *g_bar;
static bool g_quit;

@interface MHGUGamecodeWindow : NSObject
@end
@implementation MHGUGamecodeWindow
- (void)quit:(id)sender { (void)sender; g_quit = true; }
@end
static MHGUGamecodeWindow *g_target;

static void window_open(NSString *info) {
    if (!g_win) {
        g_target = [MHGUGamecodeWindow new];
        g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 150) styleMask:NSWindowStyleMaskTitled
                                              backing:NSBackingStoreBuffered defer:NO];
        g_win.title = @"MHGU Recomp";
        g_win.releasedWhenClosed = NO;
        NSView *v = g_win.contentView;
        NSTextField *label = [NSTextField wrappingLabelWithString:info];
        label.frame = NSMakeRect(20, 86, 440, 48);
        label.tag = 1;
        [v addSubview:label];
        g_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 62, 440, 20)];
        g_bar.style = NSProgressIndicatorStyleBar;
        g_bar.indeterminate = NO;  // (a new one is indeterminate: it shows no value, and moves only when animated)
        g_bar.minValue = 0, g_bar.maxValue = 1;
        [v addSubview:g_bar];
        g_status = [NSTextField labelWithString:@""];
        g_status.frame = NSMakeRect(20, 40, 440, 18);
        g_status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        g_status.textColor = NSColor.secondaryLabelColor;
        [v addSubview:g_status];
        NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:g_target action:@selector(quit:)];
        quit.frame = NSMakeRect(380, 8, 84, 28);
        [v addSubview:quit];
        [g_win center];
    }
    ((NSTextField *)[g_win.contentView viewWithTag:1]).stringValue = info;
    [g_win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

static void window_close(void) {
    [g_win orderOut:nil];
    g_win = nil;
}

// AppKit's events for a while (the window, the Quit button, the builder's lines through the main queue).
static void pump(double seconds) {
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    for (;;) @autoreleasepool {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!e) return;
        [NSApp sendEvent:e];
    }
}

// ---- Apple's Command Line Developer Tools
static NSString *wait_for_tools(void) {
    NSString *python = python_path();
    if (python) return python;
    NSAlert *a = [NSAlert new];
    a.messageText = @"MHGU Recomp needs Apple's Command Line Developer Tools";
    a.informativeText = @"The first time it starts, MHGU Recomp builds the game's code from your own game files, on this "
                        @"Mac. That takes Apple's free Command Line Developer Tools, installed once. The game continues "
                        @"by itself when they are in place.";
    [a addButtonWithTitle:@"Install"];
    [a addButtonWithTitle:@"Quit"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    NSTask *install = [NSTask new];  // (Apple's own dialog and download)
    install.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcode-select"];
    install.arguments = @[ @"--install" ];
    [install launchAndReturnError:nil];
    window_open(@"Waiting for Apple's Command Line Developer Tools to be installed. Then MHGU Recomp builds the "
                @"game's code from your game files (once).");
    g_bar.indeterminate = YES;
    [g_bar startAnimation:nil];
    g_status.stringValue = @"Follow Apple's installer to install them.";
    while (!(python = python_path())) {
        pump(2);
        if (g_quit) return nil;
    }
    [g_bar stopAnimation:nil];
    g_bar.indeterminate = NO;
    return python;
}

// ---- the build
static bool build(NSString *python, NSString *builder, NSString *game, NSString *out, NSString *exe, NSString *log) {
    window_open(@"Building the game's code from your game files on this Mac. This happens once, and again after an "
                @"update of MHGU Recomp; it takes a few minutes.");
    g_bar.doubleValue = 0;
    g_status.stringValue = @"Starting";
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:python];
    t.arguments = @[ @"-E", @"-s", builder, game, out, exe ];
    t.standardError = [NSFileHandle fileHandleForWritingAtPath:log];
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    __block NSMutableString *pending = [NSMutableString string];
    __block bool done = false;
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData;
        if (!d.length) { h.readabilityHandler = nil; return; }
        NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        dispatch_async(dispatch_get_main_queue(), ^{  // "progress <0..1> <what>" lines (tools/build_gamecode.py)
            [pending appendString:s ?: @""];
            NSRange nl;
            while ((nl = [pending rangeOfString:@"\n"]).location != NSNotFound) {
                NSString *line = [pending substringToIndex:nl.location];
                [pending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
                NSArray<NSString *> *w = [line componentsSeparatedByString:@" "];
                if (w.count >= 3 && [w[0] isEqualToString:@"progress"]) {
                    g_bar.doubleValue = w[1].doubleValue;
                    g_status.stringValue = [NSString stringWithFormat:@"%@ (%d%%)",
                        [[w subarrayWithRange:NSMakeRange(2, w.count - 2)] componentsJoinedByString:@" "],
                        (int)(w[1].doubleValue * 100)];
                }
            }
        });
    };
    t.terminationHandler = ^(NSTask *task) {
        (void)task;
        dispatch_async(dispatch_get_main_queue(), ^{ done = true; });
    };
    NSError *err = nil;
    if (![t launchAndReturnError:&err]) {
        window_close();
        plat_error_box([NSString stringWithFormat:@"Couldn't start the game code build: %@", err.localizedDescription].UTF8String);
        return false;
    }
    while (!done && !g_quit) pump(0.1);
    if (g_quit) {
        [t terminate];
        [t waitUntilExit];
        return false;
    }
    window_close();
    if (t.terminationStatus == 0) return true;
    NSString *text = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSString *last = [[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        componentsSeparatedByString:@"\n"].lastObject;
    NSAlert *a = [NSAlert new];
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = @"Building the game's code didn't work";
    a.informativeText = [NSString stringWithFormat:@"%@\n\nThe details are in gamecode/build-log.txt in "
                                                   @"~/Library/Application Support/MHGURecomp.", last ?: @""];
    [a addButtonWithTitle:@"Show Log"];
    [a addButtonWithTitle:@"Quit"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([a runModal] == NSAlertFirstButtonReturn)
        [NSWorkspace.sharedWorkspace selectFile:log inFileViewerRootedAtPath:@""];
    return false;
}

bool plat_gamecode_ready(const char *game_dir) {
    @autoreleasepool {
        char exe_c[PATH_MAX], home_c[PATH_MAX], game_c[PATH_MAX];
        if (!plat_exe_path(exe_c, sizeof exe_c)) return true;  // (gamelib_load says what's missing)
        NSString *exe = str(exe_c), *dir = exe.stringByDeletingLastPathComponent;
        NSFileManager *fm = NSFileManager.defaultManager;
        if ([fm fileExistsAtPath:[dir stringByAppendingPathComponent:@GAMELIB_NAME]]) return true;  // (make app's)
        NSString *res = [dir stringByAppendingPathComponent:@"../Resources/gamecode"].stringByStandardizingPath;
        NSString *builder = [res stringByAppendingPathComponent:@"build_gamecode.py"];
        if (![fm fileExistsAtPath:builder]) return true;
        // the player's folder: plat_standalone made it the current one
        if (!getcwd(home_c, sizeof home_c) || !realpath(game_dir, game_c)) return true;
        NSString *codedir = [str(home_c) stringByAppendingPathComponent:@"gamecode"];
        NSString *out = [codedir stringByAppendingPathComponent:
                                     [NSString stringWithFormat:@"mhgugame-%s.dylib", gamelib_id()]];
        setenv("MHGU_GAME_LIB", out.UTF8String, 0);  // (runtime/gamelib.c loads it from there)
        if ([fm fileExistsAtPath:out]) return true;
        [fm createDirectoryAtPath:codedir withIntermediateDirectories:YES attributes:nil error:nil];
        fprintf(stderr, "[gamecode] none for this runtime (%s): building %s\n", gamelib_id(), out.UTF8String);
        // SDL first: it sets up the application (its menus, its delegate) as it would for the game window
        if (!SDL_Init(SDL_INIT_VIDEO)) return true;
        NSString *python = wait_for_tools();
        if (!python) return false;
        bool ok = build(python, builder, str(game_c), out, exe, [codedir stringByAppendingPathComponent:@"build-log.txt"]);
        fprintf(stderr, "[gamecode] %s\n", ok ? "built" : g_quit ? "the player quit" : "failed: see gamecode/build-log.txt");
        return ok;
    }
}
