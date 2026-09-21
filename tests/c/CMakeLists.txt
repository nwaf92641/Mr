# The host test suite.
#
# These tests run on any machine with a C11 compiler and cover the layers that
# make decisions: PE parsing, analysis, profiles, planning, caching, telemetry and
# the compatibility database. They do not cover Metal, FEX, Wine or DXMT -- those
# need a device, and pretending otherwise by stubbing them would produce a green
# suite that proves nothing about the thing being built.
#
# `ctest` is not used as a wrapper: the suite is one binary with its own summary,
# because a failing layer needs to be identifiable from the output and a wrapper
# would only add a layer to look through.

add_executable(mr_tests
  test_mr.c
  mr_fixture.c
)
target_link_libraries(mr_tests PRIVATE mranalyzer mrcore mr_warnings)

# ctest runs the same binary the developer runs by hand, through an explicit
# generator expression rather than the bare target name. A plain name is only
# substituted when CMake resolves it to a target in this directory, and when it
# does not, ctest is left looking for a file that was never produced -- "Unable to
# find executable" is what that looks like from the outside.
add_test(NAME mr_tests COMMAND $<TARGET_FILE:mr_tests>)
set_tests_properties(mr_tests PROPERTIES TIMEOUT 120)
