/*
 * GenBridge Monitor - route an input pair to an output pair with as little delay as two clocks allow.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// Notes: Docs/code-notes/gmApp.m.md - "// notes §k" refers there.

// notes §1

#import <Cocoa/Cocoa.h>

#include "gmDraw.h"
#include "gmEngine.h"
#include "gmSettings.h"
#include "synthlibPanelView.h"

#ifndef GM_VERSION_STRING
#define GM_VERSION_STRING    "dev"
#endif

static tGmConfig gConfig;
static bool      gRunning       = false;
static bool      gDisplayPaused = false;

static void apply_running(void) {
    if (gRunning) {
        gm_engine_start(&gConfig);
    } else {
        gm_engine_stop();
    }
    gm_settings_save(&gConfig, gRunning);
}

static bool panel_paused(void * user) {
    (void)user;
    return gDisplayPaused;
}

static void panel_frame(void * user, int pixelWidth, int pixelHeight) {
    (void)user;
    gm_draw_frame(&gConfig, gRunning, gDisplayPaused, pixelWidth, pixelHeight);
}

static bool panel_click(void * user, double x, double y) {
    (void)user;

    switch (gm_draw_click(&gConfig, gRunning, x, y)) {
        case eGmEditConfig:
            apply_running();     // a running monitor restarts on the new routing; a stopped one just saves
            return true;

        case eGmEditTrim:
            gm_engine_set_trim(gConfig.trim);
            gm_settings_save(&gConfig, gRunning);
            return true;

        case eGmEditRun:
            gRunning = !gRunning;
            apply_running();
            return true;

        case eGmEditDisplay:
            gDisplayPaused = !gDisplayPaused;    // the view stops or restarts its timer after this click
            gm_settings_save_display(gDisplayPaused);
            return true;

        default:
            return false;
    }
}

static const tSynthLibPanel gPanel = {
    .canvasWidth = GM_CANVAS_W,
    .init        = gm_draw_init,
    .sync        = NULL,
    .frame       = panel_frame,
    .click       = panel_click,
    .pointer     = gm_draw_set_mouse,
    .menuActive  = gm_draw_menu_active,
    .paused      = panel_paused,
};

@interface GmAppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow * window;
@property (strong) NSTimer *  poller;
@end

@implementation GmAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    gm_settings_load(&gConfig, &gRunning);
    gDisplayPaused = gm_settings_load_display();

    NSRect rect = NSMakeRect(0, 0, GM_CANVAS_W, GM_CANVAS_H);

    self.window = [[NSWindow alloc] initWithContentRect:rect
                                              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         | NSWindowStyleMaskMiniaturizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"GenBridge Monitor";
    self.window.contentView = (__bridge_transfer NSView *)synthlib_panel_view_create(&gPanel, NULL, GM_CANVAS_W, GM_CANVAS_H);
    [self.window center];
    [self.window setFrameAutosaveName:@"GenBridgeMonitorWindow"];
    [self.window makeKeyAndOrderFront:nil];

    // notes §2
    self.poller = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer * timer) {
        (void)timer;
        gm_engine_poll();
    }];

    [NSApp activateIgnoringOtherApps:YES];

    // notes §4 - after the window is up, never before: opening a device waits on coreaudiod
    if (gRunning) {
        dispatch_async(dispatch_get_main_queue(), ^{
            gm_engine_start(&gConfig);
        });
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    gm_settings_save(&gConfig, gRunning);
    gm_engine_stop();     // puts back any buffer size it changed
}

@end

static void build_menu(void) {
    NSMenu *     bar     = [[NSMenu alloc] init];
    NSMenuItem * appItem = [[NSMenuItem alloc] init];
    NSMenu *     appMenu = [[NSMenu alloc] init];
    NSString *   about   = [NSString stringWithFormat:@"GenBridge Monitor %s", GM_VERSION_STRING];

    [bar addItem:appItem];
    [appMenu addItemWithTitle:about action:nil keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide GenBridge Monitor" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit GenBridge Monitor" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem * windowItem = [[NSMenuItem alloc] init];
    NSMenu *     windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];

    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    windowItem.submenu = windowMenu;
    [bar addItem:windowItem];

    NSApp.mainMenu = bar;
}

int main(int argc, const char * argv[]) {
    (void)argc;
    (void)argv;

    @autoreleasepool {
        // notes §3 - one instance: a second would route the same signal again and fight over the settings file
        NSString *  bundleId = [[NSBundle mainBundle] bundleIdentifier];
        pid_t       me       = [[NSProcessInfo processInfo] processIdentifier];

        for (NSRunningApplication * other in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId]) {
            if (other.processIdentifier != me) {
                [other activateWithOptions:0];
                return 0;
            }
        }

        NSApplication * app      = [NSApplication sharedApplication];
        GmAppDelegate * delegate = [[GmAppDelegate alloc] init];

        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        build_menu();
        app.delegate = delegate;
        [app run];
    }

    return 0;
}
