# macOS platform layer (macOS 13+, universal arm64 + x86_64): Objective-C++ app shell,
# Metal overlay renderer, CGWindowList tracker, menu-bar item. Include roots: this directory
# (platform/macos) and core/. Added by the root CMakeLists.txt when building on Apple.
#
# Targets
#   snow_diag        logger backend (file + stderr) — replaces core's log_portable.cpp here
#   snow_mac_core    pure C++ parts (tracker model, hotkey map, HLSL->MSL, drift mesh, icon art)
#   snow_mac_tests   unit tests of snow_mac_core                       (ctest: mac_unit)
#   SnowFall         SnowFall.app (LSUIElement, ad-hoc signed)          (ctest: mac_selftest)
#   make_dmg         SnowFall-<version>.dmg (part of ALL unless -DSNOW_MAC_DMG=OFF)
enable_language(OBJC OBJCXX)

set(SNOW_MAC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
option(SNOW_MAC_DMG "Build the .dmg as part of the default target" ON)

# Objective-C(++) with ARC everywhere; C++20 for ObjC++ as well.
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
add_compile_options($<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)

# ---- logger backend ----------------------------------------------------------------------
add_library(snow_diag STATIC diagnostics/log_mac.cpp "${SNOW_CORE_DIR}/diagnostics/log.h")
target_include_directories(snow_diag PUBLIC "${SNOW_CORE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(snow_diag PUBLIC Threads::Threads)

# ---- pure C++ ----------------------------------------------------------------------------
# drift_mesh.cpp is the portable tessellator of the Windows renderer, shared as-is.
set(SNOW_DRIFT_MESH_SRC "${CMAKE_SOURCE_DIR}/platform/windows/rendering/d3d11/drift_mesh.cpp")
add_library(snow_mac_core STATIC
  tracker/desktop_model.cpp tracker/desktop_model.h
  system/hotkey_map.cpp system/hotkey_map.h
  system/text_util.cpp system/text_util.h
  system/settings_store_mac.cpp
  render/msl_translate.cpp render/msl_translate.h
  app/icon_render.cpp app/icon_render.h
  "${SNOW_DRIFT_MESH_SRC}")
set_source_files_properties("${SNOW_DRIFT_MESH_SRC}" PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/windows")
target_include_directories(snow_mac_core PUBLIC "${SNOW_MAC_DIR}" "${SNOW_CORE_DIR}")
target_link_libraries(snow_mac_core PUBLIC snow_sim snow_settings snow_diag)

# ---- unit tests ----------------------------------------------------------------------------
add_executable(snow_mac_tests tests/mac_unit_tests.cpp)
target_include_directories(snow_mac_tests PRIVATE "${SNOW_CORE_DIR}/tests")
target_link_libraries(snow_mac_tests PRIVATE snow_mac_core "-framework Carbon")
add_test(NAME mac_unit COMMAND snow_mac_tests)

# ---- app icon (rendered at build time) ------------------------------------------------------
add_executable(snow_icon_gen tools/icon_gen.mm app/icon_render.cpp)
target_include_directories(snow_icon_gen PRIVATE "${SNOW_MAC_DIR}")
target_link_libraries(snow_icon_gen PRIVATE "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO")
set(SNOW_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
set(SNOW_ICNS "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
add_custom_command(OUTPUT "${SNOW_ICNS}"
                   COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SNOW_ICONSET}"
                   COMMAND snow_icon_gen "${SNOW_ICONSET}"
                   COMMAND iconutil -c icns -o "${SNOW_ICNS}" "${SNOW_ICONSET}"
                   DEPENDS snow_icon_gen
                   COMMENT "Rendering AppIcon.icns"
                   VERBATIM)
set_source_files_properties("${SNOW_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources GENERATED TRUE)

# ---- application -----------------------------------------------------------------------------
file(GLOB snow_mac_app_srcs CONFIGURE_DEPENDS
     "${SNOW_MAC_DIR}/app/*.mm" "${SNOW_MAC_DIR}/app/*.h"
     "${SNOW_MAC_DIR}/overlay/*.mm" "${SNOW_MAC_DIR}/overlay/*.h"
     "${SNOW_MAC_DIR}/render/*.mm" "${SNOW_MAC_DIR}/render/*.h"
     "${SNOW_MAC_DIR}/tracker/*.mm"
     "${SNOW_MAC_DIR}/system/*.mm" "${SNOW_MAC_DIR}/system/*.h"
     "${SNOW_MAC_DIR}/bridge/*.h")
configure_file(packaging/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
add_executable(SnowFall MACOSX_BUNDLE ${snow_mac_app_srcs} "${SNOW_ICNS}")
set_target_properties(SnowFall PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.davebugg.snowfall")
target_include_directories(SnowFall PRIVATE "${SNOW_MAC_DIR}" "${SNOW_MAC_DIR}/bridge")
target_link_libraries(SnowFall PRIVATE snow_mac_core snow_sim snow_settings snow_diag
  "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
  "-framework CoreGraphics" "-framework IOKit" "-framework Carbon" "-framework ServiceManagement"
  "-framework ApplicationServices")

# SwiftUI settings window + quick panel (platform/macos/ui, owned by the UI work stream).
# The shell finds it at run time with NSClassFromString(@"SFUIFactory"); -ObjC makes the
# linker keep the Objective-C-visible Swift classes of a static library.
if(EXISTS "${SNOW_MAC_DIR}/ui/CMakeLists.txt")
  add_subdirectory(ui)
  if(TARGET snowfall_ui)
    target_link_libraries(SnowFall PRIVATE snowfall_ui)
    target_link_options(SnowFall PRIVATE "-ObjC")
    message(STATUS "SnowFall: SwiftUI module linked")
  endif()
endif()

# Ad-hoc signature over the finished bundle (no Developer ID; see platform/macos/README.md).
add_custom_command(TARGET SnowFall POST_BUILD
                   COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:SnowFall>"
                   COMMENT "Ad-hoc signing SnowFall.app"
                   VERBATIM)

# Self-test of the real app binary (Metal, windows, tracker, threads).
add_test(NAME mac_selftest COMMAND "$<TARGET_FILE:SnowFall>" --selftest)
set_tests_properties(mac_selftest PROPERTIES TIMEOUT 180)

# ---- disk image -------------------------------------------------------------------------------
set(SNOW_DMG "${CMAKE_BINARY_DIR}/SnowFall-${PROJECT_VERSION}.dmg")
add_custom_command(OUTPUT "${SNOW_DMG}"
                   COMMAND sh "${SNOW_MAC_DIR}/packaging/make_dmg.sh" "$<TARGET_BUNDLE_DIR:SnowFall>" "${SNOW_DMG}"
                   DEPENDS SnowFall "${SNOW_MAC_DIR}/packaging/make_dmg.sh"
                   COMMENT "Building SnowFall-${PROJECT_VERSION}.dmg"
                   VERBATIM)
if(SNOW_MAC_DMG)
  add_custom_target(make_dmg ALL DEPENDS "${SNOW_DMG}")
else()
  add_custom_target(make_dmg DEPENDS "${SNOW_DMG}")
endif()
