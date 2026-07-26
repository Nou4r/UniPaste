cmake_minimum_required(VERSION 3.20)

# Set policy CMP0091 to NEW before project() for MSVC_RUNTIME_LIBRARY property support
cmake_policy(SET CMP0091 NEW)

project(UniPaste LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(UniPaste WIN32
    src/main.cpp
    src/spoof.cpp
    src/overlay.cpp
    src/wordlist.cpp
    src/links.cpp
    src/policy.cpp
    src/settings.cpp
    src/theme.cpp
    src/appicon.cpp
)

target_compile_definitions(UniPaste PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

target_compile_options(UniPaste PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
)

target_link_libraries(UniPaste PRIVATE
    user32
    gdi32
    shell32
    advapi32
    comctl32
    uxtheme
    dwmapi
)

set_property(TARGET UniPaste PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
