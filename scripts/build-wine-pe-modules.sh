#!/bin/bash
# Build the Wine PE modules named in tools/pe-module-manifest.txt into
# app/Madeira/{arm64ec,aarch64}-windows/.
#
# Why this exists: those two directories hold about 120 DLLs each, and the set
# was never chosen for compatibility -- it is the imports of Wine's own test
# executables (fourteen of the files in them are test exes) plus what DXMT needs.
# A title that imports anything else gets a failed LoadLibrary and no game. The
# modules here are the ones a DirectX-era title actually imports, taken from
# Winlator's components (see the manifest header).
#
# The recipe is per-module, not `make all`, and it is the same one
# scripts/build-wine-xinput-pe.sh uses: `dlls/<module>/<arch>/<module>.dll` is a
# rule Wine's makedep generates, and the arm64ec rules only exist with
# patches/wine-makedep-per-arch-pe.patch applied before configure.
#
# The four typelib modules are built first and for both architectures. widl
# resolves an `importlib` through tools/tools.h:get_arch_dir(), which maps
# CPU_ARM64EC onto "aarch64-windows" -- correct upstream, where the two
# architectures share one ARM64X image, but not in this tree, where
# patches/wine-makedep-per-arch-pe.patch splits them into two directories.
# So an arm64ec build of quartz asks for dlls/stdole2.tlb/aarch64-windows/
# stdole2.tlb, and the dependency makedep recorded is the arm64ec one, which
# does not help. The build then fails with "cannot find stdole2.tlb" on a
# module that is not in the requested set, which reads like a broken checkout.
# Building the typelibs up front for both architectures satisfies the lookup.
#
# Two things it deliberately does NOT do:
#
#   * It never overwrites a module that is already in the bundle unless --force
#     is given. The committed DLLs are known-good and were built against a
#     revision this script cannot prove it shares; the point is to close the gap,
#     not to replace the set that works.
#   * It never touches the modules the app owns: d3d11, dxgi, d3d10core and
#     winemetal come from DXMT, and xinput1_1/1_2/1_3/1_4/9_1_0 come from
#     patches/wine-xinput-virtual-pad.patch via build-wine-xinput-pe.sh. Wine's
#     own copies of those would load, run and silently drop the Metal backend and
#     the virtual pad, so they are refused rather than built.
#
# Usage:
#   scripts/build-wine-pe-modules.sh [--force] [--arch aarch64-windows|arm64ec-windows] [--dry-run]
#
# Environment:
#   PE_MODULES_OUT  install directory (default app/Madeira). The IPA workflow sets
#                   it to a cached staging directory and copies from there, so a
#                   run that only needs the modules does not rebuild them.
#
# The modules are PE/COFF, but they are produced by Wine's build tree, which is a
# macOS configure with llvm-mingw cross compilers, so this needs that tree and
# toolchain rather than a Mac as such.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_SRC="$REPO_ROOT/wine"
WINE_BUILD="$WINE_SRC/build-macos"
# Where the modules are installed. The IPA workflow points this at a directory it
# caches, then copies from there into the bundle, because a Wine tree configure is
# ~40 minutes and rebuilding 143 modules on every run is ~35 more; a module set is
# only valid for the Wine revision it was built against, so it keys on that.
BUNDLE="${PE_MODULES_OUT:-$REPO_ROOT/app/Madeira}"
MANIFEST="$REPO_ROOT/tools/pe-module-manifest.txt"
PATCH_DIR="$REPO_ROOT/patches"
MINGW="$REPO_ROOT/toolchains/llvm-mingw-20260421-ucrt-macos-universal"
ARCHS=(arm64ec-windows aarch64-windows)
JOBS="${JOBS:-3}"

FORCE=0
DRY_RUN=0
WANTED_ARCHS=("${ARCHS[@]}")

while [ $# -gt 0 ]; do
    case "$1" in
        --force)   FORCE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --arch)    WANTED_ARCHS=("$2"); shift 2 ;;
        -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Modules whose implementation is not Wine's. Building Wine's copy over one of
# these is a functional regression that no test in this repo would catch.
PROTECTED=(d3d11 dxgi d3d10core winemetal xinput1_1 xinput1_2 xinput1_3 xinput1_4 xinput9_1_0)

is_protected() {
    local candidate="$1" protected
    for protected in "${PROTECTED[@]}"; do
        [[ "$candidate" == "$protected" ]] && return 0
    done
    return 1
}

# Modules the tree does not have at all would be a manifest typo or a Wine
# revision that moved them, and either way `make` reports it as "No rule to make
# target", which reads like a missing file. Check the generated Makefile instead.
has_rule() {
    grep -q "^$1:" "$WINE_BUILD/Makefile"
}

