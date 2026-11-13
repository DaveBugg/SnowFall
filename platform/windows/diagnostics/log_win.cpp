#include "diagnostics/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>

namespace snow::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
std::atomic<Level> g_min{Level::Info};   // read without the lock in write()
long g_written = 0;
long g_maxBytes = kCompactLogBytes;

void openLocked() {
    g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYNO);
    if (g_file) {
        fseek(g_file, 0, SEEK_END);
        g_written = ftell(g_file);
    }
}

void rotateLocked() {
    if (g_file) fclose(g_file);
    g_file = nullptr;
    std::wstring old = g_path + L".1";
    MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_written = 0;
    openLocked();
}
}  // namespace

void init(const std::wstring& filePath, Level minLevel) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    g_path = filePath;
    g_min = minLevel;
    openLocked();
    if (g_file && g_written > g_maxBytes) rotateLocked();
}

void configure(Level minLevel, long maxBytes) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_min = minLevel;
    g_maxBytes = maxBytes > 16 * 1024 ? maxBytes : 16 * 1024;
    if (g_file && g_written > g_maxBytes) rotateLocked();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [%lu] %s\r\n", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, names[static_cast<int>(level)],
                     GetCurrentThreadId(), msg);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof line)) n = sizeof line - 1;

    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_file) return;
    fwrite(line, 1, static_cast<size_t>(n), g_file);
    fflush(g_file);
    g_written += n;
    if (g_written > g_maxBytes) rotateLocked();
}

}  // namespace snow::log
