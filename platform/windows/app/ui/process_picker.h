// «Выбрать из запущенных…»: lists the executables of processes that own visible top-level
// windows (exe name + icon only; window titles are NEVER read) and lets the user pick several.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/settings.h"

namespace snow::ui {

struct RunningApp {
    std::wstring exeName;   // e.g. L"notepad.exe"
    std::wstring path;      // full image path (used only to extract the icon)
};

// Sorted by name, unique (case-insensitive).
std::vector<RunningApp> listRunningApps();

class ProcessPicker {
public:
    using AddFn = std::function<void(const std::vector<std::wstring>& exeNames)>;
    ProcessPicker();
    ~ProcessPicker();
    ProcessPicker(const ProcessPicker&) = delete;
    ProcessPicker& operator=(const ProcessPicker&) = delete;

    // Modeless themed popup owned by `owner`. «Добавить» calls onAdd and closes.
    bool open(HINSTANCE instance, HWND owner, UiTheme theme, AddFn onAdd);
    void close();
    HWND hwnd() const;
    bool preTranslateMessage(MSG* msg);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace snow::ui
