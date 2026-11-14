#include "app/app_host.h"

#import <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "app/app_actions.h"
#include "app/strings_mac.h"
#include "app/updater.h"
#include "core/skins.h"
#include "diagnostics/log.h"
#include "settings/hotkey.h"
#include "settings/json.h"
#include "system/cursor_hider.h"
#include "system/hotkey_map.h"
#include "system/objc_util.h"
#include "system/text_util.h"

namespace snow::mac {
namespace {

enum : int { kHotkeyPause = 1, kHotkeyClear = 2, kHotkeyInteraction = 3 };

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s.UTF8String ?: ""; }

std::string hardwareModel() {
    char buf[128] = {};
    size_t len = sizeof buf - 1;
    if (sysctlbyname("hw.model", buf, &len, nullptr, 0) != 0) return "?";
    return buf;
}

}  // namespace

SimPlatformHooks makeMacSimHooks() {
    SimPlatformHooks h;
    h.onThreadStart = [] { pthread_setname_np("SnowFall simulation"); };
    h.cursor = [](float& x, float& y) {
        // CGEvent locations are global display coordinates (top-left origin, points),
        // exactly the core's space, and CGEventCreate is safe on any thread.
        CGEventRef e = CGEventCreate(nullptr);
        if (!e) return false;
        const CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        x = static_cast<float>(p.x);
        y = static_cast<float>(p.y);
        return true;
    };
    return h;
}

AppHost::AppHost() = default;
AppHost::~AppHost() { shutdown(); }

bool AppHost::initialize(const LaunchOptions& options) {
    dataDir_ = toUtf8(SettingsStore::dataDirectory());
    logPath_ = options.logPath;
    quiet_ = options.quiet;
    store_ = SettingsStore(SettingsStore::defaultPath());
    if (!store_.load(settings_)) SNOW_LOGW("settings file was corrupt; defaults loaded");
    sanitize(settings_);
    setUiLanguage(settings_.system.language);
    settings_.startupWithWindows = autostart::isEnabled();   // the login item is the source of truth

    // ---- startup information (no personal data) -------------------------------------
    const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
#if defined(__arm64__)
    const char* arch = "arm64";
#else
    const char* arch = "x86_64";
#endif
    SNOW_LOGI("SnowFall %s (%s) on macOS %ld.%ld.%ld, %s, %lu screen(s)", SNOW_VERSION, arch,
              static_cast<long>(v.majorVersion), static_cast<long>(v.minorVersion), static_cast<long>(v.patchVersion),
              hardwareModel().c_str(), static_cast<unsigned long>(NSScreen.screens.count));
    for (NSScreen* s in NSScreen.screens) {
        SNOW_LOGI("  screen %.0fx%.0f pt, scale %.1f", s.frame.size.width, s.frame.size.height, s.backingScaleFactor);
    }
    SNOW_LOGI("accessibility permission: %s", accessibilityTrusted() ? "granted" : "not granted");

    std::string renderError;
    renderOk_ = overlay_.initialize(&snapshots_, &renderError);
    if (!renderOk_) inputs_.renderFailed = true;

    system_.onWake = [this] {
        tracker_.refreshScreens();
        tracker_.requestResync();
    };
    system_.start([this] { updateGovernor(); });

    interaction_.onActiveChanged = [this](bool active) {
        (void)active;
        updateGovernor();   // also re-evaluates the cursor mode
    };
    interaction_.onCycleTool = [this] {
        Settings s = settings_;
        s.interaction.tool = static_cast<Tool>((static_cast<int>(s.interaction.tool) + 1) % 3);
        applySettings(s, false);
    };
    interaction_.onNeedsAccessibility = [this] { showAccessibilityExplanation(); };
    interaction_.start();

    hotkeys_.onHotkey = [this](int id) { onHotkey(id); };

    const SystemState st = system_.state();
    inputs_.sessionLocked = st.locked;
    inputs_.suspended = st.suspended;
    inputs_.onBattery = st.onBattery;
    inputs_.batterySaver = st.lowPower;
    inputs_.userPaused = !settings_.enabled;
    decision_ = decide(settings_, inputs_);
    runner_.start(settings_, decision_.tuning);

    TrackerOptions topt;
    topt.excludedProcesses = settings_.system.excludedProcesses;
    tracker_.setOptions(topt);
    tracker_.start([this](std::shared_ptr<const DesktopLayout> l) { onLayout(std::move(l)); });

    setupUi();
    applySettings(settings_, false);
    registerHotkeys();
    running_ = true;

    if (!renderOk_) showRenderFailure(renderError);
    if (options.openSettings) openSettings();
    else if (options.firstRun && !options.customDataDir && !quiet_) showFirstRunWelcome();
    startUpdates(options);
    SNOW_LOGI("started, version %s", SNOW_VERSION);
    return true;
}

