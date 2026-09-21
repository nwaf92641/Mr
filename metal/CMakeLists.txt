# The Metal 3 backend: the ABI in runtime/include/mr/mr_backend.h, implemented by
# emulation on MTLDevice, MTLCommandQueue and per-encoder binding calls.
#
# The header explains why the ABI is shaped like Metal 4 and why the Metal 3
# backend is the emulation rather than the other way round. What belongs here is
# that emulation: an allocator becomes a ring of command buffers, an argument
# table becomes the binding calls the encoders already take, a residency set
# becomes useResource:/useHeap:, and an explicit barrier becomes MTLBarrierScope
# or a split pass.
#
# Until mr_backend_metal3.m exists this directory is empty on purpose, and the
# top-level CMakeLists reports that as a status line naming the file. A library
# with no sources would link and then fail at the first draw, which is a worse
# answer than a build that says what is missing.

set(MR_METAL3_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/mr_backend_metal3.m")

if(NOT EXISTS "${MR_METAL3_SOURCE}")
  message(STATUS
    "Mr: skipping the Metal 3 backend (${MR_METAL3_SOURCE} is absent).")
  return()
endif()

add_library(mrmetal3 STATIC "${MR_METAL3_SOURCE}")
target_include_directories(mrmetal3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
target_link_libraries(mrmetal3 PUBLIC
  mrcore mr_warnings
  "-framework Metal" "-framework Foundation" "-framework QuartzCore")
