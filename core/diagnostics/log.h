// Minimal thread-safe logger. Never log window titles, text or other personal data:
// only numeric ids, sizes, HRESULTs and executable-independent state.
#pragma once
#include <string>

namespace snow::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (appends to) the log file; rotates it to "<file>.1" when it exceeds 1 MB.
// Safe to skip: without init() messages only go to OutputDebugString.
void init(const std::wstring& filePath, Level minLevel = Level::Info);
void shutdown();
void write(Level level, const char* fmt, ...);

}  // namespace snow::log

#define SNOW_LOGD(...) ::snow::log::write(::snow::log::Level::Debug, __VA_ARGS__)
#define SNOW_LOGI(...) ::snow::log::write(::snow::log::Level::Info, __VA_ARGS__)
#define SNOW_LOGW(...) ::snow::log::write(::snow::log::Level::Warn, __VA_ARGS__)
#define SNOW_LOGE(...) ::snow::log::write(::snow::log::Level::Error, __VA_ARGS__)
