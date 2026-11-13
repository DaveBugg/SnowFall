cmake_minimum_required(VERSION 3.24)

# macOS 13+. Must be set before project(). The Xcode generator builds the universal binary
# (arm64 + x86_64, what CI ships); Ninja builds the host architecture only, because CMake
# cannot build multi-architecture Swift (platform/macos/ui) with Ninja.
if(CMAKE_HOST_APPLE OR EXISTS "/System/Library/CoreServices/SystemVersion.plist")
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
  endif()
  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES AND CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS target architectures")
  endif()
endif()

project(SnowFall VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All executables land in the build root (scripts and tests expect build/SnowFall.exe).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MSVC)
  # Static CRT so the portable .exe has no VC redist dependency.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /utf-8 /permissive- /EHsc /MP)
else()
  add_compile_options(-Wall -Wextra)
endif()
# Version string shown in the app and the log: "0.1.1" for a build of the release tag
# (v0.1.1 at HEAD, or a tag build in CI), otherwise "0.1.1+<commit>" so a test build is never
# mistaken for the release. File names and bundle/installer version numbers stay numeric.
set(SNOW_VERSION_STRING "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git" AND NOT "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
  execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _snow_tags
                  RESULT_VARIABLE _snow_r1 ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _snow_hash
                  RESULT_VARIABLE _snow_r2 ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "\n" ";" _snow_tags "${_snow_tags}")
  if(_snow_r2 EQUAL 0 AND _snow_hash AND NOT "v${PROJECT_VERSION}" IN_LIST _snow_tags)
    set(SNOW_VERSION_STRING "${PROJECT_VERSION}+${_snow_hash}")
  endif()
endif()
message(STATUS "SnowFall version string: ${SNOW_VERSION_STRING}")
add_compile_definitions(SNOW_VERSION="${SNOW_VERSION_STRING}")

# Helper: one static library per module, sources picked up by glob.
#   snow_module(<name> <root> <dir>...)  — dirs are relative to <root>; <root> is the include root.
function(snow_module name root)
  set(srcs "")
  foreach(d ${ARGN})
    file(GLOB_RECURSE s CONFIGURE_DEPENDS "${root}/${d}/*.cpp" "${root}/${d}/*.h")
    list(APPEND srcs ${s})
  endforeach()
  add_library(${name} STATIC ${srcs})
  target_include_directories(${name} PUBLIC "${root}")
endfunction()

# Helper: an executable from every source in one directory.
function(snow_exe name dir)
  file(GLOB_RECURSE srcs CONFIGURE_DEPENDS "${dir}/*.cpp" "${dir}/*.h" "${dir}/*.rc")
  add_executable(${name} ${srcs})
  target_include_directories(${name} PRIVATE "${SNOW_CORE_DIR}" "${SNOW_CORE_DIR}/tests")
  target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

set(SNOW_CORE_DIR "${CMAKE_SOURCE_DIR}/core")
enable_testing()

# The macOS app brings its own logger backend (file + stderr) instead of core's stderr one.
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/platform/macos/CMakeLists.txt")
  set(SNOW_PLATFORM_DIAG ON)
endif()

# Portable core: simulation, surfaces, tools, settings model/JSON. Builds on any OS.
add_subdirectory(core)

# Platform layer.
if(WIN32)
  add_subdirectory(platform/windows)
elseif(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/platform/macos/CMakeLists.txt")
  add_subdirectory(platform/macos)
elseif(APPLE)
  message(STATUS "SnowFall: macOS app not present yet - building the portable core and its tests only.")
endif()
