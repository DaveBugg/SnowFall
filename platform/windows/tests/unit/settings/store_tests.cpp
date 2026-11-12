// SettingsStore file behaviour, exercised in a temp directory whose name contains Cyrillic
// characters (user profiles with Russian names must work).
#include <windows.h>

#include "settings/json.h"
#include "settings/settings_store.h"
#include "snow_test.h"

using namespace snow;

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool writeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    CloseHandle(h);
    return ok;
}

// Temp directory "%TEMP%\SnowFall Тест Снег <pid>-<n>", removed with its files on scope exit.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        dir_ = std::wstring(temp) + L"SnowFall Тест Снег " + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(++counter);
        CHECK(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
    }
    ~TempDir() {
        for (const wchar_t* name : {L"settings.json", L"settings.json.tmp", L"settings.json.corrupt"}) {
            const std::wstring file = dir_ + L"\\" + name;
            SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(file.c_str());
        }
        RemoveDirectoryW((dir_ + L"\\вложенная").c_str());
        CHECK(RemoveDirectoryW(dir_.c_str()) != 0);   // fails if anything unexpected is left
    }
    std::wstring file() const { return dir_ + L"\\settings.json"; }
    std::wstring tmp() const { return file() + L".tmp"; }
    std::wstring corrupt() const { return file() + L".corrupt"; }
    const std::wstring& dir() const { return dir_; }

private:
    std::wstring dir_;
};

Settings sample(int n = 0) {
    Settings s;
    s.enabled = (n % 2) == 0;
    s.startupWithWindows = true;
    s.weather.preset = WeatherPreset::Blizzard;
    s.weather.intensity = n % 101;
    s.weather.wind = -40;
    s.accumulation.maxHeightPxAt96Dpi = 80;
    s.interaction.tool = Tool::Shovel;
    s.performance.fpsCap = 30;
    s.hotkeys.clearSnow = "";
    s.system.language = Language::Russian;
    s.system.excludedProcesses = {"игра.exe", "game" + std::to_string(n) + ".exe"};
    return s;
}

}  // namespace

TEST(store_missing_file_gives_defaults) {
    TempDir t;
    SettingsStore store(t.file());
    CHECK(store.path() == t.file());
    Settings s = sample();
    CHECK(store.load(s) == true);
    CHECK(s == Settings{});
    CHECK(!exists(t.file()) && !exists(t.corrupt()) && !exists(t.tmp()));   // load creates nothing

    // A missing directory is also just "no settings yet".
    SettingsStore nowhere(t.dir() + L"\\нет такой папки\\settings.json");
    Settings n = sample();
    CHECK(nowhere.load(n) == true);
    CHECK(n == Settings{});
}

TEST(store_save_load_roundtrip_cyrillic_path) {
    TempDir t;
    SettingsStore store(t.file());
    const Settings saved = sample(7);
    CHECK(store.save(saved));
    CHECK(exists(t.file()));
    CHECK(!exists(t.tmp()));       // no leftover temp file
    CHECK(!exists(t.corrupt()));

    Settings back;
    SettingsStore other(t.file());   // a fresh instance reads the same file
    CHECK(other.load(back));
    CHECK(back == saved);
    CHECK(!exists(t.tmp()) && !exists(t.corrupt()));
}

TEST(store_saved_file_is_utf8_json_without_bom) {
    TempDir t;
    SettingsStore store(t.file());
    const Settings saved = sample(3);
    CHECK(store.save(saved));

    std::string bytes;
    CHECK(readFile(t.file(), bytes));
    CHECK(bytes == toJson(saved));
    CHECK(bytes.size() > 3 && bytes[0] == '{');                      // no BOM
    CHECK(bytes.compare(0, 3, "\xEF\xBB\xBF") != 0);
    CHECK(bytes.find('\r') == std::string::npos);
    CHECK(bytes.find("\"игра.exe\"") != std::string::npos);          // raw UTF-8, not \u escapes
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                              nullptr, 0) > 0);                      // strictly valid UTF-8
    json::Value root;
    std::string err;
    CHECK(json::parse(bytes, root, &err));
    CHECK(root.isObject() && root.find("schemaVersion")->asNumber() == kSettingsSchemaVersion);
}

