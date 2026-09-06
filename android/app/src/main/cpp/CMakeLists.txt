cmake_minimum_required(VERSION 3.22.1)
project(xstreaming_xr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenXR REQUIRED CONFIG)

add_library(
    xstreaming_xr
    SHARED
    quest_xr.cpp
)

target_compile_definitions(
    xstreaming_xr
    PRIVATE
    XR_USE_PLATFORM_ANDROID=1
    XR_USE_GRAPHICS_API_OPENGL_ES=1
)

target_compile_options(
    xstreaming_xr
    PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(
    xstreaming_xr
    OpenXR::openxr_loader
    android
    log
    EGL
    GLESv3
)