// ---- updates -----------------------------------------------------------------------------------
// One-time question a few seconds after start (never over another modal dialog), then an
// automatic check kStartupDelaySeconds after start, re-evaluated hourly, only while the user
// agreed (settings.updates.check). The self-test (quiet) never touches the network.

void AppHost::startUpdates(const LaunchOptions& options) {
    const bool network = !quiet_;
    updater_ = [[SFUpdater alloc] initWithNetworkAllowed:network];
    AppHost* self = this;
    updater_.onStatus = ^{
      self->onUpdateStatus();
    };
    updater_.onCheckAttempt = ^{
      self->settings_.updates.lastCheckUnix = static_cast<long long>(std::time(nullptr));
      self->pushSettingsToUi(false);
      if (self->running_) self->scheduleSave();
    };
    updater_.onReadyToQuit = ^{
      self->quit();
    };
    updateBlocker_ = [updater_ installBlocker];
    const long long last = settings_.updates.lastCheckUnix;
    const long long now = static_cast<long long>(std::time(nullptr));
    const std::string lastText = last > 0 ? std::to_string((now - last) / 3600) + " h ago" : std::string("never");
    SNOW_LOGI("updates: automatic check %s, asked %s, last check %s, in-place install %s",
              settings_.updates.check ? "on" : "off", settings_.updates.asked ? "yes" : "no", lastText.c_str(),
              updateBlocker_.empty() ? "possible" : updateBlocker_.c_str());
    if (!network) {
        SNOW_LOGI("updates: network disabled in this run");
        return;
    }
    [updater_ cleanUpPreviousRun];
    if (!settings_.updates.asked && !options.customDataDir) scheduleUpdateQuestion(4.0);
    updateFirstCheckTimer_ = [NSTimer scheduledTimerWithTimeInterval:update::kStartupDelaySeconds
                                                             repeats:NO
                                                               block:^(NSTimer*) {
                                                                 self->updateFirstCheckTimer_ = nil;
                                                                 self->maybeAutoCheck();
                                                               }];
    updateHourlyTimer_ = [NSTimer scheduledTimerWithTimeInterval:3600
                                                         repeats:YES
                                                           block:^(NSTimer*) {
                                                             self->maybeAutoCheck();
                                                           }];
    updateHourlyTimer_.tolerance = 60;
}

void AppHost::scheduleUpdateQuestion(double delaySeconds) {
    [updateAskTimer_ invalidate];
    AppHost* self = this;
    // Default run-loop mode: the timer does not fire while a modal alert (welcome, Accessibility)
    // runs; askAboutUpdates() re-checks anyway.
    updateAskTimer_ = [NSTimer scheduledTimerWithTimeInterval:delaySeconds
                                                      repeats:NO
                                                        block:^(NSTimer*) {
                                                          self->updateAskTimer_ = nil;
                                                          self->askAboutUpdates();
                                                        }];
}

void AppHost::askAboutUpdates() {
    if (!running_ || quiet_ || settings_.updates.asked) return;
    if (NSApp.modalWindow != nil || interaction_.active() || inputs_.fullscreenApp) {
        scheduleUpdateQuestion(15.0);   // later: never over another dialog, a fullscreen app or snow play
        return;
    }
    CursorHider::show();   // never run a dialog with a hidden cursor
    NSAlert* a = [NSAlert new];
    a.messageText = tr(Str::UpdateAskTitle);
    a.informativeText = tr(Str::UpdateAskText);
    [a addButtonWithTitle:tr(Str::BtnUpdateAskYes)];
    [a addButtonWithTitle:tr(Str::BtnUpdateAskNo)];
    a.icon = NSApp.applicationIconImage;
    sfActivateApp();
    const bool yes = [a runModal] == NSAlertFirstButtonReturn;
    SNOW_LOGI("updates: user %s automatic checks", yes ? "allowed" : "declined");
    Settings s = settings_;
    s.updates.asked = true;
    s.updates.check = yes;
    applySettings(s, false);
    saveNow();
    if (yes) checkForUpdates();   // no-op if applySettings already started one
    updateCursorMode();
}

void AppHost::maybeAutoCheck() {
    if (!running_ || !updater_ || !updater_.networkAllowed) return;
    if (update::autoCheckDue(settings_.updates, static_cast<std::int64_t>(std::time(nullptr)))) [updater_ check];
}

