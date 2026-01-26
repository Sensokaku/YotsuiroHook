cmake_minimum_required(VERSION 3.15)
project(yotsuiro_tl)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force32-bit
if(MSVC)
    set(CMAKE_GENERATOR_PLATFORM Win32)
endif()

# MinHook
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
)
FetchContent_MakeAvailable(minhook)

# Static runtime for portability (must be set before adding targets that depend on it)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Main DLL
add_library(dinput8 SHARED dllmain.cpp)
target_link_libraries(dinput8 PRIVATE minhook psapi)

# Also force static runtime on minhook
set_property(TARGET minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
