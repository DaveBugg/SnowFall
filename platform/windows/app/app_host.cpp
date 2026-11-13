#include "app/app_host.h"

#include <dbt.h>
#include <powrprof.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <algorithm>

#include "app/cursor_hider.h"
#include "app/legacy_migration.h"
#include "app/ui/strings.h"
#include "diagnostics/log.h"
#include "settings/autostart.h"
#include "settings/hotkey.h"

namespace snow {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool systemIsRussian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN; }

}  // namespace

AppHost::AppHost() = default;

AppHost::~AppHost() { shutdown(); }

bool AppHost::initialize(HINSTANCE instance, const LaunchOptions& options) {
    instance_ = instance;
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    exePath_ = path;
    logPath_ = SettingsStore::dataDirectory() + L"\\snowfall.log";
    CursorHider::setMarkerPath(SettingsStore::dataDirectory() + L"\\cursor-hidden.marker");
    CursorHider::restoreIfMarked();

    // Renamed from "Windows Snow": stop the old app, carry over its settings and autostart.
    bool oldAutostart = false;
    if (!options.customDataDir) {
        legacy::stopOldInstance();
        legacy::migrateSettings(SettingsStore::dataDirectory());
        oldAutostart = legacy::takeOldAutostart();
    }

    if (!store_.load(settings_)) SNOW_LOGW("settings file was corrupt; defaults loaded");
    sanitize(settings_);
    applyLanguage();

    // Autostart value is the source of truth for the checkbox (user may delete it).
    if (oldAutostart && !autostart::isEnabled(exePath_)) autostart::setEnabled(true, exePath_);
    settings_.startupWithWindows = autostart::isEnabled(exePath_);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &AppHost::wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClass;
    wc.hIcon = TrayIcon::createAppIcon(32);
    if (!RegisterClassExW(&wc)) return false;
    // Hidden top-level tool window (not message-only): receives broadcasts such as
    // TaskbarCreated, WM_POWERBROADCAST and WM_DISPLAYCHANGE.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"SnowFall", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) return false;

    sessionNotify_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
    RegisterPowerSettingNotification(hwnd_, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    RegisterPowerSettingNotification(hwnd_, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
    refreshPowerState();

    tray_.onQuickChange = [this](const Settings& s) { applySettings(s, false); };
    tray_.setSettings(settings_);
    tray_.create(instance, hwnd_, kMsgTray);

    SettingsWindowCallbacks cb;
    cb.onChanged = [this](const Settings& s) { applySettings(s, true); };
    cb.onClearSnow = [this] { clearSnow(); };
    cb.getDiagnostics = [this] { return diagnostics(); };
    cb.validateHotkey = [this](const std::string& t) { return validateHotkey(t); };
    cb.onFireworks = [this] { runner_.triggerFireworks(); };
    settingsWindow_.initialize(instance, std::move(cb));

    if (!overlay_.initialize(instance, &snapshots_, hwnd_, kMsgRenderFailed)) {
        SNOW_LOGE("overlay initialization failed");
        inputs_.renderFailed = true;
        tray_.showBalloon(L"SnowFall",
                          tx(L"Не удалось инициализировать графику. Эффекты отключены.",
                             L"Graphics initialization failed. Effects are disabled."),
                          true);
    }

    interaction_.initialize(hwnd_, kMsgCycleTool);
    interaction_.onActiveChanged = [this](bool) { updateTray(); };

    inputs_.userPaused = !settings_.enabled;
    decision_ = decide(settings_, inputs_);
    runner_.start(settings_, decision_.tuning);

    TrackerOptions topt;
    for (const auto& p : settings_.system.excludedProcesses) topt.excludedProcesses.push_back(widen(p));
    tracker_.setOptions(topt);
    tracker_.setIgnoredWindows(overlay_.overlayWindows());
    tracker_.start(instance, [this](std::shared_ptr<const DesktopLayout> l) { onLayout(std::move(l)); });

    applySettings(settings_, false);
    registerHotkeys(!options.autostart);
    running_ = true;

    if (options.openSettings) openSettings();
    if (!options.autostart && !options.openSettings) {
        tray_.showBalloon(L"SnowFall",
                          tx(L"Снег идёт! Значок в трее — меню и настройки. Удерживайте Alt+Shift, чтобы "
                             L"сметать снег и лепить снежки.",
                             L"It's snowing! Use the tray icon for the menu. Hold Alt+Shift to brush snow and "
                             L"roll snowballs."),
                          false);
    }
    SNOW_LOGI("started, version %s", SNOW_VERSION);
    return true;
}

int AppHost::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (settingsWindow_.preTranslateMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    shutdown();
    return 0;
}

void AppHost::shutdown() {
    if (!hwnd_ && !running_) return;
    running_ = false;
    saveNow();
    unregisterHotkeys();
    interaction_.shutdown();
    tracker_.stop();
    runner_.stop();
    overlay_.shutdown();
    settingsWindow_.close();
    tray_.remove();
    if (hwnd_) {
        if (sessionNotify_) WTSUnRegisterSessionNotification(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    SNOW_LOGI("shutdown complete");
}

LRESULT CALLBACK AppHost::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppHost* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<AppHost*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<AppHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AppHost::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = 0;
    if (tray_.handleMessage(hwnd, msg, wp, lp, &r)) return r;

    switch (msg) {
        case kMsgTray:
            if (static_cast<TrayCommand>(wp) == TrayCommand::ApplyUserPreset) {
                applyUserPreset(static_cast<std::size_t>(lp));
            } else {
                onTrayCommand(static_cast<TrayCommand>(wp));
            }
            return 0;
        case kMsgRenderFailed:
            inputs_.renderFailed = true;
            updateGovernor();
            tray_.showBalloon(L"SnowFall",
                              tx(L"Графическое устройство потеряно и не восстановилось. Эффекты отключены — "
                                 L"перезапустите приложение.",
                                 L"The graphics device was lost and could not be recovered. Effects are disabled — "
                                 L"restart the app."),
                              true);
            return 0;
        case kMsgCycleTool: {
            Settings s = settings_;
            s.interaction.tool = static_cast<Tool>((static_cast<int>(s.interaction.tool) + 1) % 3);
            applySettings(s, false);
            return 0;
        }
        case kMsgRemote:
            if (wp == 1) openSettings();
            if (wp == 2) PostQuitMessage(0);
            if (wp == 3) clearSnow();
            if (wp == 4) onTrayCommand(TrayCommand::TogglePause);
            if (wp == 5) runner_.triggerSnowplow();
            if (wp == 6) onTrayCommand(TrayCommand::ToggleInteraction);
            if (wp == 7) runner_.triggerFireworks();
            return 0;
        case WM_HOTKEY:
            onHotkey(static_cast<int>(wp));
            return 0;
        case WM_TIMER:
            if (wp == InteractionController::kTimerId) {
                interaction_.onTimer();
                syncTrackerWindows();
            } else if (wp == kTimerSave) {
                KillTimer(hwnd, kTimerSave);
                saveNow();
            }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT)
                inputs_.sessionLocked = true;
            else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
                inputs_.sessionLocked = false;
            updateGovernor();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {
                inputs_.suspended = true;
            } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                inputs_.suspended = false;
                tracker_.requestResync();
            }
            refreshPowerState();
            updateGovernor();
            return TRUE;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            tracker_.requestResync();
            return 0;
        case WM_QUERYENDSESSION:
            CursorHider::restore();
            return TRUE;
        case WM_ENDSESSION:
            CursorHider::restore();
            if (wp) shutdown();
            return 0;
        case WM_CLOSE:
            return 0;   // the host never closes by itself; Exit goes through the tray
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void AppHost::onLayout(std::shared_ptr<const DesktopLayout> layout) {
    if (!layout) return;
    layout_ = layout;
    if (layout->monitors != monitors_) {
        monitors_ = layout->monitors;
        overlay_.setMonitors(monitors_);
        tracker_.setIgnoredWindows(overlay_.overlayWindows());
        SNOW_LOGI("monitors: %zu", monitors_.size());
    }
    if (layout->fullscreenAppActive != inputs_.fullscreenApp) {
        inputs_.fullscreenApp = layout->fullscreenAppActive;
        updateGovernor();
    }
    runner_.setLayout(layout);
    // Re-asserting HWND_TOPMOST would put the overlay above our own tray menu / message boxes
    // (they are topmost too); while one is open, leave the Z-order alone.
    if (!ownPopupOpen()) overlay_.raise();
}

bool AppHost::ownPopupOpen() const {
    // Any visible window of ours that must stay above the snow: tray menu (#32768), message
    // boxes (#32770) and every topmost popup (quick panel, process picker) — but not the
    // overlays themselves.
    struct Ctx {
        const std::vector<HWND>* overlays;
        bool open;
    } ctx{nullptr, false};
    const std::vector<HWND> overlays = overlay_.overlayWindows();
    ctx.overlays = &overlays;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (!IsWindowVisible(h)) return TRUE;
            for (HWND o : *c->overlays)
                if (o == h) return TRUE;
            wchar_t cls[16] = {};
            GetClassNameW(h, cls, 16);
            const bool popup = wcscmp(cls, L"#32768") == 0 || wcscmp(cls, L"#32770") == 0 ||
                               (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            if (popup) {
                c->open = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.open;
}

void AppHost::syncTrackerWindows() {
    // The settings window should collect snow like any application window.
    HWND h = settingsWindow_.hwnd();
    if (h == trackedSettingsHwnd_) return;
    trackedSettingsHwnd_ = h;
    std::vector<HWND> own;
    if (h) own.push_back(h);
    tracker_.setOwnAppWindows(own);
    tracker_.requestResync();
}

void AppHost::refreshPowerState() {
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        inputs_.onBattery = ps.ACLineStatus == 0;
        inputs_.batterySaver = (ps.SystemStatusFlag & 1) != 0;
    }
}

void AppHost::updateGovernor() {
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
        SNOW_LOGI("governor: reason=%d visible=%d fps=%d scale=%.2f", static_cast<int>(d.reason),
                  d.overlaysVisible ? 1 : 0, d.fpsCap, d.tuning.particleScale);
    }
    decision_ = d;
    updateTray();
}

void AppHost::applySettings(Settings next, bool fromSettingsWindow) {
    sanitize(next);
    const Settings prev = settings_;
    settings_ = next;

    if (next.system.language != prev.system.language || !running_) applyLanguage();

    if (next.startupWithWindows != prev.startupWithWindows && running_) {
        if (!autostart::setEnabled(next.startupWithWindows, exePath_)) {
            settings_.startupWithWindows = autostart::isEnabled(exePath_);
            SNOW_LOGW("autostart change failed");
        }
    }
    if (next.system.excludedProcesses != prev.system.excludedProcesses || !running_) {
        TrackerOptions topt;
        for (const auto& p : next.system.excludedProcesses) topt.excludedProcesses.push_back(widen(p));
        tracker_.setOptions(topt);
        tracker_.requestResync();
    }
    if (!(next.hotkeys == prev.hotkeys) && running_) registerHotkeys(true);
    if (next.performance.detailedLog != prev.performance.detailedLog || !running_) {
        const bool detailed = next.performance.detailedLog;
        log::configure(detailed ? log::Level::Debug : log::Level::Info,
                       detailed ? log::kDetailedLogBytes : log::kCompactLogBytes);
        if (running_) SNOW_LOGI("log: %s", detailed ? "detailed" : "compact");
    }

    interaction_.setFeatureEnabled(next.interaction.enabled);
    interaction_.setHoldKeyEnabled(next.interaction.holdKeyEnabled);
    interaction_.setBlockClicksWhileHeld(next.interaction.blockClicksWhileHeld);
    // Flake and drift opacity are applied per element by the simulation (separate sliders).
    overlay_.setGlobalOpacity(1.0f);

    runner_.setSettings(settings_, decide(settings_, inputs_).tuning);
    updateGovernor();
    if (!fromSettingsWindow) settingsWindow_.update(settings_);
    tray_.setSettings(settings_);
    if (running_) scheduleSave();
}

void AppHost::scheduleSave() { SetTimer(hwnd_, kTimerSave, 700, nullptr); }

void AppHost::saveNow() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerSave);
    if (!store_.save(settings_) && !saveFailedReported_) {
        saveFailedReported_ = true;
        tray_.showBalloon(L"SnowFall", tx(L"Не удалось сохранить настройки.", L"Could not save settings."), true);
    }
}

void AppHost::applyLanguage() {
    setLanguage(settings_.system.language);   // UI module strings (tray menu, settings window)
    trayStateValid_ = false;                  // tooltip text depends on the language
    switch (settings_.system.language) {
        case Language::Russian: russian_ = true; break;
        case Language::English: russian_ = false; break;
        default: russian_ = systemIsRussian(); break;
    }
}

// ---- hotkeys -------------------------------------------------------------

void AppHost::unregisterHotkeys() {
    for (int id = kHotkeyPause; id <= kHotkeyInteraction; ++id) {
        if (hotkeyRegistered_[id]) UnregisterHotKey(hwnd_, id);
        hotkeyRegistered_[id] = false;
    }
}

void AppHost::registerHotkeys(bool notifyConflicts) {
    unregisterHotkeys();
    const std::string* texts[] = {nullptr, &settings_.hotkeys.pauseToggle, &settings_.hotkeys.clearSnow,
                                  &settings_.hotkeys.interactionToggle};
    std::wstring failed;
    for (int id = kHotkeyPause; id <= kHotkeyInteraction; ++id) {
        Hotkey hk;
        if (texts[id]->empty() || !parseHotkey(*texts[id], hk)) continue;
        if (RegisterHotKey(hwnd_, id, hk.modifiers | MOD_NOREPEAT, hk.vk)) {
            hotkeyRegistered_[id] = true;
        } else {
            SNOW_LOGW("hotkey %s is taken (error %lu)", texts[id]->c_str(), GetLastError());
            if (!failed.empty()) failed += L", ";
            failed += widen(*texts[id]);
        }
    }
    if (!failed.empty() && notifyConflicts) {
        tray_.showBalloon(L"SnowFall",
                          std::wstring(tx(L"Горячие клавиши заняты другой программой: ",
                                          L"Hotkeys are used by another program: ")) +
                              failed + tx(L". Измените их в настройках.", L". Change them in Settings."),
                          true);
    }
}

std::wstring AppHost::validateHotkey(const std::string& text) {
    if (text.empty()) return {};
    Hotkey hk;
    if (!parseHotkey(text, hk)) return tx(L"Недопустимое сочетание", L"Invalid shortcut");
    // Is it one of ours already?
    const std::string* ours[] = {&settings_.hotkeys.pauseToggle, &settings_.hotkeys.clearSnow,
                                 &settings_.hotkeys.interactionToggle};
    for (int i = 0; i < 3; ++i) {
        Hotkey o;
        if (parseHotkey(*ours[i], o) && o == hk && hotkeyRegistered_[i + 1]) return {};
    }
    if (RegisterHotKey(hwnd_, kHotkeyProbe, hk.modifiers | MOD_NOREPEAT, hk.vk)) {
        UnregisterHotKey(hwnd_, kHotkeyProbe);
        return {};
    }
    return tx(L"Сочетание занято другой программой", L"Shortcut is used by another program");
}

void AppHost::onHotkey(int id) {
    switch (id) {
        case kHotkeyPause: setPaused(settings_.enabled); break;
        case kHotkeyClear: clearSnow(); break;
        case kHotkeyInteraction: interaction_.setToggled(!interaction_.toggled()); updateTray(); break;
        default: break;
    }
}

// ---- tray ----------------------------------------------------------------

void AppHost::onTrayCommand(TrayCommand cmd) {
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
        case TrayCommand::TogglePause: setPaused(settings_.enabled); break;
        case TrayCommand::PresetLight: preset(WeatherPreset::Light); break;
        case TrayCommand::PresetNormal: preset(WeatherPreset::Normal); break;
        case TrayCommand::PresetBlizzard: preset(WeatherPreset::Blizzard); break;
        case TrayCommand::OpenSettings: openSettings(); break;
        case TrayCommand::ToggleInteraction:
            interaction_.setToggled(!interaction_.toggled());
            updateTray();
            break;
        case TrayCommand::ToolMitten: tool(Tool::Mitten); break;
        case TrayCommand::ToolShovel: tool(Tool::Shovel); break;
        case TrayCommand::ToolSnowball: tool(Tool::Snowball); break;
        case TrayCommand::ClearSnow: clearSnow(); break;
        case TrayCommand::ToggleHideAll:
            inputs_.hideAll = !inputs_.hideAll;
            updateGovernor();
            break;
        case TrayCommand::ToggleAutostart:
            s.startupWithWindows = !s.startupWithWindows;
            applySettings(s, false);
            break;
        case TrayCommand::CallSnowplow: runner_.triggerSnowplow(); break;
        case TrayCommand::PresetThaw: preset(WeatherPreset::Thaw); break;
        case TrayCommand::CallFireworks: runner_.triggerFireworks(); break;
        case TrayCommand::ApplyUserPreset: break;   // handled in handle(): needs lParam
        case TrayCommand::OpenQuickPanel: tray_.showQuickPanel(); break;
        case TrayCommand::Exit: PostQuitMessage(0); break;
    }
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

void AppHost::setPaused(bool paused) {
    Settings s = settings_;
    s.enabled = !paused;
    applySettings(s, false);
}

void AppHost::clearSnow() { runner_.clearSnow(); }

void AppHost::openSettings() {
    settingsWindow_.show(settings_);
    syncTrackerWindows();
}

void AppHost::updateTray() {
    TrayState st;
    st.paused = !settings_.enabled || decision_.autoPaused;
    st.hidden = inputs_.hideAll;
    st.interactionMode = interaction_.active();
    st.autostart = settings_.startupWithWindows;
    st.preset = settings_.weather.preset;
    st.tool = settings_.interaction.tool;
    st.leftClickTogglesPause = settings_.system.trayLeftClickTogglesPause;
    for (const auto& p : settings_.userPresets) st.userPresetNames.push_back(widen(p.name));
    st.interactionKeys = holdKeysText();
    // Slider drags call this many times per second: only touch the shell (NIM_MODIFY is a
    // cross-process call) when something the tray shows actually changed.
    const bool same = trayStateValid_ && st.paused == lastTray_.paused && st.hidden == lastTray_.hidden &&
                      st.interactionMode == lastTray_.interactionMode && st.autostart == lastTray_.autostart &&
                      st.preset == lastTray_.preset && st.tool == lastTray_.tool &&
                      st.leftClickTogglesPause == lastTray_.leftClickTogglesPause &&
                      st.userPresetNames == lastTray_.userPresetNames &&
                      st.interactionKeys == lastTray_.interactionKeys;
    if (same) return;
    lastTray_ = st;
    trayStateValid_ = true;
    tray_.setState(st);
}

// ---- diagnostics ---------------------------------------------------------

std::wstring AppHost::holdKeysText() const {
    if (!settings_.interaction.enabled || !settings_.interaction.holdKeyEnabled) return {};
    return InteractionController::chordName(interaction_.holdChord());
}

std::wstring AppHost::statusText() const {
    switch (decision_.reason) {
        case PauseReason::None:
            return interaction_.active() ? tx(L"Работает — режим взаимодействия", L"Running — interaction mode")
                                         : tx(L"Работает", L"Running");
        case PauseReason::User: return tx(L"Пауза", L"Paused");
        case PauseReason::Hidden: return tx(L"Скрыто", L"Hidden");
        case PauseReason::Fullscreen:
            return tx(L"Пауза (полноэкранное приложение)", L"Paused (fullscreen application)");
        case PauseReason::Locked: return tx(L"Пауза (сеанс заблокирован)", L"Paused (session locked)");
        case PauseReason::Suspended: return tx(L"Пауза (спящий режим)", L"Paused (sleep)");
        case PauseReason::RenderFailed: return tx(L"Графика отключена (ошибка устройства)", L"Graphics disabled");
    }
    return {};
}

DiagnosticsInfo AppHost::diagnostics() const {
    DiagnosticsInfo d;
    const RenderStats rs = overlay_.stats();
    d.fps = rs.fps;
    d.frameMs = rs.frameMs;
    d.deviceResets = rs.deviceResets;
    if (auto snap = snapshots_.latest()) {
        d.simStepMs = snap->stats.stepMs;
        d.particles = snap->stats.particles;
        d.surfaces = snap->stats.surfaces;
        d.snowballs = snap->stats.snowballs;
        d.snowmen = snap->stats.snowmen;
    }
    d.monitors = static_cast<int>(monitors_.size());
    d.trackedWindows = layout_ ? static_cast<int>(layout_->windows.size()) : 0;
    d.paused = !settings_.enabled;
    d.autoPaused = decision_.autoPaused;
    d.onBattery = inputs_.onBattery;
    d.statusText = statusText();
    d.logPath = logPath_;
    d.interactionKeys = holdKeysText();
    return d;
}

}  // namespace snow
