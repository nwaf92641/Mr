#!/bin/bash
# Run the Metal 4 milestone harness on this machine's GPU.
#
# tools/check-metal4.sh compiles the harness against the iOS SDK and archives it,
# which proves it builds. It never runs, so nothing in CI has ever reported which
# stage the Metal 4 path actually reaches. This does that, against the macOS SDK,
# because the runner is an Apple silicon Mac: if it has a Metal 4 GPU the stages
# execute, and if it does not the harness says SKIP and this exits 3.
#
# The exit codes are the point:
#   0  every stage reached -- the device, the queue, the argument table, the
#      frame, the encoder, the draw and the commit all happened on a real GPU
#   2  a stage failed; the table above names it
#   3  no Metal 4 device here -- not a pass, and not a drawn frame
#
# What 0 does not mean: nothing here goes through FEX, Wine, D3D11 or DXMT. It is
# the last hop of that path in isolation, so it can be green while the path from
# an EXE is still unproven. The stage table says which hop this is.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# The macOS SDK has to come from an Xcode that has one, and on this runner the
# default does not: `xcrun --sdk macosx` resolves to MacOSX15.5.sdk, which has no
# Metal 4 at all -- no MTLGPUFamilyMetal4, no MTL4CommandQueue -- so the harness
# failed to build and reported that, which is how a runtime check earns its
# place. The Xcode that compiles the layer against iPhoneOS 26.2 is the one to
# use, so pick the Xcode carrying a macOS 26 SDK rather than trusting the
# default.
XCODE_DEVELOPER=""
SDK=""
for app in /Applications/Xcode*.app; do
    [[ -d "$app" ]] || continue
    candidate="$(ls -d "$app"/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26*.sdk 2>/dev/null | tail -1)"
    if [[ -n "$candidate" ]]; then
        XCODE_DEVELOPER="$app/Contents/Developer"
        SDK="$candidate"
    fi
done

if [[ -z "$SDK" ]]; then
    echo "metal4-milestone: SKIP -- no Xcode on this machine has a macOS 26 SDK,"
    echo "metal4-milestone:         and Metal 4 needs one; a macOS 15 SDK has no"
    echo "metal4-milestone:         MTLGPUFamilyMetal4 and no MTL4CommandQueue"
    exit 3
fi
export DEVELOPER_DIR="$XCODE_DEVELOPER"
echo "metal4-milestone: --- toolchain $DEVELOPER_DIR"

if ! xcrun --sdk macosx --find clang++ >/dev/null 2>&1; then
    echo "metal4-milestone: SKIP -- no clang++ for macOS"
    exit 3
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CXX="$(xcrun --sdk macosx --find clang++)"
echo "metal4-milestone: --- building against $SDK"
if ! "$CXX" -isysroot "$SDK" -std=c++17 -fobjc-arc -O1 \
    -I graphics/metal4 \
    graphics/metal4/mr_metal4.mm \
    graphics/metal4/mr_metal4_milestone.mm \
    tools/run-metal4-milestone.mm \
    -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
    -o "$WORK/milestone" 2>&1 | sed 's/^/metal4-milestone:   /'; then
    echo "metal4-milestone: FAIL -- the harness does not build for macOS"
    exit 2
fi
echo "metal4-milestone: built"

echo "metal4-milestone: --- running"
set +e
"$WORK/milestone"
result=$?
set -e

echo "metal4-milestone: --- exit $result"
case "$result" in
    0) echo "metal4-milestone: every Metal 4 stage reached on this GPU";;
    3) echo "metal4-milestone: no Metal 4 device here -- stages not executed";;
    *) echo "metal4-milestone: a stage failed; the table above names it";;
esac
echo "metal4-milestone: this is the Metal 4 hop alone -- not FEX, Wine, D3D11 or DXMT"
exit "$result"
