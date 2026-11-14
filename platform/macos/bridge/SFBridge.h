// SFBridge.h — the ONLY contract between the macOS app shell (Objective-C++, owns the C++
// core) and the SwiftUI user interface (settings window + menu-bar quick panel).
//
// Pure Objective-C (no C++ types) so Swift can import it. Settings travel as JSON in exactly
// the format of core/settings/settings_json.cpp (toJson/fromJson): the Swift side decodes it
// with Codable, edits it, and sends the whole JSON back. The app shell sanitizes, applies and
// persists it, then pushes the result back with -update... (never echo-loop on that).
//
// Ownership: platform/macos/app implements SFAppActions and SFPreviewView; platform/macos/ui
// implements SFSettingsWindow, SFQuickPanel and the SFUIFactory class (Swift, @objc).
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Commands and data the UI asks of the app. Implemented by the app shell; all methods are
/// called on the main thread.
@protocol SFAppActions <NSObject>
/// Full settings JSON after a user edit (any control). The app applies and persists it.
- (void)applySettingsJSON:(NSString *)json;
- (void)clearSnow;
- (void)launchFireworks;
- (void)callSnowplow;
- (void)togglePause;
- (void)openSettings;
- (void)quitApp;
/// Live values for the diagnostics block and the status line, JSON object:
/// { "fps":60, "frameMs":0.2, "simStepMs":0.08, "particles":612, "surfaces":9, "snowballs":0,
///   "snowmen":1, "monitors":2, "trackedWindows":7, "paused":false, "autoPaused":false,
///   "onBattery":false, "statusText":"Running", "interactionKeys":"Ctrl+Option",
///   "logPath":"/Users/…/Library/Logs/SnowFall/snowfall.log", "version":"0.1.1" }
- (NSString *)diagnosticsJSON;
/// "" if the hotkey text ("Ctrl+Option+S" style, see core/settings/hotkey.h) can be registered,
/// otherwise a human-readable reason in the current UI language.
- (NSString *)validateHotkey:(NSString *)text;
/// UI strings for the current language, keyed by the StrId names of core/i18n/strings_table.inc
/// (e.g. @"TabSnow"). The UI must use these instead of hard-coding texts.
- (NSDictionary<NSString *, NSString *> *)uiStrings;
/// Snowman skins from core/core/skins.h: [ { "id":"pirate", "name":"Пират" }, … ] in UI language.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)snowmanSkins;
/// Executable/bundle names of apps that currently own visible windows (for the exclusions
/// picker). Never window titles.
- (NSArray<NSString *> *)runningAppNames;
/// A live preview view (miniature desktop with the real simulation) for the settings window.
- (NSView *)makePreviewView;

// ---- updates (core/update/update.h) -------------------------------------------------------
/// Current update state, JSON of update::statusToJson:
/// { "stage":"idle|checking|upToDate|available|downloading|installing|failed", "current":"0.1.2",
///   "latest":"0.1.3", "pageUrl":"https://github.com/…", "notes":"…", "canInstall":true,
///   "progress":0..100, "error":"UpdateErrNetwork" }   ("error" is a StrId name of the string table)
/// Changes are pushed with -updateStatusChanged: of the settings window and the quick panel.
- (NSString *)updateStatusJSON;
/// «Проверить сейчас»: one request to the GitHub API (ignored while a check / install runs).
- (void)checkForUpdates;
/// «Обновить»: download, verify and install the available version (only when canInstall);
/// the app quits and the new version starts.
- (void)installUpdate;
/// «Страница загрузки» / «Что нового»: the release page in the browser.
- (void)openUpdatePage;
/// Settings window on the «О программе» page (header badge / menu item of an available update).
- (void)openUpdateInfo;
@end

/// The live preview returned by -makePreviewView (implemented by the app shell with Metal).
@protocol SFPreviewView <NSObject>
- (void)setSettingsJSON:(NSString *)json;   // coalesce internally to <= 20 Hz
- (void)resetPreview;                         // «Сбросить превью»
@end

/// Settings window (SwiftUI inside an NSWindow), implemented in Swift.
@protocol SFSettingsWindow <NSObject>
- (void)showWithSettingsJSON:(NSString *)json;   // create on first use, bring to front
- (void)updateSettingsJSON:(NSString *)json;     // external change: refresh controls, no echo
- (void)updateStatusChanged:(NSString *)json;    // -[SFAppActions updateStatusJSON] changed
- (void)showPage:(NSString *)page;               // "about" (others: snow, drifts, tools, performance, festive, system)
- (void)close;
@property (nonatomic, readonly) BOOL isOpen;
@end

/// Quick panel in a popover under the status-bar item, implemented in Swift.
@protocol SFQuickPanel <NSObject>
- (void)showRelativeToButton:(NSStatusBarButton *)button settingsJSON:(NSString *)json;
- (void)updateSettingsJSON:(NSString *)json;
- (void)updateStatusChanged:(NSString *)json;
- (void)close;
@property (nonatomic, readonly) BOOL isShown;
@end

/// Entry point of the Swift UI module. The Swift side defines
///     @objc(SFUIFactory) public final class SFUIFactory: NSObject { … }
/// with these two class methods; the app shell looks it up with NSClassFromString(@"SFUIFactory")
/// so it does not need the generated Swift header (and can run with a fallback NSMenu-only UI
/// when the class is missing).
@protocol SFUIFactoryProtocol <NSObject>
+ (id<SFSettingsWindow>)makeSettingsWindowWithActions:(id<SFAppActions>)actions;
+ (id<SFQuickPanel>)makeQuickPanelWithActions:(id<SFAppActions>)actions;
@end

NS_ASSUME_NONNULL_END
