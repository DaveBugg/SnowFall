// Portable logger backend (stderr) for non-Windows builds of the core (unit tests, future
// macOS port until it gets its own backend). Windows uses platform/windows/diagnostics/log_win.cpp.
#ifndef _WIN32
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>

#include "diagnostics/log.h"

namespace snow::log {
namespace {
std::mutex g_mutex;
std::atomic<Level> g_min{Level::Info};   // read without the lock in write()
}  // namespace

void init(const std::wstring&, Level minLevel) { g_min = minLevel; }
void configure(Level minLevel, long) { g_min = minLevel; }
void shutdown() {}

void write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fprintf(stderr, "[%s] %s\n", names[static_cast<int>(level)], msg);
}

}  // namespace snow::log
#endif
