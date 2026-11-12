// SettingsWindow: separate Win32 window (NOT part of the overlay) with tabs
// «Снег», «Сугробы», «Инструменты», «Производительность», «Система», «О программе».
// Every change is applied immediately through the onChanged callback. UI thread only.
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer. Keep state behind Impl.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "core/settings.h"

namespace snow {

// Live values shown on the «О программе» / diagnostics area (refreshed ~2x per second
// through the getDiagnostics callback while the window is open).
struct DiagnosticsInfo {
    float fps = 0;
    float frameMs = 0;
    float simStepMs = 0;
    unsigned particles = 0;
    unsigned surfaces = 0;
    unsigned snowballs = 0;
    unsigned snowmen = 0;
    int monitors = 0;
    int trackedWindows = 0;
    int deviceResets = 0;
    bool paused = false;
    bool autoPaused = false;        // fullscreen / session lock / battery
    bool onBattery = false;
    std::wstring statusText;        // e.g. "Работает", "Пауза (полноэкранное приложение)"
    std::wstring logPath;
    std::wstring interactionKeys;   // current hold chord for interaction mode, e.g. L"Ctrl+Alt"
};

struct SettingsWindowCallbacks {
    std::function<void(const Settings&)> onChanged;      // any edit; apply + persist
    std::function<void()> onClearSnow;                   // «Очистить всё»
    std::function<DiagnosticsInfo()> getDiagnostics;
    // Returns empty string if the hotkey can be registered, else a human-readable reason;
    // used to warn about conflicts right in the UI.
    std::function<std::wstring(const std::string& hotkeyText)> validateHotkey;
    std::function<void()> onFireworks;                    // «Салют!» (launch fireworks now)
};

class SettingsWindow {
public:
    SettingsWindow();
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void initialize(HINSTANCE instance, SettingsWindowCallbacks callbacks);

    // Creates the window on first use, then shows and activates it.
    void show(const Settings& current);
    void close();
    bool isOpen() const;
    HWND hwnd() const;   // nullptr when closed

    // Push externally changed settings (tray/hotkeys) into the open window's controls.
    void update(const Settings& current);

    // Call from the message loop before TranslateMessage: handles Tab/keyboard navigation.
    bool preTranslateMessage(MSG* msg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow
