# sdcprobe — a standalone diagnostic tool.
#
# It depends on nothing but a C++20 compiler and the platform's own headers.

cmake_minimum_required(VERSION 3.25)

project(sdcprobe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Link-time optimisation must be off, explicitly.
#
# This is not belt-and-braces. The workload lives in its own translation unit
# precisely so that the call from the hot loop stays an opaque call. Let the
# linker inline it and it rebuilds the hot loop from scratch, including the
# instruction sequence under test. The tool would still look like it works and
# would quietly stop detecting anything.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(sdcprobe
    src/sdcprobe.cpp
    src/load.cpp
    src/logger.cpp
    src/platform.cpp
)

target_include_directories(sdcprobe PRIVATE src)

# Stamp the commit into the binary when building from a git checkout. Released
# source archives have no .git and build fine without it.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SDCPROBE_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SDCPROBE_GIT_HASH)
        target_compile_definitions(sdcprobe PRIVATE SDC_GIT_HASH="${SDCPROBE_GIT_HASH}")
    endif()
endif()

if(MSVC)
    # MSVC has no inline assembler for x64 at all, and does not understand the
    # GNU syntax the hot loop is written in. Failing at configure time is
    # kinder than an incomprehensible pile of compile errors.
    message(FATAL_ERROR
        "sdcprobe requires GCC or Clang: the hot loop is GNU inline assembly. "
        "On Windows, build with MinGW-w64 (MSYS2, or the 'win' cross-compile preset).")
else()
    target_compile_options(sdcprobe PRIVATE
        -O2
        # Deterministic arithmetic. The workload must be reproducible, or
        # comparing a hash built from registers against a hash of the same
        # values read from memory would mean nothing.
        -ffp-contract=off
        -fno-fast-math
        -Wall
        -Wextra
    )
endif()

install(TARGETS sdcprobe RUNTIME DESTINATION bin)
