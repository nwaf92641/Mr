#!/bin/bash
# Shared, real Wine inputs for the iOS native libraries and optional DXMT PE build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_SRC="$REPO_ROOT/wine"
WINE_BUILD="$WINE_SRC/build-macos"
# The FreeType checkout build/freetype-ios/build.sh builds from; its headers are
# on the include path of the Wine unix libraries, so the check below treats it as
# an input in its own right.
SRC_FREETYPE="$REPO_ROOT/research/freetype"
MINGW_VERSION=20260421
MINGW_DIR="$REPO_ROOT/toolchains/llvm-mingw-$MINGW_VERSION-ucrt-macos-universal"
DXMT_PE=0
case "${1:-}" in
    "") ;;
    --dxmt-pe) DXMT_PE=1 ;;
    *) echo "Usage: $0 [--dxmt-pe]" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "Usage: $0 [--dxmt-pe]" >&2; exit 2; }

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "ERROR: Wine iOS preparation requires an Apple Silicon Mac with Xcode." >&2
    exit 1
fi
[[ -f "$WINE_SRC/configure" ]] || { echo "ERROR: initialize the wine submodule first." >&2; exit 1; }
JOBS="${JOBS:-${BUILD_JOBS:-}}"
if [[ -z "$JOBS" ]]; then
    JOBS="$(sysctl -n hw.ncpu)"
fi
xcrun --sdk iphoneos --show-sdk-version

if [[ ! -x "$MINGW_DIR/bin/aarch64-w64-mingw32-clang" ]]; then
    mkdir -p "$REPO_ROOT/toolchains"
    download="$(mktemp -d "${TMPDIR:-/tmp}/wine-mingw.XXXXXX")"
    trap 'rm -rf "$download"' EXIT
    curl --fail --silent --show-error --location --retry 3 \
        --connect-timeout 30 --max-time 600 --proto '=https' --proto-redir '=https' \
        "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_VERSION/llvm-mingw-$MINGW_VERSION-ucrt-macos-universal.tar.xz" \
        -o "$download/llvm-mingw.tar.xz"
    tar -xJf "$download/llvm-mingw.tar.xz" -C "$REPO_ROOT/toolchains"
fi
export PATH="$MINGW_DIR/bin:$PATH"
# Both targets are required, not just aarch64: DXMT ships DLLs for the native
# ARM64 session and for the ARM64EC session that x64 games actually run in.
# windres and strip are listed because build-pe.sh names them in each cross
# file; a missing one there fails the Meson setup, long after this check.
for prefix in aarch64-w64-mingw32 arm64ec-w64-mingw32; do
    for tool in clang clang++ ar strip windres; do
        [[ -x "$MINGW_DIR/bin/$prefix-$tool" ]] || {
            echo "ERROR: incomplete llvm-mingw: $prefix-$tool missing." >&2
            exit 1
        }
    done
done
for tool in llvm-dlltool llvm-ar lld; do
    [[ -x "$MINGW_DIR/bin/$tool" ]] || { echo "ERROR: incomplete llvm-mingw: $tool missing." >&2; exit 1; }
done
aarch64-w64-mingw32-clang --version
arm64ec-w64-mingw32-clang --version

# Re-run configure even after cache restoration: config.h alone says nothing
# about the source revision, selected architecture, SDK or generated IDL headers.
mkdir -p "$WINE_BUILD"
(
    cd "$WINE_BUILD"
    # Wine's build tools execute on macOS, but the compiler is invoked by its
    # absolute Xcode path, which carries no default sysroot: without an
    # explicit -isysroot even <stdio.h> is missing (CI: tools/widl failed
    # after configure itself passed — its probe program needs no headers).
    # A bare exported SDKROOT does not fix it, and subshell exports would not
    # reach the make steps below this block anyway. So resolve the SDK once,
    # verify it up front (seconds, not minutes), and bake -isysroot into the
    # flags configure records in its Makefiles — configure tests and make
    # then compile and link against the same SDK.
    MACOS_SDK="$(xcrun --sdk macosx --show-sdk-path)"
    if [[ -z "$MACOS_SDK" || ! -d "$MACOS_SDK" ]]; then
        echo "ERROR: macOS SDK not found (xcrun returned '${MACOS_SDK:-<empty>}')." >&2
        exit 1
    fi
    if [[ ! -f "$MACOS_SDK/usr/include/stdio.h" ]]; then
        echo "ERROR: macOS SDK has no usr/include/stdio.h: $MACOS_SDK" >&2
        exit 1
    fi
    echo "macOS SDK: $MACOS_SDK ($(xcrun --sdk macosx --show-sdk-version))"
    SYSROOT_FLAGS="-isysroot $MACOS_SDK"
    # Preserve configure's actual compiler/linker diagnostic on failure.
    trap 'status=$?; if [[ $status -ne 0 && -f config.log ]]; then cat config.log >&2; fi; exit "$status"' EXIT
    CC="$(xcrun --sdk macosx --find clang)" \
    CXX="$(xcrun --sdk macosx --find clang++)" \
    CFLAGS="${CFLAGS:-} $SYSROOT_FLAGS" \
    CPPFLAGS="${CPPFLAGS:-} $SYSROOT_FLAGS" \
    LDFLAGS="${LDFLAGS:-} $SYSROOT_FLAGS" \
    ../configure --enable-win64 --enable-archs=aarch64,arm64ec --with-mingw=llvm-mingw \
        --without-x --without-freetype --without-vulkan --disable-tests \
        --prefix=/tmp/wine-ios
) 2>&1 | tee "$REPO_ROOT/wine-configure.log"
make -C "$WINE_BUILD" -j"$JOBS" include/all 2>&1 | tee "$REPO_ROOT/wine-headers-build.log"
for header in config.h dwrite.h dwrite_3.h; do
    [[ -s "$WINE_BUILD/include/$header" ]] || { echo "ERROR: generated Wine header missing: $header" >&2; exit 1; }
