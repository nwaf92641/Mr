#!/usr/bin/env bash
#
# Fetches the sources Mr builds against: FEX-Emu, Wine and DXMT.
#
# These are not vendored in the repository, and they are not submodules. Two of
# them are large, all three are developed on their own schedules, and a game
# runtime pinned to a stale Wine is worse than one that says which Wine it built
# against. So the rule here is simple: this script fetches sources into
# third_party/src/, which .gitignore already excludes, and it never invents a
# version. Pass one, or take the upstream default branch and be told that is what
# happened.
#
# What this script does NOT do is build them. FEX-Emu needs Rust and CMake, Wine
# needs its own toolchain, and DXMT's Metal side needs Xcode on a Mac. Each one
# prints its own remaining steps at the end rather than failing halfway through a
# build the caller cannot see.
#
# Usage:
#   tools/fetch-components.sh                 # default branch of each, then report
#   FEX_REF=<ref> WINE_REF=<ref> DXMT_REF=<ref> tools/fetch-components.sh
#
# Environment:
#   MR_ROOT=<dir>    where the tree is; defaults to the script's parent

set -u

MR_ROOT="${MR_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC_DIR="${MR_ROOT}/third_party/src"

FEX_URL="${FEX_URL:-https://github.com/FEX-Emu/FEX.git}"
WINE_URL="${WINE_URL:-https://gitlab.winehq.org/wine/wine.git}"
DXMT_URL="${DXMT_URL:-https://github.com/3Shain/dxmt.git}"

FEX_REF="${FEX_REF:-}"
WINE_REF="${WINE_REF:-}"
DXMT_REF="${DXMT_REF:-}"

die() {
  printf 'fetch-components: %s\n' "$*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not on PATH."
}

need git
mkdir -p "${SRC_DIR}" || die "cannot create ${SRC_DIR}"

# Clones, or updates the checkout if it is already there.
#
# An existing directory is fetched rather than re-cloned, and a ref that was
# asked for is checked out explicitly, so running this twice is the normal way to
# pick up a new ref rather than an error to work around.
fetch() {
  local name="$1" url="$2" ref="$3" dir="${SRC_DIR}/$1"

  if [ -d "${dir}/.git" ]; then
    printf '==> %s: updating %s\n' "${name}" "${dir}"
    if [ -n "${ref}" ]; then
      git -C "${dir}" fetch --depth 1 origin "${ref}" ||
        die "${name}: fetch failed"
    else
      git -C "${dir}" fetch --depth 1 origin || die "${name}: fetch failed"
    fi
  else
    printf '==> %s: cloning %s\n' "${name}" "${url}"
    if [ -n "${ref}" ]; then
      git clone --depth 1 --branch "${ref}" "${url}" "${dir}" ||
        die "${name}: clone of ${ref} failed (does that ref exist?)"
    else
      git clone --depth 1 "${url}" "${dir}" || die "${name}: clone failed"
    fi
  fi

  if [ -n "${ref}" ]; then
    git -C "${dir}" checkout --detach FETCH_HEAD >/dev/null 2>&1 ||
      git -C "${dir}" checkout "${ref}" >/dev/null 2>&1 ||
      die "${name}: could not check out ${ref}"
  fi

  # The revision actually obtained is recorded, not the one requested. Which
  # commit a branch pointed at when this ran is the only thing that explains a
  # later failure, and it is not recoverable from the branch name.
  printf '    %s at %s\n' "${name}" \
    "$(git -C "${dir}" rev-parse --short HEAD)"
}

fetch fex "${FEX_URL}" "${FEX_REF}"
fetch wine "${WINE_URL}" "${WINE_REF}"
fetch dxmt "${DXMT_URL}" "${DXMT_REF}"

cat <<'EOF'

Fetched. What remains is deliberately not automated, because each step needs a
toolchain this script cannot assume and a failure in the middle is worse than a
clear ending.

FEX-Emu  (MIT)        Rust + CMake. The ARM64 host build is what Mr links;
                      the JIT it produces is the x86-64 -> ARM64 translator.
Wine     (LGPL-2.1+)  Configure with --enable-archs=arm64ec. ARM64EC is what
                      lets the guest's x86-64 imports meet native ARM64 code.
DXMT     (MIT)        Needs Xcode, because its Metal shader sources are compiled
                      to a .metallib by Apple's toolchain. This step cannot be
                      done off a Mac.

Licences differ and matter: Wine is LGPL, so it stays a separate library that Mr
links against rather than code that gets merged into this tree.
EOF
