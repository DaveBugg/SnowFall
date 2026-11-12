// SettingsStore: JSON persistence of snow::Settings with schema versioning, migration,
// validation/clamping and atomic writes (temp file + replace).
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer.
#pragma once
#include <string>

#include "core/settings.h"

namespace snow {

// Clamp every field to its documented range (see core/settings.h and the spec):
// intensity 0..100, wind -100..100, gustiness 0..100, size 50..200, fallSpeed 25..200,
// opacity 10..100, maxHeight 0..100, driftOpacity 10..100, snowplowDelay 1..120 min,
// brush 12..48, shovel 48..160, fpsCap 30|60,
// telemetry always false, schemaVersion = current.
void sanitize(Settings& s);

// JSON <-> Settings. fromJson: unknown keys ignored, missing keys keep defaults, older
// schemaVersion migrated, result sanitized. Returns false only if `json` is not valid JSON
// or not an object (then `out` is untouched).
std::string toJson(const Settings& s);
bool fromJson(const std::string& json, Settings& out, std::string* error = nullptr);

class SettingsStore {
public:
    // Default location: %LOCALAPPDATA%\SnowFall\settings.json
    static std::wstring defaultPath();
    static std::wstring dataDirectory();   // %LOCALAPPDATA%\SnowFall (created if missing)
    // Portable mode / tests: use `dir` instead (command line --data-dir). Call before anything else.
    static void setDataDirectoryOverride(std::wstring dir);

    explicit SettingsStore(std::wstring path = defaultPath());

    // Missing file -> defaults (returns true). Corrupt file -> defaults, the bad file is
    // preserved as settings.json.corrupt, returns false.
    bool load(Settings& out);
    // Atomic: write "<path>.tmp", flush, then replace. Returns false on I/O failure.
    bool save(const Settings& s);

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

}  // namespace snow
