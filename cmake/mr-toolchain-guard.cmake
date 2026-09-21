# Reject configurations this release does not support, and decide which layers
# a host is allowed to build.
#
# The first release targets 64-bit ARM only (Apple Silicon, M1 and later).
# x86-64 hosts are tolerated solely so the portable core and its test suite can
# be built and run off-device; the emulation and graphics layers are excluded
# there because FEX-Emu and the Metal framework are not available.

set(MR_ARM64_PROCESSORS "arm64;aarch64;ARM64")
set(MR_CORE_ONLY_PROCESSORS "x86_64;amd64;AMD64")

function(mr_assert_supported_arch)
  if(CMAKE_SYSTEM_PROCESSOR IN_LIST MR_ARM64_PROCESSORS)
    set(MR_TARGET_ARM64 TRUE PARENT_SCOPE)
    return()
  endif()

  if(CMAKE_SYSTEM_PROCESSOR IN_LIST MR_CORE_ONLY_PROCESSORS)
    message(STATUS
      "Mr: host is ${CMAKE_SYSTEM_PROCESSOR}. Building the portable core and its "
      "tests only. The emulation and graphics layers need arm64 (Apple Silicon).")
    set(MR_TARGET_ARM64 FALSE PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR
    "Mr: unsupported processor '${CMAKE_SYSTEM_PROCESSOR}'. This release targets "
    "arm64 only. Intel Macs, Windows and non-ARM64 architectures are out of scope "
    "by design, not by omission.")
endfunction()

# Apple platform layers (Metal, Metal 4, FEX/Wine/DXMT bridges) need both an
# Apple SDK and arm64. Both conditions are checked here so a stray -DMR_BUILD_APPLE=ON
# on Linux fails at configure time instead of at link time.
function(mr_can_build_apple_layers out_var)
  if(NOT APPLE)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  if(NOT MR_TARGET_ARM64)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  set(${out_var} TRUE PARENT_SCOPE)
endfunction()
