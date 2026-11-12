// Quick panel: a small themed popup next to the tray icon (left click). Preset mini-cards,
// intensity and wind sliders, user preset chips and the most common actions.
// Keyboard: Tab / arrows / Space / Enter / Esc (closes). Closes when it loses activation.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>

#include "core/settings.h"
#include "tray_icon.h"

namespace snow::ui {

class QuickPanel {
public:
    using ChangeFn = std::function<void(const Settings&)>;
    using CommandFn = std::function<void(TrayCommand, LPARAM)>;

    QuickPanel();
    ~QuickPanel();
    QuickPanel(const QuickPanel&) = delete;
    QuickPanel& operator=(const QuickPanel&) = delete;

    // anchor: screen rect of the tray icon (the panel opens next to it, inside the work area).
    void show(HINSTANCE instance, const RECT& anchor, const Settings& s, const TrayState& st, ChangeFn onChange,
              CommandFn onCommand);
    void hide();
    void destroy();
    bool visible() const;
    HWND hwnd() const;
    void update(const Settings& s, const TrayState& st);
    ULONGLONG lastHideTick() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace snow::ui
