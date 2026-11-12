// Windows implementation of SettingsStore: data directory and atomic file I/O.
// JSON mapping / sanitize() are portable and live in core/settings/settings_json.cpp.
#include "settings/settings_store.h"

#include <windows.h>

#include <shlobj.h>

#include <string>

#include "diagnostics/log.h"

// NOTE on logging: file paths contain the user name, so only error codes are logged.

namespace snow {

namespace {

// A larger settings file is treated as corrupt.
constexpr LONGLONG kMaxSettingsFileBytes = 4 * 1024 * 1024;

bool readWholeFile(const std::wstring& path, std::string& out, DWORD& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok && size.QuadPart > kMaxSettingsFileBytes) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        ok = false;
    }
    if (ok) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            ok = ReadFile(h, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr) != 0;
            if (ok && got == 0) break;   // file shrank underneath us
            done += got;
        }
        if (ok) out.resize(done);
    }
    error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return ok;
}

bool writeWholeFileFlushed(const std::wstring& path, const std::string& data, DWORD& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        DWORD written = 0;
        ok = WriteFile(h, data.data() + done, static_cast<DWORD>(data.size() - done), &written, nullptr) != 0 &&
             written != 0;
        done += written;
    }
    ok = ok && FlushFileBuffers(h) != 0;
    error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return ok;
}

std::wstring parentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

// ---------------------------------------------------------------- public: store

namespace {
std::wstring& dataDirOverride() {
    static std::wstring dir;
    return dir;
}
}  // namespace

void SettingsStore::setDataDirectoryOverride(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    dataDirOverride() = std::move(dir);
}

std::wstring SettingsStore::dataDirectory() {
    if (!dataDirOverride().empty()) return dataDirOverride();
    std::wstring base;
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known)) && known) {
        base = known;
    }
    CoTaskMemFree(known);
    if (base.empty()) {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        base = (n > 0 && n < MAX_PATH) ? buf : L".";
        SNOW_LOGW("settings: SHGetKnownFolderPath failed, using a fallback data directory");
    }
    std::wstring dir = base + L"\\SnowFall";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        SNOW_LOGW("settings: cannot create the data directory (error %lu)", GetLastError());
    }
    return dir;
}

std::wstring SettingsStore::defaultPath() { return dataDirectory() + L"\\settings.json"; }

SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)) {}

bool SettingsStore::load(Settings& out) {
    std::string text;
    DWORD error = ERROR_SUCCESS;
    if (!readWholeFile(path_, text, error)) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            out = Settings{};   // first run
            return true;
        }
        SNOW_LOGW("settings: cannot read the settings file (error %lu); using defaults", error);
    } else {
        std::string parseError;
        if (fromJson(text, out, &parseError)) return true;
        SNOW_LOGW("settings: settings file is corrupt (%s); using defaults", parseError.c_str());
    }

    out = Settings{};
    const std::wstring corrupt = path_ + L".corrupt";
    if (!MoveFileExW(path_.c_str(), corrupt.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SNOW_LOGW("settings: cannot preserve the bad file as .corrupt (error %lu)", GetLastError());
    }
    return false;
}

bool SettingsStore::save(const Settings& s) {
    const std::string text = toJson(s);
    const std::wstring tmp = path_ + L".tmp";

    if (const std::wstring dir = parentDirectory(path_); !dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr);   // best effort; failure surfaces below
    }

    DWORD error = ERROR_SUCCESS;
    if (!writeWholeFileFlushed(tmp, text, error)) {
        SNOW_LOGW("settings: cannot write the temporary settings file (error %lu)", error);
        DeleteFileW(tmp.c_str());
        return false;
    }

    // ReplaceFileW swaps atomically and keeps the destination's attributes/ACLs, but it
    // requires the destination to exist; the first save goes through MoveFileExW.
    if (ReplaceFileW(path_.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        return true;
    }
    const DWORD replaceError = GetLastError();
    if (MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    SNOW_LOGW("settings: cannot replace the settings file (errors %lu / %lu)", replaceError, GetLastError());
    DeleteFileW(tmp.c_str());
    return false;
}

}  // namespace snow