void AppHost::checkForUpdates() { [updater_ check]; }
void AppHost::installUpdate() { [updater_ install]; }
void AppHost::openUpdatePage() { [updater_ openReleasePage]; }
bool AppHost::updaterNetworkAllowed() const { return updater_ && updater_.networkAllowed; }

std::string AppHost::updateStatusJson() const {
    if (!updater_) {
        update::Status s;
        const update::Version v = update::parseVersion(SNOW_VERSION);
        s.current = v.valid ? v.text() : SNOW_VERSION;
        return update::statusToJson(s);
    }
    return update::statusToJson([updater_ status]);
}

void AppHost::onUpdateStatus() {
    NSString* json = ns(updateStatusJson());
    [settingsWindow_ updateStatusChanged:json];
    [quickPanel_ updateStatusChanged:json];
    updateStatusItem();
}

void AppHost::setupUi() {
    actions_ = [[SFAppActionsImpl alloc] initWithHost:this];
    Class factory = NSClassFromString(@"SFUIFactory");
    if (factory) {
        Class<SFUIFactoryProtocol> f = (Class<SFUIFactoryProtocol>)factory;
        if ([factory respondsToSelector:@selector(makeSettingsWindowWithActions:)])
            settingsWindow_ = [f makeSettingsWindowWithActions:actions_];
        if ([factory respondsToSelector:@selector(makeQuickPanelWithActions:)])
            quickPanel_ = [f makeQuickPanelWithActions:actions_];
        SNOW_LOGI("ui: SwiftUI module found (settings window %s, quick panel %s)", settingsWindow_ ? "yes" : "no",
                  quickPanel_ ? "yes" : "no");
    } else {
        SNOW_LOGI("ui: SwiftUI module not linked - menu-only fallback");
    }
    status_ = [[SFStatusController alloc] initWithHandler:[this](MenuCommand c, int arg) { onMenu(c, arg); }];
}

void AppHost::shutdown() {
    CursorHider::show();
    [cursorTimer_ invalidate];
    cursorTimer_ = nil;
    [updateAskTimer_ invalidate];
    [updateFirstCheckTimer_ invalidate];
    [updateHourlyTimer_ invalidate];
    updateAskTimer_ = updateFirstCheckTimer_ = updateHourlyTimer_ = nil;
    [updater_ cancel];
    if (!running_) return;
    running_ = false;
    saveNow();
    hotkeys_.unregisterAll();
    interaction_.shutdown();
    tracker_.stop();
    runner_.stop();
    overlay_.shutdown();
    system_.stop();
    [settingsWindow_ close];
    [quickPanel_ close];
    [status_ remove];
    status_ = nil;
    SNOW_LOGI("shutdown complete");
}

void AppHost::quit() { [NSApp terminate:nil]; }

void AppHost::closeUi() {
    [settingsWindow_ close];
    [quickPanel_ close];
}

// ---- layout / governor ---------------------------------------------------------------

void AppHost::onLayout(std::shared_ptr<const DesktopLayout> layout) {
    if (!layout) return;   // the first layout arrives from tracker_.start(), before running_
    layout_ = layout;
    std::vector<ScreenInfo> screens = tracker_.screens();
    if (screens != screens_) {
        screens_ = screens;
        overlay_.setScreens(screens_);
        tracker_.setIgnoredWindows(overlay_.windowNumbers());
    }
    if (layout->fullscreenAppActive != inputs_.fullscreenApp) {
        inputs_.fullscreenApp = layout->fullscreenAppActive;
        SNOW_LOGI("fullscreen app %s", inputs_.fullscreenApp ? "active" : "gone");
        updateGovernor();
    }
    runner_.setLayout(layout);
}

void AppHost::updateGovernor() {
    const SystemState st = system_.state();
    inputs_.sessionLocked = st.locked;
    inputs_.suspended = st.suspended;
    inputs_.onBattery = st.onBattery;
    inputs_.batterySaver = st.lowPower;
    inputs_.userPaused = !settings_.enabled;
    inputs_.interactionMode = interaction_.active();
    const GovernorDecision d = decide(settings_, inputs_);
    const bool first = !decisionValid_;
    decisionValid_ = true;
    if (first || d.simMode != decision_.simMode) runner_.setMode(d.simMode);
    if (first || d.overlaysVisible != decision_.overlaysVisible) {
        overlay_.setVisible(d.overlaysVisible);
        interaction_.setSuspended(!d.overlaysVisible);
    }
    if (first || d.fpsCap != decision_.fpsCap) overlay_.setFpsCap(d.fpsCap);
    if (first || !(d.tuning == decision_.tuning)) runner_.setSettings(settings_, d.tuning);
    if (first || d.reason != decision_.reason) {
        SNOW_LOGI("governor: reason=%d visible=%d fps=%d scale=%.2f", static_cast<int>(d.reason), d.overlaysVisible ? 1 : 0,
                  d.fpsCap, d.tuning.particleScale);
    }
    decision_ = d;
    updateCursorMode();
    updateStatusItem();
}

