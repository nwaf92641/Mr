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

SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
if [[ -z "$SDK" ]]; then
    echo "metal4-milestone: SKIP -- no macOS SDK on this machine"
    exit 3
fi

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
