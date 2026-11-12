#include "app/legacy_migration.h"

#include <windows.h>
#include <shlobj.h>

#include "diagnostics/log.h"

namespace snow::legacy {
namespace {
constexpr wchar_t kOldHostClass[] = L"WindowsSnowHost";
constexpr wchar_t kOldFolder[] = L"WindowsSnow";
constexpr wchar_t kOldRunValue[] = L"WindowsSnow";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kOldRemoteMsg = WM_APP + 120;   // same remote protocol: wParam 2 = exit

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

bool stopOldInstance() {
    HWND old = FindWindowW(kOldHostClass, nullptr);
    if (!old) return false;
    PostMessageW(old, kOldRemoteMsg, 2, 0);
    for (int i = 0; i < 40 && IsWindow(old); ++i) Sleep(50);   // up to 2 s
    SNOW_LOGI("legacy: asked the old Windows Snow instance to exit");
    return true;
}

bool migrateSettings(const std::wstring& newDataDir) {
    const std::wstring target = newDataDir + L"\\settings.json";
    if (exists(target)) return false;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) {
        CoTaskMemFree(local);
        return false;
    }
    const std::wstring source = std::wstring(local) + L"\\" + kOldFolder + L"\\settings.json";
    CoTaskMemFree(local);
    if (!exists(source)) return false;
    if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) return false;
    SNOW_LOGI("legacy: settings copied from the old Windows Snow folder");
    return true;
}

bool takeOldAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const bool had = RegQueryValueExW(key, kOldRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (had) {
        RegDeleteValueW(key, kOldRunValue);
        SNOW_LOGI("legacy: removed the old Windows Snow autostart entry");
    }
    RegCloseKey(key);
    return had;
}

}  // namespace snow::legacy
