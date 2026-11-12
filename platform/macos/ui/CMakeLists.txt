# SnowFall macOS user interface: SwiftUI settings window + menu-bar quick panel.
#
# Target `snowfall_ui` (static Swift library, module name SnowFallUI) implements the protocols of
# platform/macos/bridge/SFBridge.h and the @objc(SFUIFactory) entry class.
#
# Two ways to build it:
#   * standalone (CI on mac/ui, local experiments):
#         cmake -S platform/macos/ui -B build-ui -G Ninja && cmake --build build-ui
#     also builds `snowfall_ui_demo`, an AppKit harness with a fake SFAppActions
#     (run `snowfall_ui_demo --selftest <out-dir>` to exercise the UI and render PNGs).
#   * from the app (platform/macos/CMakeLists.txt):
#         enable_language(OBJC OBJCXX Swift)     # in platform/macos, BEFORE add_subdirectory(ui)
#         add_subdirectory(ui)
#         target_link_libraries(SnowFall PRIVATE snowfall_ui)
#     Swift needs the Ninja or Xcode generator (Unix Makefiles cannot compile Swift).
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
  project(SnowFallUI VERSION 0.1.1 LANGUAGES C CXX OBJC OBJCXX Swift)
  set(SNOWFALL_UI_STANDALONE ON)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  # Mimic the root project's global flags so the standalone build catches the same problems
  # the integrated build would have (they must never reach swiftc).
  add_compile_options(-Wall -Wextra)
  add_compile_definitions(SNOW_VERSION="${PROJECT_VERSION}")
  set(SNOWFALL_UI_DEMO_DEFAULT ON)
else()
  get_property(_sf_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
  if(NOT "Swift" IN_LIST _sf_langs)
    message(FATAL_ERROR "snowfall_ui: call enable_language(OBJC OBJCXX Swift) in platform/macos/CMakeLists.txt "
                        "before add_subdirectory(ui) (and configure with -G Ninja or -G Xcode).")
  endif()
  set(SNOWFALL_UI_DEMO_DEFAULT OFF)
endif()

option(SNOWFALL_UI_DEMO "Build snowfall_ui_demo (UI harness with a fake app)" ${SNOWFALL_UI_DEMO_DEFAULT})
option(SNOWFALL_UI_WERROR "Treat Swift / Objective-C warnings in the UI module as errors" OFF)

# The root project adds C/C++ flags (-Wall, SNOW_VERSION="…") for every language; swiftc rejects
# both, so this directory starts from a clean slate and adds per-language flags below.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

set(SF_BRIDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bridge")
get_filename_component(SF_BRIDGE_DIR "${SF_BRIDGE_DIR}" ABSOLUTE)
get_filename_component(SF_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../core" ABSOLUTE)
set(SF_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Clang module for the bridge header so Swift can `import SFBridge` (no bridging header needed,
# which static Swift libraries do not support well).
set(SF_BRIDGE_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/SFBridgeModule")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SFBridge.modulemap.in" "${SF_BRIDGE_MODULE_DIR}/module.modulemap" @ONLY)

file(GLOB SF_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Sources/SnowFallUI/*.swift")
add_library(snowfall_ui STATIC ${SF_UI_SOURCES})
set_target_properties(snowfall_ui PROPERTIES
  Swift_MODULE_NAME SnowFallUI
  Swift_LANGUAGE_VERSION 5
  Swift_MODULE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/swift")
target_compile_options(snowfall_ui PUBLIC
  "$<$<COMPILE_LANGUAGE:Swift>:-I${SF_BRIDGE_MODULE_DIR}>")
if(SNOWFALL_UI_WERROR)
  target_compile_options(snowfall_ui PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-warnings-as-errors>")
endif()
target_include_directories(snowfall_ui INTERFACE "${SF_BRIDGE_DIR}")
# The app finds the UI with NSClassFromString(@"SFUIFactory"); nothing references the Swift
# objects statically, so tell the linker to keep every Objective-C class of the archive.
target_link_options(snowfall_ui INTERFACE "LINKER:-ObjC")
target_link_libraries(snowfall_ui PUBLIC "-framework AppKit" "-framework SwiftUI" "-framework Combine")

if(SNOWFALL_UI_DEMO)
  add_executable(snowfall_ui_demo demo/main.mm demo/demo_strings.mm demo/demo_strings.h)
  target_include_directories(snowfall_ui_demo PRIVATE "${SF_BRIDGE_DIR}" "${SF_CORE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/selftest" "${CMAKE_CURRENT_SOURCE_DIR}/demo")
  target_compile_options(snowfall_ui_demo PRIVATE -fobjc-arc -Wall -Wextra)
  if(SNOWFALL_UI_WERROR)
    target_compile_options(snowfall_ui_demo PRIVATE -Werror)
  endif()
  set_target_properties(snowfall_ui_demo PROPERTIES CXX_STANDARD 20 OBJCXX_STANDARD 20)
  target_link_libraries(snowfall_ui_demo PRIVATE snowfall_ui)
endif()