apply_patch() {
    local name="$1" patch="$PATCH_DIR/$1"
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

if [[ ! -f "$WINE_SRC/configure" ]]; then
    echo "ERROR: initialize the wine submodule first." >&2
    exit 1
fi
[[ -f "$WINE_BUILD/Makefile" ]] || {
    echo "ERROR: $WINE_BUILD is not configured; run scripts/prepare-wine-ios.sh first." >&2
    exit 1
}

# llvm-mingw's strip, per architecture, so the modules land in the bundle the
# same size the committed ones are (see the strip comment where they are
# installed). Absent, the build still works and says how many modules went in
# unstripped -- a silent 3x IPA is worse than a warning.
strip_for() {
    local arch="$1" candidate="$MINGW/bin/${arch%%-windows}-w64-mingw32-strip"
    [[ -x "$candidate" ]] || return 1
    printf '%s' "$candidate"
}
for prefix in aarch64 arm64ec; do
    [[ -x "$MINGW/bin/$prefix-w64-mingw32-clang" ]] || {
        echo "ERROR: llvm-mingw is missing the $prefix target; run scripts/prepare-wine-ios.sh." >&2
        exit 1
    }
done

# Requested modules, in manifest order, split from the ones that are already
# there (or protected). `native` entries are not buildable from Wine and are
# reported, not attempted.
requested=()
skipped_present=()
skipped_protected=()
native_only=()
missing_rule=()

while read -r kind module component _; do
    [[ "$kind" == "wine" || "$kind" == "native" ]] || continue
    [[ -n "${module:-}" ]] || continue
    if [[ "$kind" == "native" ]]; then
        native_only+=("$module${component:+ ($component)}")
        continue
    fi
    if is_protected "$module"; then
        skipped_protected+=("$module")
        continue
    fi
    requested+=("$module")
done < <(sed 's/#.*//' "$MANIFEST")

# One arch at a time, so a failure names the arch it failed in. A module present
# in one directory and absent from the other is exactly the asymmetry that makes
# a title fail in one session and work in the other, so the two are tracked
# separately rather than as "the module is done".
apply_patch wine-arm64ec-fastfail.patch
apply_patch wine-makedep-per-arch-pe.patch

# The rules asked for below are generated by tools/makedep, and one of those two
# patches rewrites it. A build directory configured *before* the patch still
# holds a Makefile made by the unpatched makedep, in which the arm64ec-windows/
# half of every module does not exist as a separate target at all -- it is folded
# into aarch64-windows/ as an ARM64X hybrid -- and the has_rule loop then reports
# the whole arm64ec set as unbuildable. That is a true statement about a stale
# Makefile, not about the tree, and every caller that runs prepare-wine-ios.sh
# first (the IPA workflow included) is in exactly that state.
#
# `make Makefile` is Wine's own regeneration path: the generated Makefile depends
# on config.status and tools/makedep, so this rebuilds makedep from the patched
# source and re-runs it to regenerate the rules. No-op when the file is current.
make -C "$WINE_BUILD" -j"$JOBS" Makefile

targets=()
for arch in "${WANTED_ARCHS[@]}"; do
    for module in "${requested[@]}"; do
        if [[ "$FORCE" == 0 ]] && [[ -s "$BUNDLE/$arch/$module.dll" ]]; then
            skipped_present+=("$arch/$module")
            continue
        fi
        target="dlls/$module/$arch/$module.dll"
        if ! has_rule "$target"; then
            missing_rule+=("$target")
            continue
        fi
        targets+=("$target")
    done
done

# Typelibs precede the modules that import them, for both architectures, and
# whether or not the module that needs one is in the requested set -- an arm64ec
# build asks widl for the aarch64 copy (see the note at the top of the file).
# They are always rebuilt with the modules, so they are not in skipped_present.
TYPELIBS=(stdole2.tlb stdole32.tlb activeds.tlb mshtml.tlb)
typelib_targets=()
for arch in "${WANTED_ARCHS[@]}"; do
    for tlb in "${TYPELIBS[@]}"; do
        target="dlls/$tlb/$arch/$tlb"
        if has_rule "$target"; then
            typelib_targets+=("$target")
        else
            missing_rule+=("$target")
        fi
    done
done

if [[ ${#missing_rule[@]} -gt 0 ]]; then
    echo "ERROR: the generated Makefile has no rule for ${#missing_rule[@]} of the requested targets:" >&2
    printf '         %s\n' "${missing_rule[@]}" >&2
    echo "       Either the module is not built by this Wine revision (remove it from" >&2
    echo "       tools/pe-module-manifest.txt) or the tree was configured without" >&2
    echo "       patches/wine-makedep-per-arch-pe.patch applied, in which case every" >&2
    echo "       arm64ec-windows/ output collapses into aarch64-windows/." >&2
    exit 1
fi

if [[ ${#targets[@]} -eq 0 ]]; then
    echo "Nothing to build: every wine module in the manifest is present (use --force to rebuild)."
    exit 0
fi

echo "Building ${#targets[@]} PE modules for ${WANTED_ARCHS[*]} (${#native_only[@]} native modules are not buildable from Wine)."
if [[ "$DRY_RUN" == 1 ]]; then
    printf '  %s\n' "${typelib_targets[@]}" "${targets[@]}"
    exit 0
fi

# A forced rebuild has to delete the outputs first: make cannot see that a
# target is stale with respect to a patch or a configure flag.
if [[ "$FORCE" == 1 ]]; then
    for target in "${targets[@]}" "${typelib_targets[@]}"; do rm -f "$WINE_BUILD/$target"; done
fi
# -k so one module that fails to compile does not hide the state of the other
# 200; the per-file check below is what decides success.
make -C "$WINE_BUILD" -j"$JOBS" "${typelib_targets[@]}"
make -C "$WINE_BUILD" -k -j"$JOBS" "${targets[@]}"

built=0
failed=()
unstripped=0
for arch in "${WANTED_ARCHS[@]}"; do
    mkdir -p "$BUNDLE/$arch"
done
for target in "${targets[@]}"; do
    arch="$(basename "$(dirname "$target")")"
    name="$(basename "$target")"
    if [[ ! -s "$WINE_BUILD/$target" ]]; then
        failed+=("$target")
        continue
    fi
    install -m 755 "$WINE_BUILD/$target" "$BUNDLE/$arch/$name"

    # Wine's build leaves full DWARF in every module (-gdwarf-4 -g), and it is
    # most of the file. Measured on this tree: stripping takes d2d1 from 4.6 MB
    # to 0.7 MB, msxml3 from 10.9 MB to 2.6 MB, and the 143 modules from 168 MB
    # to 50 MB (arm64ec) and 267 MB to 111 MB (aarch64), byte counts not du. Debug info in a shipped DLL is pure payload, and --strip-debug
    # leaves the export table bit-identical (verified: 336 exports in, 336 out on
    # a d3dx9_41 arm64ec module). Not --strip-all: 16% smaller again, but it drops
    # the COFF symbol table, which is a risk with no reward here.
    if STRIP="$(strip_for "$arch")"; then
        "$STRIP" --strip-debug "$BUNDLE/$arch/$name" 2>/dev/null || unstripped=$((unstripped + 1))
    else
        unstripped=$((unstripped + 1))
    fi
    built=$((built + 1))
done

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "ERROR: ${#failed[@]} of ${#targets[@]} modules did not build:" >&2
    printf '         %s\n' "${failed[@]}" >&2
    exit 1
fi

# Reuse the bundle validator's predicates rather than a second opinion about what
# a correct artifact looks like: an ARM64EC image is marked AMD64 (0x8664) with
# the EC metadata in .a64xrm, and a native ARM64 one is 0xAA64. A module in the
# wrong one of those directories loads in the session it does not belong to.
python3 - "$REPO_ROOT" "$built" <<'PY'
import runpy
import sys
from pathlib import Path

root = Path(sys.argv[1])
validator = runpy.run_path(str(root / "tools/validate-ios-bundle.py"))
built = int(sys.argv[2])

checked = 0
for arch, machine in (("arm64ec-windows", 0x8664), ("aarch64-windows", 0xAA64)):
    directory = root / "app/Madeira" / arch
    for path in sorted(directory.glob("*.dll")):
        validator["pe"](path.read_bytes(), machine)
        checked += 1
print(f"Validated {checked} PE modules in both architecture directories ({built} built by this run).")
PY

echo "Built and installed $built modules."
if [[ $unstripped -gt 0 ]]; then
    echo "WARNING: $unstripped module(s) went in with debug info; the IPA will be"
    echo "         several times larger than it needs to be. Check that"
    echo "         $MINGW/bin has the *-w64-mingw32-strip binaries." >&2
fi
if [[ ${#skipped_present[@]} -gt 0 ]]; then
    echo "Already present, left alone: ${#skipped_present[@]} modules (--force rebuilds them)."
fi
if [[ ${#native_only[@]} -gt 0 ]]; then
    echo "Not built, Microsoft-only: ${#native_only[@]} modules"
    printf '  %s\n' "${native_only[@]}"
    echo "  These need the redistributable component, not a Wine build:"
    echo "    tools/fetch-directx.sh   -> app/Madeira/x86_64-directx/"
fi
if [[ ${#skipped_protected[@]} -gt 0 ]]; then
    echo "Refused (owned by DXMT or the XInput patch): ${skipped_protected[*]}"
fi
echo "Next: python3 tools/check-pe-module-set.py --strict"
echo "The modules land in app/Madeira/{arm64ec,aarch64}-windows/ and are packaged"
echo "into the IPA by .github/workflows/ipa.yml, which runs this script; they are"
echo "not committed, so they always match the pinned wine revision."
