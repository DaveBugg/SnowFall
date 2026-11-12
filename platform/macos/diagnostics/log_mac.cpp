// macOS logger backend (target snow_diag on macOS; the core's stderr-only log_portable.cpp is
// not built when the platform provides its own, see SNOW_PLATFORM_DIAG in the root CMake).
//
// File: ~/Library/Logs/SnowFall/snowfall.log (chosen by the app via init()), appended,
// rotated to snowfall.log.1 above 1 MB. Lines are also mirrored to stderr when
// SNOW_LOG_STDERR=1 is set or before init() (unit tests, --selftest output in CI).
// Pure POSIX C++: also linked by the core unit tests on macOS.
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "diagnostics/log.h"

namespace snow::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
Level g_min = Level::Info;
long g_written = 0;
bool g_stderr = true;   // until init() opened a file
constexpr long kMaxBytes = 1024 * 1024;

std::string utf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w) {
        auto c = static_cast<std::uint32_t>(wc);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c <= 0x10FFFF) {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void openLocked() {
    g_file = std::fopen(g_path.c_str(), "ab");
    if (g_file) {
        std::fseek(g_file, 0, SEEK_END);
        g_written = std::ftell(g_file);
    }
}

void rotateLocked() {
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    const std::string old = g_path + ".1";
    std::rename(g_path.c_str(), old.c_str());
    g_written = 0;
    openLocked();
}

bool envStderr() {
    const char* e = std::getenv("SNOW_LOG_STDERR");
    return e && e[0] == '1';
}

}  // namespace

void init(const std::wstring& filePath, Level minLevel) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_path = utf8(filePath);
    g_min = minLevel;
    if (!g_path.empty()) {
        // Create the parent directory (one level is enough: ~/Library/Logs exists).
        const std::size_t slash = g_path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) ::mkdir(g_path.substr(0, slash).c_str(), 0755);
        openLocked();
        if (g_file && g_written > kMaxBytes) rotateLocked();
    }
    g_stderr = !g_file || envStderr();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_stderr = true;
}

void write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    static const char* names[] = {"DBG", "INF", "WRN", "ERR"};

    timeval tv{};
    gettimeofday(&tv, nullptr);
    std::tm tm{};
    const std::time_t secs = tv.tv_sec;
    localtime_r(&secs, &tm);
    std::uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    char line[1200];
    int n = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [%llu] %s\n", tm.tm_year + 1900,
                          tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<int>(tv.tv_usec / 1000), names[static_cast<int>(level)],
                          static_cast<unsigned long long>(tid), msg);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof line)) n = static_cast<int>(sizeof line) - 1;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_stderr) std::fwrite(line, 1, static_cast<std::size_t>(n), stderr);
    if (!g_file) return;
    std::fwrite(line, 1, static_cast<std::size_t>(n), g_file);
    std::fflush(g_file);
    g_written += n;
    if (g_written > kMaxBytes) rotateLocked();
}

}  // namespace snow::log
