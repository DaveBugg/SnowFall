#include "app/app_host.h"

#import <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cmath>

#include "app/app_actions.h"
#include "app/strings_mac.h"
#include "core/skins.h"
#include "diagnostics/log.h"
#include "settings/hotkey.h"
#include "settings/json.h"
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
        overlay_.setCursorBadge(active);
        updateGovernor();
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
    else if (options.firstRun && !options.customDataDir) showFirstRunWelcome();
    SNOW_LOGI("started, version %s", SNOW_VERSION);
    return true;
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
    updateStatusItem();
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

    interaction_.setFeatureEnabled(settings_.interaction.enabled);
    interaction_.setHoldKeyEnabled(settings_.interaction.holdKeyEnabled);
    interaction_.setBlockClicksWhileHeld(settings_.interaction.blockClicksWhileHeld);
    overlay_.setGlobalOpacity(1.0f);   // flake / drift opacity are applied per element by the core

    runner_.setSettings(settings_, decide(settings_, inputs_).tuning);
    updateGovernor();
    pushSettingsToUi(fromUi);
    if (running_) scheduleSave();
}

void AppHost::applySettingsJson(const std::string& json) {
    Settings s;
    std::string err;
    if (!fromJson(json, s, &err)) {
        SNOW_LOGW("ui: settings JSON rejected (%s)", err.c_str());
        pushSettingsToUi(false);
        return;
    }
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
        case MenuCommand::OpenLogFolder:
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(logPath_)] ]];
            break;
        case MenuCommand::Quit: quit(); break;
    }
}

void AppHost::openSettings() {
    if (!settingsWindow_) {
        onMenu(MenuCommand::OpenSettingsFile, 0);
        return;
    }
    sfActivateApp();
    [settingsWindow_ showWithSettingsJSON:ns(toJson(settings_))];
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
    o.set("gpu", overlay_.deviceName());
    o.set("gpuErrors", rs.gpuErrors);
    const TrackerStats ts = tracker_.stats();
    o.set("trackerPollsPerSecond", std::round(ts.pollsPerSecond * 10.0) / 10.0);
    o.set("trackerPollMs", std::round(ts.avgPollMs * 100.0) / 100.0);
    o.set("dockWidth", ts.dockWidth);
    o.set("dockFromAccessibility", ts.dockFromAccessibility);
    return json::write(o, 0);
}

// ---- dialogs -----------------------------------------------------------------------------------

void AppHost::showAccessibilityExplanation() {
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
    NSAlert* a = [NSAlert new];
    a.messageText = tr(Str::BalloonDeviceLostTitle);
    a.informativeText = tr(Str::BalloonDeviceLostText);
    sfActivateApp();
    [a runModal];
}

}  // namespace snow::mac
