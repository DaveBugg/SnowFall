// snowfall_ui_demo — AppKit harness for the SwiftUI module with a FAKE app behind SFAppActions.
//
//   snowfall_ui_demo                         interactive: settings window + menu-bar item with the quick panel
//   snowfall_ui_demo --selftest <out-dir>    runs the UI self-test (SFUISelfTest), writes PNGs + report.txt,
//                                            exit code = number of failed checks
//
// It links snowfall_ui exactly like the app does and finds the Swift classes by name only
// (NSClassFromString), so it also proves the integration path of the real app shell.
#import <AppKit/AppKit.h>

#include <cmath>
#include <cstring>
#include <cwchar>

#import "SFBridge.h"
#import "SFSelfTest.h"
#include "core/skins.h"
#import "demo_strings.h"

// ------------------------------------------------------------------------------------------------
// Settings as the app would send them (core/settings/settings_json.cpp format) plus keys this UI
// does not know: they must survive every edit.
static const char kInitialSettingsText[] = R"JSON({
  "schemaVersion": 1, "enabled": true, "startupWithWindows": false,
  "weather": {"preset": "normal", "intensity": 55, "wind": 10, "gustiness": 20, "sizePercent": 100,
              "fallSpeedPercent": 100, "opacityPercent": 100, "skyPerMonitor": true, "futureKnob": 7},
  "accumulation": {"enabled": true, "onWindows": true, "onTaskbar": true, "onScreenBottom": true,
                   "maxHeightPxAt96Dpi": 36, "shakeOffOnMove": true, "skipMaximized": true, "dropSnowOnClose": true,
                   "driftOpacityPercent": 90, "shakeToClear": false, "naturalCaps": true, "fallingSnowmen": true,
                   "snowmenIntervalMinutes": 2, "snowmenNeedHeavySnow": true, "snowmenMaxAlive": 3, "snowplow": true,
                   "snowplowDelayMinutes": 10, "snowplowAllTaskbars": true, "icicles": true},
  "interaction": {"enabled": true, "holdKeyEnabled": true, "tool": "snowball", "snowmanSkin": "random",
                  "brushRadiusPxAt96Dpi": 28, "shovelWidthPxAt96Dpi": 96, "blockClicksWhileHeld": true,
                  "objectsOnAllMonitors": false, "umbrellaCursor": true},
  "performance": {"profile": "balanced", "fpsCap": 60, "pauseOnFullscreen": true, "reduceOnBattery": true, "animations": true},
  "hotkeys": {"pauseToggle": "Ctrl+Alt+S", "clearSnow": "Ctrl+Alt+X", "interactionToggle": "Ctrl+Shift+Win+I"},
  "system": {"language": "en", "uiTheme": "night", "trayLeftClickTogglesPause": false,
             "excludedProcesses": ["Telegram", "Keynote"], "excludedSnowMode": "behind"},
  "festive": {"garland": false, "fireworks": true},
  "userPresets": [
    {"name": "Cozy evening", "weather": {"preset": "light", "intensity": 30, "wind": -5, "gustiness": 10, "sizePercent": 120,
     "fallSpeedPercent": 70, "opacityPercent": 90, "skyPerMonitor": true}, "maxHeightPxAt96Dpi": 40, "driftOpacityPercent": 95},
    {"name": "Office", "weather": {"preset": "light", "intensity": 15, "wind": 0, "gustiness": 0, "sizePercent": 80,
     "fallSpeedPercent": 60, "opacityPercent": 60, "skyPerMonitor": true}, "maxHeightPxAt96Dpi": 12, "driftOpacityPercent": 70},
    {"name": "Blizzard night", "weather": {"preset": "blizzard", "intensity": 95, "wind": 70, "gustiness": 80, "sizePercent": 90,
     "fallSpeedPercent": 160, "opacityPercent": 100, "skyPerMonitor": true}, "maxHeightPxAt96Dpi": 60, "driftOpacityPercent": 100}
  ],
  "privacy": {"telemetry": false},
  "futureSection": {"answer": 42, "list": [1, 2, 3]}
})JSON";
#define kInitialSettings @(kInitialSettingsText)

// ------------------------------------------------------------------------------------------------
@interface SFDemoPreviewView : NSView <SFPreviewView>
@property (nonatomic) NSInteger resets;
@property (nonatomic) NSInteger updates;
@end