// ---- tool cursor ---------------------------------------------------------------------------
// Replace (interaction.hideSystemCursor and the private call works): the system cursor is
// hidden and the tool sprite is the cursor, like on Windows. Hold: the arrow stays visible and
// the tool is drawn around it. While the pointer is over something drawn above the snow (menu
// bar, Dock) the arrow is shown even in Replace mode, because the tool would be hidden there.

bool AppHost::pointerOverSystemChrome() const {
    CGEventRef e = CGEventCreate(nullptr);
    if (!e) return false;
    const CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y));
    for (const ScreenInfo& s : screens_)
        if (s.frame.contains(x, y) && y < s.visible.t) return true;   // menu bar
    if (layout_)
        for (const WindowInfo& w : layout_->windows)
            if (w.kind == WindowKind::Taskbar && w.bounds.contains(x, y)) return true;   // Dock
    return false;
}

void AppHost::updateCursorMode() {
    const bool active = running_ && interaction_.active() && decision_.overlaysVisible;
    const bool wantReplace = active && settings_.interaction.hideSystemCursor && !cursorReplaceFailed_;
    bool replaced = false;
    if (wantReplace && !pointerOverSystemChrome()) {
        replaced = CursorHider::hide();
        if (!replaced) {
            cursorReplaceFailed_ = true;   // fall back to Hold for this session
            SNOW_LOGW("cursor: cannot hide the system cursor - drawing the tool around it instead");
        }
    }
    if (!replaced) CursorHider::show();
    overlay_.setCursorDraw(replaced ? CursorDraw::Replace : CursorDraw::Hold);
    if (active) lastInteractionActive_ = CFAbsoluteTimeGetCurrent();

    // While Replace is wanted: follow the pointer over the menu bar / Dock (30 Hz). The same
    // timer is the watchdog: a cursor hidden by us outside interaction mode for > 1 s is shown.
    const bool needTimer = wantReplace || CursorHider::hidden();
    if (needTimer && !cursorTimer_) {
        AppHost* self = this;
        cursorTimer_ = [NSTimer timerWithTimeInterval:1.0 / 30.0
                                              repeats:YES
                                                block:^(NSTimer*) {
                                                  self->cursorTick();
                                                }];
        [NSRunLoop.mainRunLoop addTimer:cursorTimer_ forMode:NSRunLoopCommonModes];
    }
}

void AppHost::cursorTick() {
    const bool active = running_ && interaction_.active() && decision_.overlaysVisible;
    if (active) {
        lastInteractionActive_ = CFAbsoluteTimeGetCurrent();
        const bool wantReplace = settings_.interaction.hideSystemCursor && !cursorReplaceFailed_;
        if (wantReplace && CursorHider::hidden() == pointerOverSystemChrome()) updateCursorMode();
        if (wantReplace) return;
    } else if (CursorHider::hidden() && CFAbsoluteTimeGetCurrent() - lastInteractionActive_ > 1.0) {
        SNOW_LOGW("cursor: watchdog shows the cursor (hidden outside interaction mode)");
        CursorHider::show();
    }
    if (!CursorHider::hidden() && !(active && settings_.interaction.hideSystemCursor && !cursorReplaceFailed_)) {
        [cursorTimer_ invalidate];
        cursorTimer_ = nil;
    }
}

std::string AppHost::cursorModeName() const {
    if (!settings_.interaction.hideSystemCursor) return "hold";
    return cursorReplaceFailed_ ? "replace-unavailable" : "replace";
}

// ---- settings ----------------------------------------------------------------------------

