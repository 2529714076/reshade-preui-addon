cmake_minimum_required(VERSION 3.20)
project(reshade_preui VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if (MSVC)
    # 静态 CRT，避免目标机缺少 VC 运行库
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W3 /EHsc /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_library(reshade_preui SHARED
    src/main.cpp
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hde/hde64.c
)

target_include_directories(reshade_preui PRIVATE
    third_party/reshade/include
    third_party/ngx
    third_party/minhook/include
    third_party/minhook/src
)

target_compile_definitions(reshade_preui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    # 版本号唯一来源：改上面的 project(... VERSION)，日志和 Release 产物会跟着变
    PREUI_VERSION="${PROJECT_VERSION}"
)

set_target_properties(reshade_preui PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    OUTPUT_NAME "ReShade_PreUI"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