TEST(store_corrupt_file_gives_defaults_and_backup) {
    TempDir t;
    SettingsStore store(t.file());
    const std::string garbage = "{ \"enabled\": tru";
    CHECK(writeFile(t.file(), garbage));

    Settings s = sample();
    CHECK(store.load(s) == false);
    CHECK(s == Settings{});
    CHECK(!exists(t.file()));
    std::string kept;
    CHECK(readFile(t.corrupt(), kept));
    CHECK(kept == garbage);

    // After the bad file was moved away the next load is a clean "missing file".
    CHECK(store.load(s) == true);

    // A second corrupt file overwrites the older .corrupt.
    const std::string garbage2 = "[1, 2, 3]";   // valid JSON, but not an object
    CHECK(writeFile(t.file(), garbage2));
    CHECK(store.load(s) == false);
    CHECK(s == Settings{});
    CHECK(readFile(t.corrupt(), kept));
    CHECK(kept == garbage2);
    CHECK(!exists(t.file()));

    // Empty and binary files are corrupt too.
    CHECK(writeFile(t.file(), ""));
    CHECK(store.load(s) == false);
    CHECK(writeFile(t.file(), std::string("\xFF\xFE{\0}\0", 6)));   // UTF-16 by mistake
    CHECK(store.load(s) == false);
    CHECK(s == Settings{});

    // Saving after a corrupt load works and leaves the backup alone.
    CHECK(store.save(sample(1)));
    CHECK(store.load(s) && s == sample(1));
    CHECK(exists(t.corrupt()));
}

TEST(store_repeated_overwrite) {
    TempDir t;
    SettingsStore store(t.file());
    for (int i = 0; i < 20; ++i) {
        const Settings saved = sample(i);
        CHECK(store.save(saved));
        CHECK(!exists(t.tmp()));
        Settings back;
        CHECK(store.load(back));
        CHECK(back == saved);
    }
    CHECK(!exists(t.corrupt()));
}

TEST(store_save_replaces_stale_tmp_and_sanitizes) {
    TempDir t;
    SettingsStore store(t.file());
    CHECK(writeFile(t.tmp(), "stale partial write from a crashed run"));
    Settings wild;
    wild.weather.intensity = 5000;
    wild.performance.fpsCap = 240;
    wild.privacy.telemetry = true;
    CHECK(store.save(wild));
    CHECK(!exists(t.tmp()));
    Settings back;
    CHECK(store.load(back));
    CHECK(back.weather.intensity == 100 && back.performance.fpsCap == 60 && !back.privacy.telemetry);
}

TEST(store_save_failure_keeps_existing_file) {
    TempDir t;
    SettingsStore store(t.file());
    CHECK(store.save(sample(1)));
    std::string before;
    CHECK(readFile(t.file(), before));

    // Hold the destination open without sharing: the replace must fail cleanly.
    HANDLE lock = CreateFileW(t.file().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(store.save(sample(2)) == false);
    CloseHandle(lock);

    CHECK(!exists(t.tmp()));
    std::string after;
    CHECK(readFile(t.file(), after));
    CHECK(after == before);   // never truncated or half-written
    Settings back;
    CHECK(store.load(back) && back == sample(1));

    // Unwritable location (the "file" path is an existing directory) -> false, no crash.
    SettingsStore bad(t.dir());
    CHECK(bad.save(sample(3)) == false);
    CHECK(!exists(t.dir() + L".tmp"));
}

TEST(store_save_creates_parent_directory) {
    TempDir t;
    const std::wstring nested = t.dir() + L"\\вложенная";
    SettingsStore store(nested + L"\\settings.json");
    CHECK(store.save(sample(4)));
    Settings back;
    CHECK(store.load(back) && back == sample(4));
    CHECK(DeleteFileW((nested + L"\\settings.json").c_str()) != 0);
}

TEST(store_default_location) {
    const std::wstring dir = SettingsStore::dataDirectory();
    const std::wstring suffix = L"\\SnowFall";
    CHECK(dir.size() > suffix.size() && dir.compare(dir.size() - suffix.size(), suffix.size(), suffix) == 0);
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK(SettingsStore::defaultPath() == dir + L"\\settings.json");
    // Not loading/saving here: the unit tests must not touch the user's real settings.
}