void AppHost::applySettings(Settings next, bool fromUi) {
    // Hotkeys may arrive in Mac spelling ("Ctrl+Option+S"); store the portable form.
    for (std::string* h : {&next.hotkeys.pauseToggle, &next.hotkeys.clearSnow, &next.hotkeys.interactionToggle})
        *h = canonicalHotkeyText(*h);
    sanitize(next);
    const Settings prev = settings_;
    settings_ = next;

    if (next.system.language != prev.system.language || !running_) setUiLanguage(next.system.language);
    if (running_ && next.startupWithWindows != prev.startupWithWindows) {
        std::string err;
        if (!autostart::setEnabled(next.startupWithWindows, &err)) {
            settings_.startupWithWindows = autostart::isEnabled();
        } else if (autostart::status() == autostart::Status::RequiresApproval) {
            autostart::openLoginItemsSettings();
        }
    }
    if (next.system.excludedProcesses != prev.system.excludedProcesses || !running_) {
        TrackerOptions topt;
        topt.excludedProcesses = next.system.excludedProcesses;
        tracker_.setOptions(topt);
        tracker_.requestResync();
    }
    if (running_ && !(next.hotkeys == prev.hotkeys)) registerHotkeys();
    if (next.performance.detailedLog != prev.performance.detailedLog || !running_) {
        const bool detailed = next.performance.detailedLog;
        log::configure(detailed ? log::Level::Debug : log::Level::Info,
                       detailed ? log::kDetailedLogBytes : log::kCompactLogBytes);
        if (running_) SNOW_LOGI("log: %s", detailed ? "detailed" : "compact");
    }
    if (next.interaction.hideSystemCursor && !prev.interaction.hideSystemCursor) cursorReplaceFailed_ = false;

    interaction_.setFeatureEnabled(settings_.interaction.enabled);
    interaction_.setHoldKeyEnabled(settings_.interaction.holdKeyEnabled);
    interaction_.setBlockClicksWhileHeld(settings_.interaction.blockClicksWhileHeld);
    overlay_.setGlobalOpacity(1.0f);   // flake / drift opacity are applied per element by the core

    runner_.setSettings(settings_, decide(settings_, inputs_).tuning);
    updateGovernor();
    pushSettingsToUi(fromUi);
    if (running_) scheduleSave();
    if (running_ && next.updates.check != prev.updates.check) {
        SNOW_LOGI("updates: automatic check turned %s", next.updates.check ? "on" : "off");
        if (next.updates.check) maybeAutoCheck();
    }
}

void AppHost::applySettingsJson(const std::string& text) {
    // Accept Mac hotkey spellings ("Ctrl+Option+P") before the core's sanitize() would drop them.
    std::string json = text;
    json::Value root;
    if (json::parse(text, root)) {
        if (json::Value* h = root.find("hotkeys"); h && h->isObject()) {
            for (const char* k : {"pauseToggle", "clearSnow", "interactionToggle"})
                if (json::Value* v = h->find(k); v && v->isString()) *v = json::Value(canonicalHotkeyText(v->asString()));
        }
        json = json::write(root, 0);
    }
    Settings s;
    std::string err;
    if (!fromJson(json, s, &err)) {
        SNOW_LOGW("ui: settings JSON rejected (%s)", err.c_str());
        pushSettingsToUi(false);
        return;
    }
    // App-owned values: the UI may hold an older copy of them.
    s.updates.lastCheckUnix = settings_.updates.lastCheckUnix;
    if (settings_.updates.asked) s.updates.asked = true;
    applySettings(s, true);
}

std::string AppHost::settingsJson() const { return toJson(settings_); }

void AppHost::pushSettingsToUi(bool fromUi) {
    (void)fromUi;   // the bridge contract: always push the sanitized result; the UI never echoes it
    if (!settingsWindow_ && !quickPanel_) return;
    NSString* json = ns(toJson(settings_));
    if (settingsWindow_ && settingsWindow_.isOpen) [settingsWindow_ updateSettingsJSON:json];
    if (quickPanel_ && quickPanel_.isShown) [quickPanel_ updateSettingsJSON:json];
}

void AppHost::scheduleSave() {
    [saveTimer_ invalidate];
    AppHost* self = this;
    saveTimer_ = [NSTimer scheduledTimerWithTimeInterval:0.7
                                                 repeats:NO
                                                   block:^(NSTimer*) {
                                                     self->saveNow();
                                                   }];
}

void AppHost::saveNow() {
    [saveTimer_ invalidate];
    saveTimer_ = nil;
    if (!store_.save(settings_) && !saveFailedReported_) {
        saveFailedReported_ = true;
        SNOW_LOGE("settings could not be saved");
        if (quiet_) return;
        NSAlert* a = [NSAlert new];
        a.messageText = @"SnowFall";
        a.informativeText = uiRussian() ? @"Не удалось сохранить настройки." : @"Could not save settings.";
        sfActivateApp();
        [a runModal];
    }
}

void AppHost::setPaused(bool paused) {
    Settings s = settings_;
    s.enabled = !paused;
    applySettings(s, false);
}

