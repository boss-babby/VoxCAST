cmake_minimum_required(VERSION 3.25)
project(VoxCast VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VOXCAST_BUILD_TESTS "Build the unit test suite"            ON)
option(VOXCAST_BUILD_TOOLS "Build the offscreen render harness"   ON)
option(VOXCAST_GUI         "Link the app for the GUI subsystem"   ON)
set(SKIA_ROOT "" CACHE PATH
    "Root of a built Skia checkout. Empty = use the bundled mini-Skia software backend.")

# ---------------------------------------------------------------------------
# Vendored dependencies (third_party/) — no network needed at configure time.
#   nlohmann/json  single header
#   sqlite3        amalgamation
#   spdlog         header-only mode
#   mini-Skia      software backend implementing the Skia subset PopupView uses
# ---------------------------------------------------------------------------
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(vox_json INTERFACE)
target_include_directories(vox_json INTERFACE ${TP})

add_library(vox_spdlog INTERFACE)
target_include_directories(vox_spdlog INTERFACE ${TP}/spdlog_dl)
target_compile_definitions(vox_spdlog INTERFACE SPDLOG_HEADER_ONLY SPDLOG_COMPILED_LIB=0)

if(EXISTS ${TP}/sqlite/sqlite3.c)
  add_library(vox_sqlite STATIC ${TP}/sqlite/sqlite3.c)
  target_include_directories(vox_sqlite PUBLIC ${TP}/sqlite)
  target_compile_definitions(vox_sqlite PUBLIC
      SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5 SQLITE_OMIT_LOAD_EXTENSION)
endif()

# --- Graphics backend ------------------------------------------------------
add_library(vox_gfx INTERFACE)
if(SKIA_ROOT)
  message(STATUS "VoxCast: linking real Skia (GPU) from ${SKIA_ROOT}")
  target_include_directories(vox_gfx INTERFACE ${SKIA_ROOT})
  target_link_directories(vox_gfx INTERFACE ${SKIA_ROOT}/out/Release)
  target_link_libraries(vox_gfx INTERFACE skia)
  target_compile_definitions(vox_gfx INTERFACE VOXCAST_HAS_SKIA=1)
else()
  message(STATUS "VoxCast: using bundled mini-Skia software backend "
                 "(set -DSKIA_ROOT=... for the GPU path)")
  add_library(miniskia STATIC ${TP}/miniskia/MiniSkia.cpp)
  target_include_directories(miniskia PUBLIC ${TP}/miniskia)
  target_link_libraries(vox_gfx INTERFACE miniskia)
  target_compile_definitions(vox_gfx INTERFACE VOXCAST_HAS_SKIA=0)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(voxcast_core STATIC
  ai/Modes.cpp
  ai/PromptBuilder.cpp
  ai/VoiceCommands.cpp
  ui/Theme.cpp
  ui/PopupView.cpp
  ui/SkiaRenderer.cpp
  ui/MainWindow.cpp
)
target_include_directories(voxcast_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voxcast_core PUBLIC vox_gfx vox_json)

# --- Platform layer --------------------------------------------------------
if(WIN32)
  target_sources(voxcast_core PRIVATE platform/win/WinPlatform.cpp)
  target_link_libraries(voxcast_core PUBLIC
      gdi32 user32 shell32 dwmapi crypt32 ole32 oleaut32 advapi32 winhttp winmm comctl32 uxtheme gdiplus)
  target_compile_definitions(voxcast_core PUBLIC
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
  target_sources(voxcast_core PRIVATE
      platform/mac/MacPlatform.mm platform/mac/MacWindow.mm platform/mac/MacAudio.mm)
  target_link_libraries(voxcast_core PUBLIC
      "-framework Cocoa" "-framework Metal" "-framework QuartzCore"
      "-framework CoreAudio" "-framework AudioToolbox"
      "-framework ApplicationServices" "-framework Security" "-framework Carbon")
endif()

# --- Networking + AI + DB (need HTTP; optional so tests stay hermetic) -----
if(WIN32)
  target_sources(voxcast_core PRIVATE net/WinHttpClient.cpp)
elseif(NOT APPLE)
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_sources(voxcast_core PRIVATE net/CurlHttpClient.cpp)
    target_link_libraries(voxcast_core PUBLIC CURL::libcurl)
  endif()
endif()
if(TARGET vox_sqlite AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/db/HistoryStore.cpp)
  target_sources(voxcast_core PRIVATE db/HistoryStore.cpp)
  target_link_libraries(voxcast_core PUBLIC vox_sqlite)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ai/GeminiProvider.cpp AND VOXCAST_ENABLE_AI)
  target_sources(voxcast_core PRIVATE ai/GeminiProvider.cpp)
endif()
target_link_libraries(voxcast_core PUBLIC vox_spdlog)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(VoxCast app/main.cpp)
target_link_libraries(VoxCast PRIVATE voxcast_core)
if(WIN32)
  set_target_properties(VoxCast PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(VoxCast PRIVATE VOXCAST_GUI_SUBSYSTEM=1)
  target_link_options(VoxCast PRIVATE -mwindows)
endif()
if(APPLE)
  set_target_properties(VoxCast PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_GUI_IDENTIFIER com.voxcast.app
      MACOSX_BUNDLE_BUNDLE_NAME VoxCast)
endif()

# ---------------------------------------------------------------------------
# Offscreen render harness — writes PNG stills of the real PopupView
# ---------------------------------------------------------------------------
if(VOXCAST_BUILD_TOOLS)
  add_executable(render_frames tools/render_frames.cpp)
  target_link_libraries(render_frames PRIVATE voxcast_core)
endif()

# ---------------------------------------------------------------------------
# Tests — real Catch2 when available, otherwise the bundled shim.
# ---------------------------------------------------------------------------
if(VOXCAST_BUILD_TESTS)
  enable_testing()
  add_executable(voxcast_tests
      tests/catch2/catch_main.cpp
      tests/test_vad.cpp
      tests/test_spring.cpp
      tests/test_voice_commands.cpp
      tests/test_json_parsing.cpp)
  target_include_directories(voxcast_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(voxcast_tests PRIVATE voxcast_core)
  target_compile_options(voxcast_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-unused-result>)

  add_test(NAME unit COMMAND voxcast_tests)
  add_test(NAME platform_selftest COMMAND VoxCast --selftest)
endif()

message(STATUS "VoxCast ${PROJECT_VERSION} configured — tests=${VOXCAST_BUILD_TESTS} "
               "tools=${VOXCAST_BUILD_TOOLS} skia=${SKIA_ROOT}")
