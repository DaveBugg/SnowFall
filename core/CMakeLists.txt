# Portable core of SnowFall — no OS headers allowed in here (see docs/architecture.md).
#
#   snow_sim       simulation + surfaces + interaction (World facade, physics, tools, snowmen)
#   snow_settings  settings model, JSON mapping, migrations, sanitize(), hotkey grammar
#                  (SettingsStore file I/O is implemented by the platform layer)
#   snow_diag      logger backend: portable stderr version here on non-Windows;
#                  Windows provides its own (platform/windows/diagnostics)

snow_module(snow_sim      "${CMAKE_CURRENT_SOURCE_DIR}" core simulation surfaces interaction runtime i18n)
snow_module(snow_settings "${CMAKE_CURRENT_SOURCE_DIR}" settings)

if(NOT WIN32)
  add_library(snow_diag STATIC diagnostics/log_portable.cpp diagnostics/log.h)
  target_include_directories(snow_diag PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  find_package(Threads REQUIRED)
  target_link_libraries(snow_diag PUBLIC Threads::Threads)
endif()

target_link_libraries(snow_sim PUBLIC snow_diag)
target_link_libraries(snow_settings PUBLIC snow_diag)

# Unit tests of the core (run on every platform).
snow_exe(snow_sim_tests      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sim"      snow_sim snow_settings)
snow_exe(snow_settings_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/settings" snow_settings)
add_test(NAME sim COMMAND snow_sim_tests)
add_test(NAME settings COMMAND snow_settings_tests)
