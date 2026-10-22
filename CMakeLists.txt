cmake_minimum_required(VERSION 3.25)

project(PocketNX
    VERSION 0.0.1
    DESCRIPTION "PocketNX - native iOS Nintendo Switch emulator (route B)"
    LANGUAGES CXX C OBJCXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Core: platform-independent C++ emulation engine
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    Core/src/common/Log.cpp
    Core/src/common/Result.cpp
    Core/src/cpu/CPU.cpp
    Core/src/cpu/BlockCache.cpp
    Core/src/gpu/GPU.cpp
    Core/src/os/OS.cpp
    Core/src/fs/NRO.cpp
)

add_library(PocketNXCore STATIC ${CORE_SOURCES})
target_include_directories(PocketNXCore PUBLIC Core/src)

target_compile_features(PocketNXCore PUBLIC cxx_std_20)
target_compile_options(PocketNXCore PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:Release>:-O3>
)

# Threading (CPU translation runs on multiple threads)
find_package(Threads REQUIRED)
target_link_libraries(PocketNXCore PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Optional: Metal backend. Only build when targeting Apple platforms.
# The iOS app project links PocketNXCore + the Metal framework.
# ---------------------------------------------------------------------------
if(APPLE)
    enable_language(OBJCXX)
endif()

message(STATUS "PocketNX core configured. Target: ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
