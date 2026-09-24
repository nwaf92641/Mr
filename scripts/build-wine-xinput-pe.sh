#!/bin/bash
# Rebuild the shipped XInput PE DLLs -- app/Madeira/{aarch64,arm64ec}-windows/
# xinput1_{1,2,3,4}.dll and xinput9_1_0.dll -- from patches/wine-xinput-virtual-pad.patch.
#
# These ten binaries are committed and shipped, like the four DXMT PE modules,
# and CI never rebuilds them: the workflow restores native archives from cache
# and would happily write over a checked-in DLL with whatever an earlier run
# built. So this script is the only thing that connects the patch in patches/ to
# the binaries in the bundle, and the only place that knows the recipe.
#
# Run it after ANY change to the patch, then commit both the DLLs and the stamp
# it writes. tools/check-wine-pe-stamp.py compares that stamp against the patch
# in every release gate, so a patch edited without a rebuild fails the build
# instead of shipping -- which is how the arm64ec DXMT set once shipped stale
# (ml807). tools/validate-ios-bundle.py checks the other half: that every
# shipped module has the right machine word and carries the pad.
#
# Requires a Mac with the pinned llvm-mingw, and a wine/build-macos tree already
# configured through scripts/prepare-wine-ios.sh (with the two arm64ec patches
# this tree carries; the Makefile check below says so by name).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_SRC="$REPO_ROOT/wine"
WINE_BUILD="$WINE_SRC/build-macos"
PATCH_DIR="$REPO_ROOT/patches"
PATCH="$PATCH_DIR/wine-xinput-virtual-pad.patch"
MINGW="$REPO_ROOT/toolchains/llvm-mingw-20260421-ucrt-macos-universal"
STAMP="$REPO_ROOT/app/Madeira/.wine-pe-xinput-stamp"
JOBS="${JOBS:-3}"

# 1_1, 1_2 and 1_4 build from dlls/xinput1_3/main.c (PARENTSRC); 9_1_0 has its
# own source and forwards to xinput1_4 at runtime, so it carries no pad of its
# own and needs no patch -- it is here so the shipped set stays in step.
DLLS=(xinput1_1 xinput1_2 xinput1_3 xinput1_4 xinput9_1_0)
ARCHS=(aarch64-windows arm64ec-windows)

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

# A patch that neither applies nor is already applied is fatal: skipping it
# silently ships DLLs without the pad while the build reports success.
apply_patch() {
    local name=$1 patch="$PATCH_DIR/$1"
    if git -C "$WINE_SRC" apply --check "$patch" 2>/dev/null; then
        git -C "$WINE_SRC" apply "$patch"
        echo "Applied patches/$name"
    elif git -C "$WINE_SRC" apply --reverse --check "$patch" 2>/dev/null; then
        echo "patches/$name already applied"
    else
        echo "ERROR: patches/$name does not apply and is not already applied." >&2
        echo "       wine is at $(git -C "$WINE_SRC" rev-parse --short HEAD);" >&2
        echo "       rebase the patch onto that revision." >&2
        exit 1
    fi
}

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "ERROR: this builds PE binaries with llvm-mingw and needs an Apple Silicon Mac." >&2
    exit 1
fi
[[ -f "$WINE_SRC/configure" ]] || { echo "ERROR: initialize the wine submodule first." >&2; exit 1; }
[[ -f "$WINE_BUILD/Makefile" ]] || {
    echo "ERROR: $WINE_BUILD is not configured; run scripts/prepare-wine-ios.sh first." >&2
    exit 1
}
for prefix in aarch64 arm64ec; do
    [[ -x "$MINGW/bin/$prefix-w64-mingw32-clang" ]] || {
        echo "ERROR: llvm-mingw is missing the $prefix target; run scripts/prepare-wine-ios.sh." >&2
        exit 1
    }
done

# The arm64ec make rules only exist when Wine stops pairing aarch64+arm64ec into
# one ARM64X image, which is what patches/wine-makedep-per-arch-pe.patch does.
# Without it make dies at "No rule to make target .../arm64ec-windows/...", which
# reads like a missing file rather than a configure that never saw the patch.
for target in "dlls/xinput1_4/aarch64-windows/xinput1_4.dll" "dlls/xinput1_4/arm64ec-windows/xinput1_4.dll"; do
    grep -q "^$target:" "$WINE_BUILD/Makefile" || {
        echo "ERROR: Wine's generated Makefile has no rule for $target." >&2
        echo "       patches/wine-makedep-per-arch-pe.patch must be applied before" >&2
        echo "       scripts/prepare-wine-ios.sh configures, or every arm64ec-windows/" >&2
        echo "       output collapses into aarch64-windows/. Reconfigure and retry." >&2
        exit 1
    }
done

# wine-arm64ec-fastfail.patch first: without it the arm64ec objects do not
# compile at all (clang rejects winnt.h's x86 inline asm).
apply_patch wine-arm64ec-fastfail.patch
apply_patch wine-makedep-per-arch-pe.patch
apply_patch wine-xinput-virtual-pad.patch

targets=()
for arch in "${ARCHS[@]}"; do
    for dll in "${DLLS[@]}"; do
        targets+=("dlls/$dll/$arch/$dll.dll")
    done
done
# Rebuild unconditionally: the point of running this script is that the patch
# changed, and make cannot see that a .dll is older than a patch file.
for target in "${targets[@]}"; do rm -f "$WINE_BUILD/$target"; done
make -C "$WINE_BUILD" -j"$JOBS" "${targets[@]}"

for arch in "${ARCHS[@]}"; do
    for dll in "${DLLS[@]}"; do
        built="$WINE_BUILD/dlls/$dll/$arch/$dll.dll"
        [[ -s "$built" ]] || { echo "ERROR: build produced no $built" >&2; exit 1; }
        install -m 755 "$built" "$REPO_ROOT/app/Madeira/$arch/$dll.dll"
    done
done

# Reuse the validator's own predicates rather than a second opinion about what a
# correct artifact looks like: machine word, and the pad for the four modules
# that share dlls/xinput1_3/main.c.
python3 - "$REPO_ROOT" <<'PY'
import runpy
import sys
from pathlib import Path

validator = runpy.run_path(str(Path(sys.argv[1]) / "tools/validate-ios-bundle.py"))
root = Path(sys.argv[1]) / "app/Madeira"

for arch, machine in (("aarch64-windows", 0xAA64), ("arm64ec-windows", 0x8664)):
    for name in ("xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0"):
        data = (root / arch / f"{name}.dll").read_bytes()
        validator["pe"](data, machine)
        # xinput9_1_0 is a forwarder that LoadLibrary's xinput1_4, so it has no
        # pad of its own and no unix call to import.
        if name != "xinput9_1_0":
            validator["require"](validator["PAD_IMPORT"] in data,
                                 f"{arch}/{name}.dll has no Madeira virtual pad")
print("xinput PE DLLs: architecture and virtual pad verified")
PY

sha256_of "$PATCH" | cut -d' ' -f1 > "$STAMP"
echo "Wrote $(basename "$STAMP") = $(cat "$STAMP")"
echo "Now commit app/Madeira/{${ARCHS[*]// /,}}/*.dll (ten files) together with the stamp."
