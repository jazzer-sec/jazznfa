cmake_minimum_required(VERSION 3.20)
project(jazznfa LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(external/freetype EXCLUDE_FROM_ALL)

set(IMGUI_DIR external/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC freetype d3d11)

if(NOT DEFINED BUNDLE_DIRECTORY OR NOT EXISTS "${BUNDLE_DIRECTORY}/bundle.rc")
    message(FATAL_ERROR "Run Build.ps1 to compile and embed the NFA runtime from source.")
endif()
add_executable(loader WIN32 src/main.cpp src/gui.cpp src/nfa_client.cpp src/runtime_bundle.cpp "${BUNDLE_DIRECTORY}/bundle.rc")
target_include_directories(loader PRIVATE "${BUNDLE_DIRECTORY}")
set_target_properties(loader PROPERTIES OUTPUT_NAME JazzNfa)
target_compile_definitions(loader PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(loader PRIVATE imgui dwmapi windowscodecs ole32 crypt32 shell32 setupapi)

# All runtime files and notices are in the embedded cabinet. No adjacent assets.