done
# FreeType takes minutes to rebuild and its outputs are cached alongside the
# native dependencies: skip when the staged archive and headers validate.
#
# The source tree is part of the test because the cache is not. The native cache
# holds build/freetype-ios/build -- the cmake output -- but not research/freetype,
# and it is research/freetype/include that build/ntdll-unix/build.sh and
# build/win32u-unix/build.sh put on their include path. A run with a native cache
# hit and no Wine-unix cache therefore skipped this rebuild, restored a
# libfreetype.a, and then died at "required Wine input missing:
# research/freetype/include/ft2build.h", which reads like a broken checkout
# rather than a cache that never contained the headers.
if python3 "$REPO_ROOT/tools/validate-ios-bundle.py" --archive "$REPO_ROOT/build/freetype-ios/build/libfreetype.a" >/dev/null 2>&1 \
    && [[ -s "$REPO_ROOT/build/freetype-ios/build/include/freetype/config/ftconfig.h" ]] \
    && [[ -s "$SRC_FREETYPE/include/ft2build.h" ]]; then
    echo "FreeType outputs valid — skipping rebuild."
else
    bash "$REPO_ROOT/build/freetype-ios/build.sh" 2>&1 | tee "$REPO_ROOT/freetype-build.log"
fi

if [[ "$DXMT_PE" == 1 ]]; then
    # These are PE/COFF libraries, not the iOS Mach-O archives. Always rebuild
    # their final outputs so old scaffolding archives/scripts cannot satisfy make.
    # One set per architecture: DXMT links the arm64ec-windows DLLs against the
    # arm64ec imports, and using the aarch64 set instead produces link errors
    # that look like DXMT bugs rather than a missing import library.
    targets=(
        tools/winebuild/winebuild
        libs/winecrt0/aarch64-windows/libwinecrt0.a
        dlls/ntdll/aarch64-windows/libntdll.a
        dlls/dbghelp/aarch64-windows/libdbghelp.a
        libs/winecrt0/arm64ec-windows/libwinecrt0.a
        dlls/ntdll/arm64ec-windows/libntdll.a
        dlls/dbghelp/arm64ec-windows/libdbghelp.a
    )
    # A target Wine generated no rule for fails deep inside make as "No rule to
    # make target", which reads like a missing file rather than a configure that
    # never saw patches/wine-makedep-per-arch-pe.patch. Check the generated
    # makefile text up front so the error names its cause.
    for target in "${targets[@]:1}"; do
        grep -q "^$target:" "$WINE_BUILD/Makefile" || {
            echo "ERROR: Wine's generated Makefile has no rule for $target." >&2
            echo "       Apply patches/wine-makedep-per-arch-pe.patch before configure runs:" >&2
            echo "       without it Wine pairs aarch64+arm64ec into one ARM64X image and" >&2
            echo "       every arm64ec-windows/ output collapses into aarch64-windows/." >&2
            exit 1
        }
    done
    for target in "${targets[@]}"; do rm -f "$WINE_BUILD/$target"; done
    make -C "$WINE_BUILD" -j"$JOBS" "${targets[@]}" 2>&1 | tee "$REPO_ROOT/wine-dxmt-pe-build.log"
    "$WINE_BUILD/tools/winebuild/winebuild" --version
    for target in "${targets[@]:1}"; do
        # llvm-ar must be able to read actual members; eight-byte placeholders fail.
        members="$("$MINGW_DIR/bin/llvm-ar" t "$WINE_BUILD/$target")"
        [[ -n "$members" ]] || { echo "ERROR: empty Wine PE archive: $target" >&2; exit 1; }
    done
fi
