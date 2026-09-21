# The portable core.
#
# Everything in this target builds and runs on any host with a C11 compiler, and
# that is a requirement rather than a convenience: it holds the parts of the
# runtime that make decisions -- analysis, planning, caching, compatibility --
# and those are the parts worth testing off-device. Nothing here may depend on
# Metal, FEX, Wine or DXMT headers.

set(MR_CORE_SOURCES
  core/mr_types.c
  core/mr_util.c
  core/mr_host.c
  core/mr_profile.c
  core/mr_launch.c
  core/mr_cache.c
  core/mr_telemetry.c
  core/mr_compat.c
  core/mr_backend.c
)

add_library(mrcore STATIC ${MR_CORE_SOURCES})
target_include_directories(mrcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(mrcore PUBLIC mr_warnings)

# The host probe is the one piece of the core that has to ask the operating
# system questions, so it is selected rather than #ifdef'd inside the core.
#
# The Metal capability probe is Objective-C -- MTLDevice is an Objective-C
# protocol -- so OBJC has to be enabled before any target lists a .m source;
# CMake determines a target's languages when the target is created, and enabling
# the language afterwards leaves the .m file without a compiler.
if(APPLE)
  enable_language(OBJC)
endif()

if(APPLE)
  target_sources(mrcore PRIVATE
    platform/apple/mr_host_apple.c
    platform/apple/mr_metal_probe.m
  )
  # PUBLIC because the probe's Foundation and Metal calls are unresolved symbols
  # in a static library: whoever links mrcore has to bring the frameworks.
  target_link_libraries(mrcore PUBLIC "-framework Foundation" "-framework Metal")
else()
  target_sources(mrcore PRIVATE platform/posix/mr_host_posix.c)
endif()

# The analyzer is part of the runtime, not a separate tool: the on-device import
# flow calls it directly through this library. It is pure C with no dependency on
# the core library's mutable state, which is why the app can link it without
# pulling anything else in.
add_library(mranalyzer STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../analyzer/mr_pe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../analyzer/mr_analyze.c
)
target_include_directories(mranalyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mranalyzer PUBLIC mrcore mr_warnings)

# The command-line front end. On Apple platforms the app links mrcore directly and
# this exists for diagnostics plus the host test suite.
add_executable(mrctl ${CMAKE_CURRENT_SOURCE_DIR}/../tools/mrctl.c)
target_link_libraries(mrctl PRIVATE mranalyzer mrcore mr_warnings)

# The Metal backends live in metal/ and metal4/ rather than here. They need the
# Apple SDK, they are alternatives chosen at run time rather than layers, and
# keeping them out of this file is what lets a non-Apple host configure the tree
# without ever seeing a Metal header.
#
# What this file does have to say is whether they exist, because that decides
# whether mr_gfx_backend_create_apple is a symbol anything can reference. The
# top-level CMakeLists sets MR_APPLE_GFX_BACKEND from the same source paths the
# subdirectories use, and the define below is what keeps mr_backend.c's Apple
# branch out of the link when there is nothing behind it.
if(APPLE AND MR_TARGET_ARM64 AND NOT MR_APPLE_GFX_BACKEND)
  target_compile_definitions(mrcore PUBLIC MR_NO_APPLE_GFX_BACKEND)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests/c mr_tests)