@implementation SFDemoPreviewView
- (BOOL)isFlipped { return YES; }
- (void)setSettingsJSON:(NSString *)json {
    (void)json;
    self.updates += 1;
}
- (void)resetPreview {
    self.resets += 1;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect b = self.bounds;
    NSGradient *sky = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.11 green:0.24 blue:0.42 alpha:1]
                                                    endingColor:[NSColor colorWithSRGBRed:0.04 green:0.10 blue:0.20 alpha:1]];
    [sky drawInRect:b angle:90];
    // two fake windows with snow caps, a Dock and a few flakes: a stand-in for the Metal preview
    const NSRect w1 = NSMakeRect(b.size.width * 0.08, b.size.height * 0.30, b.size.width * 0.46, b.size.height * 0.42);
    const NSRect w2 = NSMakeRect(b.size.width * 0.44, b.size.height * 0.16, b.size.width * 0.48, b.size.height * 0.40);
    for (NSValue *v in @[ [NSValue valueWithRect:w1], [NSValue valueWithRect:w2] ]) {
        const NSRect r = v.rectValue;
        [[NSColor colorWithSRGBRed:0.17 green:0.28 blue:0.44 alpha:1] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:r xRadius:4 yRadius:4] fill];
        [[NSColor colorWithSRGBRed:0.23 green:0.36 blue:0.55 alpha:1] setFill];
        NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 9));
        [[NSColor colorWithWhite:0.97 alpha:1] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(r.origin.x - 1, r.origin.y - 5, r.size.width + 2, 7) xRadius:3 yRadius:3] fill];
    }
    [[NSColor colorWithWhite:0.97 alpha:1] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, b.size.height - 22, b.size.width, 10) xRadius:4 yRadius:4] fill];
    [[NSColor colorWithSRGBRed:0.02 green:0.05 blue:0.09 alpha:1] setFill];
    NSRectFill(NSMakeRect(0, b.size.height - 14, b.size.width, 14));
    [[NSColor whiteColor] setFill];
    for (int i = 0; i < 40; ++i) {
        const double x = std::fmod(i * 37.7, b.size.width), y = std::fmod(i * 23.3 + 7, b.size.height - 24);
        const double r = 0.8 + (i % 3) * 0.5;
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x - r, y - r, 2 * r, 2 * r)] fill];
    }
}
@end

// ------------------------------------------------------------------------------------------------
@interface SFDemoActions : NSObject <SFAppActions, SFSelfTestProbe>
@property (nonatomic) NSMutableArray<NSString *> *applied;
@property (nonatomic) NSMutableArray<NSString *> *invoked;
@property (nonatomic) NSString *currentJSON;
@property (nonatomic) BOOL paused;
@property (nonatomic, weak) SFDemoPreviewView *preview;
@property (nonatomic, strong) id<SFSettingsWindow> settingsWindow;
@end

@implementation SFDemoActions

- (instancetype)init {
    if ((self = [super init])) {
        _applied = [NSMutableArray array];
        _invoked = [NSMutableArray array];
        _currentJSON = kInitialSettings;
    }
    return self;
}

- (NSDictionary *)current {
    NSData *data = [self.currentJSON dataUsingEncoding:NSUTF8StringEncoding];
    id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [obj isKindOfClass:[NSDictionary class]] ? obj : @{};
}

- (BOOL)russian {
    NSString *lang = [self current][@"system"][@"language"];
    if ([lang isEqual:@"ru"]) return YES;
    if ([lang isEqual:@"en"]) return NO;
    return [[NSLocale preferredLanguages].firstObject hasPrefix:@"ru"];
}

// ---- SFAppActions
- (void)applySettingsJSON:(NSString *)json {
    [self.applied addObject:json];
    self.currentJSON = json;
}
- (void)clearSnow { [self.invoked addObject:@"clearSnow"]; }
- (void)launchFireworks { [self.invoked addObject:@"launchFireworks"]; }
- (void)callSnowplow { [self.invoked addObject:@"callSnowplow"]; }
- (void)togglePause {
    [self.invoked addObject:@"togglePause"];
    self.paused = !self.paused;
}
- (void)openSettings {
    [self.invoked addObject:@"openSettings"];
    [self.settingsWindow showWithSettingsJSON:self.currentJSON];
}
- (void)quitApp { [self.invoked addObject:@"quitApp"]; }

