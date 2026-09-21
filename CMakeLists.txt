cmake_minimum_required(VERSION 3.20)

project(Mr
  VERSION 0.1.0
  DESCRIPTION "Apple Silicon runtime for Windows x86-64 DirectX 11 games"
  LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(mr-toolchain-guard)

mr_assert_supported_arch()
mr_can_build_apple_layers(MR_APPLE_LAYERS_DEFAULT)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MR_BUILD_TESTS "Build the test suite" ON)
option(MR_BUILD_APPLE_LAYERS
  "Build the Metal / FEX / Wine / DXMT bridge layers (Apple Silicon only)"
  ${MR_APPLE_LAYERS_DEFAULT})
option(MR_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(MR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MR_BUILD_APPLE_LAYERS AND NOT MR_APPLE_LAYERS_DEFAULT)
  message(FATAL_ERROR
    "MR_BUILD_APPLE_LAYERS=ON requires an Apple host on arm64. The Metal and "
    "emulation bridges cannot be built here.")
endif()

if(APPLE AND NOT MR_TARGET_ARM64)
  message(FATAL_ERROR "Mr: Intel Macs are out of scope for this release.")
endif()

set(MR_C_WARNINGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wwrite-strings
  -Wundef -Wpointer-arith -Wvla -Wdouble-promotion
  -Wno-unused-parameter)

# GCC 14 and Apple clang both accept these; MSVC is not a supported host.
add_library(mr_warnings INTERFACE)
target_compile_options(mr_warnings INTERFACE ${MR_C_WARNINGS})
if(MR_WARNINGS_AS_ERRORS)
  target_compile_options(mr_warnings INTERFACE -Werror)
endif()

if(MR_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Testing is enabled before the subdirectories are added. enable_testing() sets a
# directory property that applies to the subdirectories added *after* it, so
# calling it at the end of this file -- as it was -- meant every add_test() in
# tests/c was accepted and then discarded, and `ctest` reported "No tests were
# found!!!" while the suite existed and was failing.
if(MR_BUILD_TESTS)
  enable_testing()
endif()

# The Apple graphics backend is not implemented yet, and the build says so
# instead of pretending otherwise.
#
# Before this check, an Apple Silicon configure died in add_subdirectory(metal)
# with "does not contain a CMakeLists.txt", and runtime/CMakeLists.txt named two
# sources that did not exist. That is the worst of both worlds: the one platform
# this project targets could not be configured at all, and the error said nothing
# about what was missing. Now the backend's absence is a status line naming the
# file that would fill it, the rest of the Apple build proceeds, and the portable
# core -- analyzer, planner, cache, profiles -- builds and tests on a Mac today.
#
# MR_APPLE_GFX_BACKEND is what removes mr_gfx_backend_create_apple from the link,
# so the flag and the sources can never disagree about whether it exists.
set(MR_METAL3_BACKEND_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/metal/mr_backend_metal3.m")
set(MR_METAL4_BACKEND_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/metal4/mr_backend_metal4.m")

if(APPLE AND MR_TARGET_ARM64)
  if(EXISTS "${MR_METAL3_BACKEND_SOURCE}")
    set(MR_APPLE_GFX_BACKEND TRUE)
  else()
    set(MR_APPLE_GFX_BACKEND FALSE)
    message(STATUS
      "Mr: the Metal 3 backend is not implemented. ${MR_METAL3_BACKEND_SOURCE} "
      "does not exist, so nothing in this build can draw. The analyzer, the "
      "launch planner, the shader cache and the profile system are unaffected.")
  endif()
  if(NOT EXISTS "${MR_METAL4_BACKEND_SOURCE}")
    message(STATUS
      "Mr: the Metal 4 backend is not implemented. "
      "${MR_METAL4_BACKEND_SOURCE} does not exist; the runtime will report "
      "Metal 3 as the only graphics backend until it does.")
  endif()
else()
  set(MR_APPLE_GFX_BACKEND FALSE)
endif()

# The runtime tree defines the portable core, the analyzer and the CLI. The
# analyzer lives beside the runtime rather than inside it because it is the one
# component with no dependency on the runtime's state: it answers a question
# about a file and nothing else.
add_subdirectory(runtime)

# The Apple layers need the Metal SDK, FEX, Wine and DXMT, none of which are
# reachable from a non-Apple host. They are added only where they can compile, so
# a Linux build reports the absence rather than failing on an unknown header.
# Both directories exist on every host so the tree reads the same way everywhere,
# and each decides whether it has anything to build.
if(APPLE AND MR_TARGET_ARM64)
  add_subdirectory(metal)
  add_subdirectory(metal4)
endif()

# The Metal backend's test lives outside tests/c because it needs an Apple SDK
# and a GPU, neither of which the portable suite may depend on. The directory
# returns early everywhere else and says why.
add_subdirectory(tests/metal)

message(STATUS "Mr ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  host processor ..... ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  apple layers ....... ${MR_BUILD_APPLE_LAYERS}")
message(STATUS "  apple graphics ..... ${MR_APPLE_GFX_BACKEND}")
message(STATUS "  tests .............. ${MR_BUILD_TESTS}")
