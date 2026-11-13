// Minimal thread-safe logger. Never log window titles, text or other personal data:
// only numeric ids, sizes, HRESULTs and executable-independent state.
#pragma once
#include <string>

namespace snow::log {

enum class Level { Debug, Info, Warn, Error };

// The log never grows without bound: when the file exceeds the size limit it becomes
// "<file>.1" (replacing the previous one) and a new file starts. Compact (default) keeps the
// last few thousand records, enough to see what happened before a crash.
inline constexpr long kCompactLogBytes = 256 * 1024;
inline constexpr long kDetailedLogBytes = 2 * 1024 * 1024;

// Opens (appends to) the log file. Safe to skip: without init() messages only go to the
// debugger output / stderr.
void init(const std::wstring& filePath, Level minLevel = Level::Info);
// Changes the level and the size limit at run time (settings: performance.detailedLog).
void configure(Level minLevel, long maxBytes);
void shutdown();
void write(Level level, const char* fmt, ...);

}  // namespace snow::log

#define SNOW_LOGD(...) ::snow::log::write(::snow::log::Level::Debug, __VA_ARGS__)
#define SNOW_LOGI(...) ::snow::log::write(::snow::log::Level::Info, __VA_ARGS__)
#define SNOW_LOGW(...) ::snow::log::write(::snow::log::Level::Warn, __VA_ARGS__)
#define SNOW_LOGE(...) ::snow::log::write(::snow::log::Level::Error, __VA_ARGS__)