void AppHost::applyUserPreset(std::size_t index) {
    if (index >= settings_.userPresets.size()) return;
    Settings s = settings_;
    const UserPreset& p = settings_.userPresets[index];
    s.weather = p.weather;
    s.accumulation.maxHeightPxAt96Dpi = p.maxHeightPxAt96Dpi;
    s.accumulation.driftOpacityPercent = p.driftOpacityPercent;
    applySettings(s, false);
}

// ---- hotkeys -------------------------------------------------------------------------------

void AppHost::registerHotkeys() {
    hotkeys_.unregisterAll();
    const std::string* texts[] = {nullptr, &settings_.hotkeys.pauseToggle, &settings_.hotkeys.clearSnow,
                                  &settings_.hotkeys.interactionToggle};
    for (int id = kHotkeyPause; id <= kHotkeyInteraction; ++id) {
        hotkeyOk_[id] = false;
        Hotkey hk;
        if (texts[id]->empty() || !parseHotkey(*texts[id], hk)) continue;
        hotkeyOk_[id] = hotkeys_.registerHotkey(id, hk);
        if (!hotkeyOk_[id]) SNOW_LOGW("hotkey %s could not be registered", macHotkeyDisplay(*texts[id]).c_str());
    }
}

std::string AppHost::validateHotkey(const std::string& text) const {
    if (text.empty()) return {};
    const std::string canonical = canonicalHotkeyText(text);
    Hotkey hk;
    unsigned key = 0;
    if (canonical.empty() || !parseHotkey(canonical, hk) || !vkToMacKeyCode(hk.vk, key)) return str(tr(Str::HotkeyInvalid));
    const std::string* ours[] = {&settings_.hotkeys.pauseToggle, &settings_.hotkeys.clearSnow,
                                 &settings_.hotkeys.interactionToggle};
    for (int i = 0; i < 3; ++i) {
        Hotkey o;
        if (parseHotkey(*ours[i], o) && o == hk && hotkeyOk_[i + 1]) return {};
    }
    if (HotkeyManager::reservedBySystem(hk) || !HotkeyManager::probe(hk)) return str(tr(Str::HotkeyInUse));
    return {};
}

void AppHost::onHotkey(int id) {
    switch (id) {
        case kHotkeyPause: togglePause(); break;
        case kHotkeyClear: clearSnow(); break;
        case kHotkeyInteraction:
            interaction_.setToggled(!interaction_.toggled());
            updateStatusItem();
            break;
        default: break;
    }
}

// ---- menu ------------------------------------------------------------------------------------

void AppHost::onMenu(MenuCommand cmd, int arg) {
    Settings s = settings_;
    auto preset = [&](WeatherPreset p) {
        s.weather.preset = p;
        applySettings(s, false);
    };
    auto tool = [&](Tool t) {
        s.interaction.tool = t;
        applySettings(s, false);
    };
    switch (cmd) {
        case MenuCommand::LeftClick:
            if (settings_.system.trayLeftClickTogglesPause) {
                togglePause();
            } else if (quickPanel_) {
                if (quickPanel_.isShown) {
                    [quickPanel_ close];
                } else {
                    sfActivateApp();
                    [quickPanel_ showRelativeToButton:status_.button settingsJSON:ns(toJson(settings_))];
                    [quickPanel_ updateStatusChanged:ns(updateStatusJson())];
                }
            } else {
                [status_ popUpMenu];
            }
            break;
        case MenuCommand::TogglePause: togglePause(); break;
        case MenuCommand::PresetLight: preset(WeatherPreset::Light); break;
        case MenuCommand::PresetNormal: preset(WeatherPreset::Normal); break;
        case MenuCommand::PresetBlizzard: preset(WeatherPreset::Blizzard); break;
        case MenuCommand::PresetThaw: preset(WeatherPreset::Thaw); break;
        case MenuCommand::ApplyUserPreset: applyUserPreset(static_cast<std::size_t>(std::max(arg, 0))); break;
        case MenuCommand::ToggleInteraction:
            interaction_.setToggled(!interaction_.toggled());
            updateStatusItem();
            break;
        case MenuCommand::ToolMitten: tool(Tool::Mitten); break;
        case MenuCommand::ToolShovel: tool(Tool::Shovel); break;
        case MenuCommand::ToolSnowball: tool(Tool::Snowball); break;
        case MenuCommand::ClearSnow: clearSnow(); break;
        case MenuCommand::CallSnowplow: callSnowplow(); break;
        case MenuCommand::Fireworks: fireworks(); break;
        case MenuCommand::ToggleHideAll:
            inputs_.hideAll = !inputs_.hideAll;
            updateGovernor();
            break;
        case MenuCommand::OpenSettings: openSettings(); break;
        case MenuCommand::ToggleAutostart:
            s.startupWithWindows = !s.startupWithWindows;
            applySettings(s, false);
            break;
        case MenuCommand::OpenSettingsFile: {
            saveNow();
            NSURL* file = [NSURL fileURLWithPath:ns(toUtf8(store_.path()))];
            NSURL* textEdit = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.TextEdit"];
            if (textEdit) {
                [NSWorkspace.sharedWorkspace openURLs:@[ file ]
                                 withApplicationAtURL:textEdit
                                        configuration:[NSWorkspaceOpenConfiguration configuration]
                                    completionHandler:nil];
            } else {
                [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
            }
            break;
        }
        case MenuCommand::ReloadSettingsFile: {
            Settings loaded;
            if (store_.load(loaded)) {
                loaded.startupWithWindows = settings_.startupWithWindows;
                applySettings(loaded, false);
            }
            break;
        }
        case MenuCommand::OpenUpdateInfo: openSettings("about"); break;
        case MenuCommand::OpenLogFolder:
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(logPath_)] ]];
            break;
        case MenuCommand::Quit: quit(); break;
    }
}

