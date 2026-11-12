// macOS implementation of SettingsStore (core/settings/settings_store.h): data directory and
// atomic file I/O with POSIX calls. JSON mapping / sanitize() live in the core.
//
//   data directory  ~/Library/Application Support/SnowFall   (--data-dir overrides)
//   settings        <data>/settings.json, written as settings.json.tmp + fsync + rename(2)
//   corrupt file    preserved as settings.json.corrupt
//
// Paths contain the user name, so only errno values are logged.
#include "settings/settings_store.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "diagnostics/log.h"
#include "system/text_util.h"

namespace snow {
namespace {

using mac::fromUtf8;
using mac::toUtf8;

constexpr off_t kMaxSettingsFileBytes = 4 * 1024 * 1024;   // larger = corrupt

std::wstring& dataDirOverride() {
    static std::wstring dir;
    return dir;
}

// mkdir -p
bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string homeDirectory() {
    if (const char* h = std::getenv("HOME"); h && h[0]) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/tmp";
}

bool readWholeFile(const std::string& path, std::string& out, int& err) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        return false;
    }
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0;
    if (ok && st.st_size > kMaxSettingsFileBytes) {
        errno = EFBIG;
        ok = false;
    }
    if (ok) {
        out.assign(static_cast<std::size_t>(st.st_size), '\0');
        std::size_t done = 0;
        while (done < out.size()) {
            const ssize_t got = ::read(fd, out.data() + done, out.size() - done);
            if (got < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (got == 0) break;   // file shrank underneath us
            done += static_cast<std::size_t>(got);
        }
        if (ok) out.resize(done);
    }
    err = ok ? 0 : errno;
    ::close(fd);
    return ok;
}

bool writeWholeFileSynced(const std::string& path, const std::string& data, int& err) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = errno;
        return false;
    }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else {
            done += static_cast<std::size_t>(n);
        }
    }
    // fsync is enough for a settings file (F_FULLFSYNC would flush the disk cache too).
    ok = ok && ::fsync(fd) == 0;
    err = ok ? 0 : errno;
    ::close(fd);
    return ok;
}

}  // namespace

void SettingsStore::setDataDirectoryOverride(std::wstring dir) {
    while (dir.size() > 1 && dir.back() == L'/') dir.pop_back();
    if (!dir.empty()) makeDirs(toUtf8(dir));
    dataDirOverride() = std::move(dir);
}

std::wstring SettingsStore::dataDirectory() {
    if (!dataDirOverride().empty()) return dataDirOverride();
    const std::string dir = homeDirectory() + "/Library/Application Support/SnowFall";
    if (!makeDirs(dir)) SNOW_LOGW("settings: cannot create the data directory (errno %d)", errno);
    return fromUtf8(dir);
}

std::wstring SettingsStore::defaultPath() { return dataDirectory() + L"/settings.json"; }

SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)) {}

bool SettingsStore::load(Settings& out) {
    const std::string path = toUtf8(path_);
    std::string text;
    int err = 0;
    if (!readWholeFile(path, text, err)) {
        if (err == ENOENT || err == ENOTDIR) {
            out = Settings{};   // first run
            return true;
        }
        SNOW_LOGW("settings: cannot read the settings file (errno %d); using defaults", err);
    } else {
        std::string parseError;
        if (fromJson(text, out, &parseError)) return true;
        SNOW_LOGW("settings: settings file is corrupt (%s); using defaults", parseError.c_str());
    }
    out = Settings{};
    const std::string corrupt = path + ".corrupt";
    if (::rename(path.c_str(), corrupt.c_str()) != 0) {
        SNOW_LOGW("settings: cannot preserve the bad file as .corrupt (errno %d)", errno);
    }
    return false;
}

bool SettingsStore::save(const Settings& s) {
    const std::string path = toUtf8(path_);
    const std::string text = toJson(s);
    const std::string tmp = path + ".tmp";
    if (const std::size_t slash = path.find_last_of('/'); slash != std::string::npos && slash > 0) {
        makeDirs(path.substr(0, slash));   // best effort; failure surfaces below
    }
    int err = 0;
    if (!writeWholeFileSynced(tmp, text, err)) {
        SNOW_LOGW("settings: cannot write the temporary settings file (errno %d)", err);
        ::unlink(tmp.c_str());
        return false;
    }
    // rename(2) atomically replaces the destination on the same volume.
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        SNOW_LOGW("settings: cannot replace the settings file (errno %d)", errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace snow
