cmake_minimum_required(VERSION 3.25)

project(PocketNX
    VERSION 0.0.1
    DESCRIPTION "PocketNX - native iOS Nintendo Switch emulator (route B)"
    LANGUAGES CXX C
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
    Core/src/os/Services.cpp
    Core/src/os/SvcTable.cpp
    Core/src/fs/NRO.cpp
    Core/src/cpu/Loader.cpp
    Core/src/cpu/GuestThread.cpp
    Core/src/cpu/ProgramRewriter.cpp
    Core/src/cpu/SyscallTrap.cpp
    Core/src/runtime/Runtime.cpp
)

# AArch64 trampoline for direct guest execution.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    enable_language(ASM)
    list(APPEND CORE_SOURCES Core/src/cpu/GuestThreadTrampoline.S)
endif()

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
# Tools: pnx-nro-inspect — standalone NRO/NSO/NXO inspector (dev/diagnostic)
# ---------------------------------------------------------------------------
add_executable(pnx-nro-inspect tools/pnx-nro-inspect.cpp)
target_link_libraries(pnx-nro-inspect PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-loader-test — self-contained NRO parse + load + relocation test
# ---------------------------------------------------------------------------
add_executable(pnx-loader-test tools/pnx-loader-test.cpp)
target_link_libraries(pnx-loader-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-guest-thread-test — stack/TLS + direct-exec trampoline smoke test
# ---------------------------------------------------------------------------
add_executable(pnx-guest-thread-test tools/pnx-guest-thread-test.cpp)
target_link_libraries(pnx-guest-thread-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-program-rewriter-test — SVC -> BRK rewriting smoke test
# ---------------------------------------------------------------------------
add_executable(pnx-program-rewriter-test tools/pnx-program-rewriter-test.cpp)
target_link_libraries(pnx-program-rewriter-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-syscall-trap-test — BRK/SIGTRAP trap + dispatch smoke test
# ---------------------------------------------------------------------------
add_executable(pnx-syscall-trap-test tools/pnx-syscall-trap-test.cpp)
target_link_libraries(pnx-syscall-trap-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-os-dispatch-test — OS command-dispatch table + service stubs test
# ---------------------------------------------------------------------------
add_executable(pnx-os-dispatch-test tools/pnx-os-dispatch-test.cpp)
target_link_libraries(pnx-os-dispatch-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Tools: pnx-runtime-test — full direct-execution pipeline (load -> rewrite ->
# mprotect -> trap -> OS) e2e test. Optionally runs a real NRO as argv[1].
# ---------------------------------------------------------------------------
add_executable(pnx-runtime-test tools/pnx-runtime-test.cpp)
target_link_libraries(pnx-runtime-test PRIVATE PocketNXCore)

# ---------------------------------------------------------------------------
# Optional: Metal backend. Only build when targeting Apple platforms.
# The iOS app project links PocketNXCore + the Metal framework.
# ---------------------------------------------------------------------------
if(APPLE)
    enable_language(OBJCXX)
endif()

message(STATUS "PocketNX core configured. Target: ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
