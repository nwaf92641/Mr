# The Metal 4 backend: the ABI in runtime/include/mr/mr_backend.h implemented
# natively, with no emulation in the way.
#
# This is where MTL4CommandAllocator, MTL4CommandQueue, MTL4CommandBuffer,
# MTL4ArgumentTable, MTL4Compiler and MTLResidencySet are used directly, and
# where the pieces the ABI was shaped around actually pay for themselves:
# allocators that own command memory, argument tables that replace per-bind-point
# encoder calls, explicit barriers instead of implicit hazard tracking, residency
# sets instead of useResource:, and asynchronous pipeline compilation.
#
# It is a separate target from mrmetal3 because the two are alternatives chosen
# at run time by mr_host_pick_backend, not layers of one another. A device that
# reports MTLGPUFamilyMetal4 loads the compiler twice and uses one; a device that
# does not never loads this one at all.
#
# Until mr_backend_metal4.m exists this directory is empty on purpose and the
# top-level CMakeLists says so by name.

set(MR_METAL4_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/mr_backend_metal4.m")

if(NOT EXISTS "${MR_METAL4_SOURCE}")
  message(STATUS
    "Mr: skipping the Metal 4 backend (${MR_METAL4_SOURCE} is absent).")
  return()
endif()

add_library(mrmetal4 STATIC "${MR_METAL4_SOURCE}")
target_include_directories(mrmetal4 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
target_link_libraries(mrmetal4 PUBLIC
  mrcore mr_warnings
  "-framework Metal" "-framework Foundation" "-framework QuartzCore")
