cmake_minimum_required(VERSION 3.15)
project(yotsuiro_tl)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit & add utf-8
if(MSVC)
    set(CMAKE_GENERATOR_PLATFORM Win32)
    add_compile_options(/utf-8)
endif()

# Static runtime for portability
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Enable MASM
enable_language(ASM_MASM)

# MinHook
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
)
FetchContent_MakeAvailable(minhook)

# Discord-RPC
FetchContent_Declare(
    discord-rpc
    GIT_REPOSITORY https://github.com/TheXTech/thextech-discord-rpc.git
    GIT_TAG main
)
FetchContent_MakeAvailable(discord-rpc)

# Force static runtime on dependencies
set_property(TARGET minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET discord-rpc PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Also force-remove any /MD flags that might sneak in
if(MSVC)
    foreach(flag_var
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE)
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/MDd" "/MTd" ${flag_var} "${${flag_var}}")
    endforeach()
endif()

# Extra definitions to quiet old code
target_compile_definitions(discord-rpc PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
)

# Main DLL - winmm proxy
add_library(winmm SHARED
    dllmain.cpp
    proxy.cpp
    proxy_exports.asm
)

target_link_libraries(winmm PRIVATE minhook psapi discord-rpc)
target_include_directories(winmm PRIVATE
    ${minhook_SOURCE_DIR}/include
    ${discord-rpc_SOURCE_DIR}/include
)

# Use module definition file for exports
set_target_properties(winmm PROPERTIES
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/winmm.def"
)