void AppHost::openSettings(const std::string& page) {
    if (!settingsWindow_) {
        if (page == "about") openUpdatePage();
        else onMenu(MenuCommand::OpenSettingsFile, 0);
        return;
    }
    sfActivateApp();
    [settingsWindow_ showWithSettingsJSON:ns(toJson(settings_))];
    [settingsWindow_ updateStatusChanged:ns(updateStatusJson())];
    if (!page.empty()) [settingsWindow_ showPage:ns(page)];
    tracker_.requestResync();   // the settings window collects snow like any app window
}

void AppHost::remoteCommand(const std::string& cmd) {
    SNOW_LOGI("remote command: %s", cmd.c_str());
    if (cmd == "settings") openSettings();
    else if (cmd == "exit") quit();
    else if (cmd == "clear") clearSnow();
    else if (cmd == "pause") togglePause();
    else if (cmd == "plow") callSnowplow();
    else if (cmd == "interaction") onMenu(MenuCommand::ToggleInteraction, 0);
    else if (cmd == "fireworks") fireworks();
}

// ---- status ----------------------------------------------------------------------------------

std::string AppHost::holdKeysText() const {
    if (!settings_.interaction.enabled || !settings_.interaction.holdKeyEnabled) return {};
    return InteractionController::chordName();
}

std::string AppHost::statusText() const {
    const bool ru = uiRussian();
    switch (decision_.reason) {
        case PauseReason::None:
            if (interaction_.active())
                return interaction_.tapInstalled() ? (ru ? "Работает — режим взаимодействия" : "Running — interaction mode")
                                                   : (ru ? "Режим взаимодействия: только просмотр (нет разрешения)"
                                                         : "Interaction mode: view only (no permission)");
            if (inputs_.onBattery && settings_.performance.reduceOnBattery) return str(tr(Str::StatusReducedOnBattery));
            return str(tr(Str::StatusRunning));
        case PauseReason::User: return str(tr(Str::StatusPaused));
        case PauseReason::Hidden: return str(tr(Str::StatusHidden));
        case PauseReason::Fullscreen: return str(tr(Str::StatusAutoPausedFullscreen));
        case PauseReason::Locked: return str(tr(Str::StatusAutoPausedLocked));
        case PauseReason::Suspended: return ru ? "Пауза (спящий режим)" : "Paused (sleep)";
        case PauseReason::RenderFailed: return str(tr(Str::StatusRenderingDisabled));
    }
    return {};
}

void AppHost::updateStatusItem() {
    if (!status_) return;
    StatusMenuState st;
    st.paused = !settings_.enabled || decision_.autoPaused;
    st.autoPaused = decision_.autoPaused;
    st.hidden = inputs_.hideAll;
    st.interaction = interaction_.active();
    st.autostart = settings_.startupWithWindows;
    st.preset = settings_.weather.preset;
    st.tool = settings_.interaction.tool;
    for (const UserPreset& p : settings_.userPresets) st.userPresets.push_back(p.name);
    st.statusText = statusText();
    st.holdKeys = holdKeysText();
    st.hasSettingsWindow = settingsWindow_ != nil;
    if (updater_ && [updater_ status].stage == update::Stage::Available) st.updateVersion = [updater_ status].latest;
    [status_ setState:st];
}

