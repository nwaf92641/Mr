#!/usr/bin/env bash
#
# Type-check the project's Objective-C on a host with no Apple SDK.
#
# This is not a build. It compiles nothing to an object file, links nothing and
# runs nothing. It asks clang one question: is this Objective-C well-formed, and
# do the backend's vtable assignments match mr_backend.h?
#
# Which is worth asking, because otherwise this code is compiled exactly nowhere
# until someone opens it on a Mac. Read tools/objc-stub-sdk/README.md for what a
# green run here does and does not mean.
#
# Each file is checked twice: once as macOS, once with TARGET_OS_OSX forced to 0
# so the iOS branches are type-checked rather than being code only a device would
# ever look at. mr_backend_metal3.m gets a third pass with OS_OBJECT_USE_OBJC
# forced to 0, because its #if on that has two branches and only one of them can
# be the one the SDK picks.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUB="$ROOT/tools/objc-stub-sdk"

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
  echo "check-objc: no clang on PATH, so there is nothing to check with." >&2
  echo "check-objc: install clang, or run this on a Mac." >&2
  exit 2
fi

# The same warning set the CMake build uses for these files, so a pass here means
# a pass there as far as the warnings go.
WARNINGS=(
  -Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wstrict-prototypes
  -Wmissing-prototypes -Wdouble-promotion -Wformat=2 -Wundef
  -Wno-unused-parameter
)

INCLUDES=(
  -I "$STUB"
  -I "$ROOT"
  -I "$ROOT/runtime/include"
  -I "$ROOT/metal"
)

FILES=(
  "$ROOT/metal/mr_backend_metal3.m"
  "$ROOT/tests/metal/mr_metal_test.m"
)

failures=0
checks=0

run_check() {
  local label="$1"
  shift
  checks=$((checks + 1))
  if "$CLANG" "${WARNINGS[@]}" "${INCLUDES[@]}" -fsyntax-only -x objective-c \
      -fno-objc-arc -fobjc-runtime=gnustep-2.0 "$@" 2>/tmp/mr-objc-check.log; then
    printf 'ok   %s\n' "$label"
  else
    printf 'FAIL %s\n' "$label"
    sed 's/^/     /' /tmp/mr-objc-check.log
    failures=$((failures + 1))
  fi
}

for f in "${FILES[@]}"; do
  rel="${f#"$ROOT"/}"
  run_check "$rel (macOS)" "$f"
  run_check "$rel (iOS)" -DTARGET_OS_OSX=0 -DTARGET_OS_IPHONE=1 "$f"
done

run_check "metal/mr_backend_metal3.m (dispatch as plain C)" \
  -DOS_OBJECT_USE_OBJC=0 "$ROOT/metal/mr_backend_metal3.m"

printf '\n%d checks, %d failed\n' "$checks" "$failures"

if [ "$failures" -ne 0 ]; then
  printf '\nThis says the Objective-C is malformed, which is a real problem.\n' >&2
  printf 'It does not say anything about behaviour: the stubs are declarations,\n' >&2
  printf 'not an SDK. The Mac is still the only place that can say that.\n' >&2
  exit 1
fi

printf '\nWell-formed as far as these stubs can tell. That is not the same as\n'
printf 'working: see tools/objc-stub-sdk/README.md for what is left unproven.\n'
