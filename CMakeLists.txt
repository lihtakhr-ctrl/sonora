cmake_minimum_required(VERSION 3.16)
project(ApexRing0Client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_executable(ApexRing0Client WIN32
    main.cpp
    AutoOffsets.cpp
    Game.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(ApexRing0Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_compile_options(ApexRing0Client PRIVATE /utf-8)
target_link_libraries(ApexRing0Client PRIVATE d3d11 dxgi dwmapi winhttp)