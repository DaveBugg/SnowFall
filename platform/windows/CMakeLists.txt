# Windows 11 x64 platform layer: Win32 + D3D11/DirectComposition overlay, window tracker,
# tray/settings UI, installer and packaging. Include root: this directory.
if(NOT MSVC)
  message(FATAL_ERROR "The Windows platform layer requires MSVC (Visual Studio 2022 Build Tools).")
endif()

set(SNOW_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

set(SNOW_WIN_LIBS user32 gdi32 shell32 advapi32 ole32 dwmapi shcore comctl32 uxtheme wtsapi32 powrprof
                  d3d11 dxgi dcomp d3dcompiler dxguid d2d1 dwrite windowscodecs dbghelp)

# ---- libraries -----------------------------------------------------------
snow_module(snow_diag         "${SNOW_WIN_DIR}" diagnostics)
target_include_directories(snow_diag PUBLIC "${SNOW_CORE_DIR}")
snow_module(snow_settings_win "${SNOW_WIN_DIR}" settings)            # SettingsStore I/O, autostart
target_link_libraries(snow_settings_win PUBLIC snow_settings snow_diag)
snow_module(snow_win32        "${SNOW_WIN_DIR}" tracker)
snow_module(snow_render       "${SNOW_WIN_DIR}" rendering/d3d11)
snow_module(snow_ui           "${SNOW_WIN_DIR}" app/ui)
foreach(lib snow_win32 snow_render snow_ui)
  target_link_libraries(${lib} PUBLIC snow_sim snow_settings snow_diag)
endforeach()

# ---- application -----------------------------------------------------------
file(GLOB app_srcs CONFIGURE_DEPENDS "${SNOW_WIN_DIR}/app/*.cpp" "${SNOW_WIN_DIR}/app/*.h"
                                     "${SNOW_WIN_DIR}/app/*.rc" "${SNOW_WIN_DIR}/app/*.manifest")
add_executable(SnowFall WIN32 ${app_srcs})
target_include_directories(SnowFall PRIVATE "${SNOW_WIN_DIR}")
target_link_libraries(SnowFall PRIVATE snow_ui snow_render snow_win32 snow_sim snow_settings_win snow_settings
                                       snow_diag ${SNOW_WIN_LIBS})

# ---- tests and tools -------------------------------------------------------
function(snow_win_exe name dir)
  snow_exe(${name} "${SNOW_WIN_DIR}/${dir}" ${ARGN})
  target_include_directories(${name} PRIVATE "${SNOW_WIN_DIR}")
endfunction()
snow_win_exe(snow_settings_win_tests tests/unit/settings      snow_settings_win advapi32 shell32 ole32)
snow_win_exe(tracker_dump            tests/integration/tracker snow_win32 ${SNOW_WIN_LIBS})
snow_win_exe(render_demo             tests/integration/render  snow_render ${SNOW_WIN_LIBS})
snow_win_exe(e2e_smoke               tests/integration/e2e     snow_settings_win ${SNOW_WIN_LIBS})
snow_win_exe(ui_demo                 tests/integration/ui      snow_ui snow_sim snow_settings_win ${SNOW_WIN_LIBS})
snow_win_exe(soak_test               tests/integration/soak    snow_settings_win user32 gdi32 advapi32 shell32 ole32)
add_test(NAME settings_win COMMAND snow_settings_win_tests)

# ---- packaging -------------------------------------------------------------
# snow_art: installer .ico and MSIX logo PNGs rendered from the app's own procedural icon code.
add_executable(snow_art packaging/art/snow_art.cpp app/ui/app_icon.cpp)
target_include_directories(snow_art PRIVATE "${SNOW_WIN_DIR}" "${SNOW_CORE_DIR}")
target_link_libraries(snow_art PRIVATE user32 gdi32 ole32 windowscodecs)

# SnowFallSetup: per-user installer that embeds SnowFall.exe as RCDATA.
# -DSNOW_SETUP_PAYLOAD=<path\SnowFall.exe> embeds a prebuilt exe instead of the SnowFall target.
set(SNOW_SETUP_PAYLOAD "" CACHE FILEPATH "Prebuilt SnowFall.exe to embed (default: the SnowFall target)")
if(SNOW_SETUP_PAYLOAD)
  file(TO_CMAKE_PATH "${SNOW_SETUP_PAYLOAD}" SNOW_PKG_PAYLOAD)
else()
  set(SNOW_PKG_PAYLOAD "${CMAKE_BINARY_DIR}/SnowFall.exe")
endif()
set(SNOW_PKG_DIR "${CMAKE_BINARY_DIR}/packaging")
set(SNOW_PKG_ICO "${SNOW_PKG_DIR}/SnowFall.ico")
add_custom_command(OUTPUT "${SNOW_PKG_ICO}"
                   COMMAND "${CMAKE_COMMAND}" -E make_directory "${SNOW_PKG_DIR}"
                   COMMAND snow_art --ico "${SNOW_PKG_ICO}"
                   DEPENDS snow_art VERBATIM)
configure_file(installer/setup_payload.rc.in "${SNOW_PKG_DIR}/setup_payload.rc" @ONLY)
add_executable(SnowFallSetup WIN32 installer/setup_main.cpp installer/setup.manifest
               "${SNOW_PKG_DIR}/setup_payload.rc" "${SNOW_PKG_ICO}")
set_source_files_properties("${SNOW_PKG_DIR}/setup_payload.rc" PROPERTIES OBJECT_DEPENDS "${SNOW_PKG_PAYLOAD};${SNOW_PKG_ICO}")
if(NOT SNOW_SETUP_PAYLOAD)
  add_dependencies(SnowFallSetup SnowFall)
endif()
target_link_libraries(SnowFallSetup PRIVATE user32 gdi32 shell32 ole32 advapi32 comctl32 uuid)

# The app embeds the same generated icon (shortcuts, Explorer, Apps list).
target_sources(SnowFall PRIVATE "${SNOW_PKG_ICO}")
target_include_directories(SnowFall PRIVATE "${SNOW_PKG_DIR}")
set_source_files_properties("${SNOW_WIN_DIR}/app/app.rc" PROPERTIES OBJECT_DEPENDS "${SNOW_PKG_ICO}")