std::string AppHost::diagnosticsJson() const {
    json::Value o = json::Value::object();
    const RenderStats rs = overlay_.stats();
    o.set("fps", static_cast<double>(std::round(rs.fps * 10.0f) / 10.0f));
    o.set("frameMs", static_cast<double>(std::round(rs.frameMs * 100.0f) / 100.0f));
    SimStats ss;
    if (auto snap = snapshots_.latest()) ss = snap->stats;
    o.set("simStepMs", static_cast<double>(std::round(ss.stepMs * 100.0f) / 100.0f));
    o.set("particles", static_cast<int>(ss.particles));
    o.set("surfaces", static_cast<int>(ss.surfaces));
    o.set("snowballs", static_cast<int>(ss.snowballs));
    o.set("snowmen", static_cast<int>(ss.snowmen));
    o.set("monitors", static_cast<int>(screens_.size()));
    o.set("trackedWindows", layout_ ? static_cast<int>(layout_->windows.size()) : 0);
    o.set("paused", !settings_.enabled);
    o.set("autoPaused", decision_.autoPaused);
    o.set("onBattery", inputs_.onBattery);
    o.set("statusText", statusText());
    o.set("interactionKeys", holdKeysText());
    o.set("logPath", logPath_);
    o.set("version", SNOW_VERSION);
    // macOS extras
    o.set("accessibility", accessibilityTrusted());
    o.set("interactionActive", interaction_.active());
    o.set("eventTap", interaction_.tapInstalled());
    o.set("cursorMode", cursorModeName());
    o.set("cursorHidden", CursorHider::hidden());
    o.set("gpu", overlay_.deviceName());
    o.set("gpuErrors", rs.gpuErrors);
    const TrackerStats ts = tracker_.stats();
    o.set("trackerPollsPerSecond", std::round(ts.pollsPerSecond * 10.0) / 10.0);
    o.set("trackerPollMs", std::round(ts.avgPollMs * 100.0) / 100.0);
    o.set("dockWidth", ts.dockWidth);
    o.set("dockFromAccessibility", ts.dockFromAccessibility);
    // updates (no URLs, no paths)
    o.set("updateCheck", settings_.updates.check);
    o.set("updateLastCheck", static_cast<double>(settings_.updates.lastCheckUnix));
    o.set("updateStage", update::stageName(updater_ ? [updater_ status].stage : update::Stage::Idle));
    o.set("updateLatest", updater_ ? [updater_ status].latest : std::string());
    o.set("updateNetwork", updaterNetworkAllowed());
    o.set("updateInstallBlocker", updateBlocker_);
    return json::write(o, 0);
}

// ---- dialogs -----------------------------------------------------------------------------------

void AppHost::showAccessibilityExplanation() {
    if (quiet_) return;
    CursorHider::show();   // never run a dialog with a hidden cursor
    NSAlert* a = [NSAlert new];
    a.messageText = tr(Str::AccessibilityNeededTitle);
    a.informativeText = tr(Str::AccessibilityNeededText);
    [a addButtonWithTitle:tr(Str::BtnOpenSystemSettings)];
    [a addButtonWithTitle:tr(Str::BtnCancel)];
    sfActivateApp();
    if ([a runModal] == NSAlertFirstButtonReturn) {
        requestAccessibility();
        openAccessibilitySettings();
    }
    updateCursorMode();
}

void AppHost::showFirstRunWelcome() {
    const bool ru = uiRussian();
    NSAlert* a = [NSAlert new];
    a.messageText = ru ? @"Снег идёт!" : @"It's snowing!";
    a.informativeText = ru ? @"SnowFall живёт в строке меню (снежинка вверху справа): щелчок — быстрая панель, правый "
                             @"щелчок — меню. Удерживайте Ctrl+Option, чтобы сметать снег и лепить снежки."
                           : @"SnowFall lives in the menu bar (the snowflake at the top right): click it for the quick "
                             @"panel, right-click for the menu. Hold Ctrl+Option to brush snow and roll snowballs.";
    a.icon = NSApp.applicationIconImage;
    sfActivateApp();
    [a runModal];
}

void AppHost::showRenderFailure(const std::string& why) {
    SNOW_LOGE("rendering disabled: %s", why.c_str());
    if (quiet_) return;
    NSAlert* a = [NSAlert new];
    a.messageText = tr(Str::BalloonDeviceLostTitle);
    a.informativeText = tr(Str::BalloonDeviceLostText);
    sfActivateApp();
    [a runModal];
}

}  // namespace snow::mac