- (NSString *)diagnosticsJSON {
    NSDictionary *strings = [self uiStrings];
    NSDictionary *d = @{
        @"fps": @60, @"frameMs": @0.21, @"simStepMs": @0.08, @"particles": @612, @"surfaces": @9, @"snowballs": @2,
        @"snowmen": @1, @"monitors": @2, @"trackedWindows": @7, @"paused": @(self.paused), @"autoPaused": @NO,
        @"onBattery": @NO, @"statusText": strings[self.paused ? @"StatusPaused" : @"StatusRunning"] ?: @"",
        @"interactionKeys": @"Ctrl+Option", @"logPath": @"/Users/demo/Library/Logs/SnowFall/snowfall.log",
        @"version": @"0.1.1"
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

- (NSString *)validateHotkey:(NSString *)text {
    // Pretend another program owns Ctrl+Shift+Win+I (Ctrl+Shift+Cmd+I on a Mac).
    if ([text caseInsensitiveCompare:@"Ctrl+Shift+Win+I"] == NSOrderedSame) return [self uiStrings][@"HotkeyInUse"];
    return @"";
}

- (NSDictionary<NSString *, NSString *> *)uiStrings { return SFDemoStrings([self russian]); }

- (NSArray<NSDictionary<NSString *, NSString *> *> *)snowmanSkins {
    NSMutableArray *out = [NSMutableArray array];
    const BOOL ru = [self russian];
    for (const snow::SnowmanSkin &skin : snow::kSnowmanSkins) {
        const wchar_t *name = ru ? skin.nameRu : skin.nameEn;
        NSString *n = [[NSString alloc] initWithBytes:name
                                               length:wcslen(name) * sizeof(wchar_t)
                                             encoding:NSUTF32LittleEndianStringEncoding];
        [out addObject:@{@"id": @(skin.id), @"name": n}];
    }
    return out;
}

- (NSArray<NSString *> *)runningAppNames {
    return @[ @"Finder", @"Safari", @"Mail", @"Telegram", @"Xcode", @"Music", @"Notes", @"Keynote", @"Terminal" ];
}

- (NSView *)makePreviewView {
    SFDemoPreviewView *v = [[SFDemoPreviewView alloc] initWithFrame:NSMakeRect(0, 0, 240, 150)];
    self.preview = v;
    return v;
}

// ---- SFSelfTestProbe
- (NSArray<NSString *> *)appliedJSON { return [self.applied copy]; }
- (NSArray<NSString *> *)commands { return [self.invoked copy]; }
- (NSInteger)previewResets { return self.preview.resets; }
- (NSInteger)previewUpdates { return self.preview.updates; }
@end

// ------------------------------------------------------------------------------------------------
@interface SFDemoAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic) SFDemoActions *actions;
@property (nonatomic) NSStatusItem *statusItem;
@property (nonatomic) id<SFQuickPanel> quickPanel;
@property (nonatomic, copy) NSString *selfTestDir;
@end

@implementation SFDemoAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    self.actions = [[SFDemoActions alloc] init];
    Class factory = NSClassFromString(@"SFUIFactory");
    if (!factory || ![factory conformsToProtocol:@protocol(SFUIFactoryProtocol)]) {
        fprintf(stderr, "SFUIFactory not found (is the Swift library linked with -ObjC?)\n");
        exit(100);
    }
    if (self.selfTestDir) {
        Class runner = NSClassFromString(@"SFUISelfTest");
        if (!runner || ![runner conformsToProtocol:@protocol(SFUISelfTestRunner)]) {
            fprintf(stderr, "SFUISelfTest not found\n");
            exit(101);
        }
        const NSInteger failures = [(Class<SFUISelfTestRunner>)runner runWithActions:self.actions
                                                                         settingsJSON:kInitialSettings
                                                                      outputDirectory:self.selfTestDir];
        fprintf(stderr, "self-test finished: %ld failed check(s)\n", (long)failures);
        exit(static_cast<int>(failures > 100 ? 100 : failures));
    }

    Class<SFUIFactoryProtocol> f = (Class<SFUIFactoryProtocol>)factory;
    self.actions.settingsWindow = [f makeSettingsWindowWithActions:self.actions];
    self.quickPanel = [f makeQuickPanelWithActions:self.actions];
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    self.statusItem.button.title = @"❄";
    self.statusItem.button.target = self;
    self.statusItem.button.action = @selector(statusClicked:);
    [self.actions.settingsWindow showWithSettingsJSON:self.actions.currentJSON];
}

- (void)statusClicked:(id)sender {
    (void)sender;
    if (self.quickPanel.isShown) [self.quickPanel close];
    else [self.quickPanel showRelativeToButton:self.statusItem.button settingsJSON:self.actions.currentJSON];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return self.selfTestDir == nil;
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        SFDemoAppDelegate *delegate = [[SFDemoAppDelegate alloc] init];
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--selftest") == 0) {
                delegate.selfTestDir = i + 1 < argc ? @(argv[i + 1]) : @"selftest-out";
            }
        }
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
