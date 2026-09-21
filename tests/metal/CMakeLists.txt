# The Metal backend's test.
#
# It only exists where there is an Apple SDK, a Metal compiler and a GPU to talk
# to. Everywhere else it prints why it is not being built, because a test that
# silently does not exist is indistinguishable from a test that passes.
#
# The shaders are compiled here rather than compiled from source at run time.
# That is not convenience: DXMT's shader path ends in a .metallib produced by
# Apple's toolchain, so loading a compiled library is the code path the runtime
# will use, and a test that compiled from source would be testing a path that
# does not exist in the product.

if(NOT APPLE)
  message(STATUS
    "Mr: skipping the Metal backend test. It needs Apple's Metal compiler and a "
    "GPU; on this host the portable suite in tests/c is the whole suite.")
  return()
endif()

find_program(MR_XCRUN xcrun)
if(NOT MR_XCRUN)
  message(STATUS "Mr: skipping the Metal tests (xcrun is not on PATH; so this "
                 "is not a machine with Apple's toolchain)")
  return()
endif()

# mrmetal3 is only defined when metal/mr_backend_metal3.m exists, and the Metal
# backend it builds is the thing under test. Checking before anything is created
# means a machine without the backend gets no half-built target, rather than a
# target that fails to link later.
if(NOT TARGET mrmetal3)
  message(STATUS "Mr: skipping mr_metal_test (the Metal backend target does not "
                 "exist yet)")
  return()
endif()

# The host SDK, so the same test can run on macOS CI and, later, on device with
# an iphoneos sysroot. DXMT's iOS port needed exactly this switch, and it is
# cheaper to have one variable than one edit.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(MR_METAL_SDK iphoneos)
else()
  set(MR_METAL_SDK macosx)
endif()

set(MR_METAL_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(MR_METAL_AIR ${MR_METAL_TEST_DIR}/shaders.air)
set(MR_METAL_LIB ${MR_METAL_TEST_DIR}/triangle.metallib)

add_custom_command(
  OUTPUT ${MR_METAL_AIR}
  COMMAND ${MR_XCRUN} -sdk ${MR_METAL_SDK} metal -c
          ${CMAKE_CURRENT_SOURCE_DIR}/shaders.metal -o ${MR_METAL_AIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders.metal
  COMMENT "Compiling the test shaders for the ${MR_METAL_SDK} target"
  VERBATIM)

add_custom_command(
  OUTPUT ${MR_METAL_LIB}
  COMMAND ${MR_XCRUN} -sdk ${MR_METAL_SDK} metallib ${MR_METAL_AIR}
          -o ${MR_METAL_LIB}
  DEPENDS ${MR_METAL_AIR}
  COMMENT "Linking the test shaders into a metallib"
  VERBATIM)

add_custom_target(mr_metallib DEPENDS ${MR_METAL_LIB})

add_executable(mr_metal_test mr_metal_test.m)

# The metallib is built first, so the path can be passed as an argument. Without
# the dependency the test would start before the shaders existed and fail for a
# reason that has nothing to do with the backend.
add_dependencies(mr_metal_test mr_metallib)

target_include_directories(mr_metal_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../metal
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_options(mr_metal_test PRIVATE -fno-objc-arc)

target_link_libraries(mr_metal_test PRIVATE
  # mrcore brings mrmetal3 with it: the top-level links whichever backend exists
  # into mrcore PUBLIC, because mrcore is what declares the factory that calls it.
  # Listing it here as well made ld say "ignoring duplicate libraries" on every
  # Apple build.
  mrcore mr_warnings
  "-framework Metal" "-framework Foundation" "-framework QuartzCore")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_link_libraries(mr_metal_test PRIVATE "-framework UIKit")
else()
  target_link_libraries(mr_metal_test PRIVATE "-framework AppKit")
endif()

# The metallib is built first, so the path can be passed as an argument. Without
# the dependency the test would start before the shaders existed and fail for a
# reason that has nothing to do with the backend.
add_dependencies(mr_metal_test mr_metallib)

add_test(NAME mr_metal_test
         COMMAND mr_metal_test ${MR_METAL_LIB})

# The test exits 77 when the machine cannot answer its questions -- no Metal
# device, or a paravirtual one with no Metal 3 GPU family, which is what GitHub's
# arm64 macOS runners provide. CTest reports that as skipped rather than passed,
# because "unverified" and "verified" must not look the same in a summary.
set_tests_properties(mr_metal_test PROPERTIES SKIP_RETURN_CODE 77)
