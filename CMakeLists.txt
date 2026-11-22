cmake_minimum_required(VERSION 3.21)
project(PLAYERSLICE VERSION 0.7.1 LANGUAGES C CXX)

# PLAYERSLICE — player de vídeo para ecrãs fora de formato (slices)
# Criado por Daniel Nunes — github.com/NunesDanielPT

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
  message(FATAL_ERROR "PLAYERSLICE é uma aplicação Windows (Direct3D 11).")
endif()

# ---------------------------------------------------------------------------
# FFmpeg (pré-compilado). Indica a pasta com include/ lib/ bin/
# ---------------------------------------------------------------------------
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "Pasta do FFmpeg (include/lib/bin)")
if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR "FFmpeg não encontrado em ${FFMPEG_DIR}. Corre scripts/get-deps.ps1 ou define -DFFMPEG_DIR=...")
endif()

# ---------------------------------------------------------------------------
# Dependências de código (FetchContent; podem ser substituídas por pastas locais
# com -DFETCHCONTENT_SOURCE_DIR_IMGUI=... etc.)
# ---------------------------------------------------------------------------
include(FetchContent)
# CMake 4 recusa projetos com cmake_minimum_required antigo (snappy)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(imgui  GIT_REPOSITORY https://github.com/ocornut/imgui.git  GIT_TAG 33d9ed0d6bdeaf2fdd1636b72d68c268e851033f)
FetchContent_Declare(hap    GIT_REPOSITORY https://github.com/Vidvox/hap.git     GIT_TAG d847f6bbd3be88575dd4ef33a877243780e3be76)
FetchContent_Declare(snappy GIT_REPOSITORY https://github.com/google/snappy.git  GIT_TAG 1.2.1   GIT_SHALLOW TRUE)
FetchContent_Declare(json   GIT_REPOSITORY https://github.com/nlohmann/json.git  GIT_TAG v3.11.3 GIT_SHALLOW TRUE)

set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE INTERNAL "")

# imgui e hap não têm CMakeLists: MakeAvailable só descarrega
FetchContent_MakeAvailable(snappy json imgui hap)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

add_library(hap STATIC ${hap_SOURCE_DIR}/source/hap.c)
target_include_directories(hap PUBLIC ${hap_SOURCE_DIR}/source)
target_link_libraries(hap PRIVATE snappy)

# ---------------------------------------------------------------------------
# Aplicação
# ---------------------------------------------------------------------------
add_executable(PLAYERSLICE WIN32
  src/main.cpp
  src/app/App.cpp
  src/core/Log.cpp
  src/core/Model.cpp
  src/core/CrashHandler.cpp
  src/core/ThreadPool.cpp
  src/core/I18n.cpp
  src/core/Effects.cpp
  src/core/Osc.cpp
  src/core/Timecode.cpp
  src/core/WebServer.cpp
  src/core/Settings.cpp
  src/core/Updater.cpp
  src/gfx/Gfx.cpp
  src/gfx/Displays.cpp
  src/gfx/ImageLoader.cpp
  src/video/VideoPlayer.cpp
  src/video/Thumbnail.cpp
  src/render/Compositor.cpp
  src/render/OutputWindows.cpp
  src/ui/UI.cpp
  src/ui/ClipGrid.cpp
  src/ui/SliceEditor.cpp
  src/ui/Theme.cpp
  src/ui/SettingsWindow.cpp
  src/ui/Properties.cpp
  src/ui/Layout.cpp
  src/ui/ClipTime.cpp
  src/ui/TimecodeUI.cpp
  src/ui/CuesUI.cpp
  res/app.rc)

target_include_directories(PLAYERSLICE PRIVATE src ${FFMPEG_DIR}/include)
target_compile_definitions(PLAYERSLICE PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
  PLAYERSLICE_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(PLAYERSLICE PRIVATE /W3 /utf-8 /MP)
  target_link_directories(PLAYERSLICE PRIVATE ${FFMPEG_DIR}/lib)
  target_link_libraries(PLAYERSLICE PRIVATE avformat avcodec avutil swscale)
else()
  target_compile_options(PLAYERSLICE PRIVATE -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers -municode)
  target_link_options(PLAYERSLICE PRIVATE -municode)
  target_link_libraries(PLAYERSLICE PRIVATE
    ${FFMPEG_DIR}/lib/libavformat.dll.a ${FFMPEG_DIR}/lib/libavcodec.dll.a
    ${FFMPEG_DIR}/lib/libavutil.dll.a ${FFMPEG_DIR}/lib/libswscale.dll.a
    -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic)
endif()

target_link_libraries(PLAYERSLICE PRIVATE
  imgui hap nlohmann_json::nlohmann_json
  d3d11 dxgi d3dcompiler dbghelp shell32 winmm ole32 dwmapi shcore comdlg32 windowscodecs winhttp iphlpapi ws2_32 uuid)

# Copiar DLLs do FFmpeg para junto do .exe
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET PLAYERSLICE POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:PLAYERSLICE>)

install(TARGETS PLAYERSLICE RUNTIME DESTINATION .)
install(FILES ${FFMPEG_DLLS} DESTINATION .)
