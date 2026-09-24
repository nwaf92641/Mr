#!/bin/bash
# Compile Mr's Metal 4 layer against the real iOS SDK.
#
# The layer names MTL4 selectors and properties from Apple's documentation, and
# nothing else in the tree compiles them: build/dxmt-ios/build.sh does, but only
# after FEX, GnuTLS, LLVM 15 and Wine have been built, about three hours in.
# This is the same compile in about a minute, so a wrong selector is reported
# before the expensive work rather than after it.
#
# It is a compile check and nothing more. It cannot tell you Metal 4 works --
# that needs a device with MTLGPUFamilyMetal4 and is what
# graphics/metal4/mr_metal4_milestone.mm is for.
#
# Exits 0 with SKIP when there is no Apple toolchain or no Metal 4 SDK, so it is
# safe on a Linux checkout and on a runner whose Xcode predates iOS 26.
set -uo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_DIR="$ROOT/graphics/metal4"

if ! command -v xcrun >/dev/null 2>&1; then
    echo "check-metal4: SKIP -- no xcrun (this is not a macOS checkout)"
    exit 0
fi

# The app is built with the newest Xcode 26 when the runner has one, and the
# Metal 4 headers only exist from iOS 26. Match that selection here.
NEWEST=$(ls -d /Applications/Xcode_26*.app 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "$NEWEST" ]]; then
    export DEVELOPER_DIR="$NEWEST/Contents/Developer"
fi

if ! SDK=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null); then
    echo "check-metal4: SKIP -- no iphoneos SDK"
    exit 0
fi
SDK_VERSION=$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo 0)

# Metal 4 headers are the real precondition. Naming a specific one means a
# pre-26 SDK reports SKIP rather than a wall of unknown-type errors.
if [[ ! -f "$SDK/System/Library/Frameworks/Metal.framework/Headers/MTL4CommandQueue.h" ]]; then
    echo "check-metal4: SKIP -- iPhoneOS $SDK_VERSION has no Metal 4 headers (MTL4CommandQueue.h)"
    echo "check-metal4:        the layer stays uncompiled until an iOS 26 SDK is selected"
    exit 0
fi

# Same minimum as the app, so an accidental use of a non-Metal-4 API that is
# newer than the deployment target fails here rather than in make-ipa.sh.
DEPLOYMENT="${IOS_DEPLOYMENT_TARGET:-18.0}"
OBJ_DIR="$ROOT/build/metal4"
mkdir -p "$OBJ_DIR"

FLAGS=(-arch arm64 -isysroot "$SDK" "-miphoneos-version-min=$DEPLOYMENT" -fobjc-arc -fblocks -O2
       -std=c++17 -I"$SRC_DIR")
if [[ "${MR_METAL4_WERROR:-0}" == "1" ]]; then
    FLAGS+=(-Werror)
fi

OBJECTS=()
status=0

# When a compile fails, the useful thing is not the error but the declaration
# the error is about: MTL4 is new enough that its selectors are not in any
# reference this repository can reach offline, and guessing a second time would
# be the same mistake twice. This prints the real signatures out of the SDK
# headers that are on the runner, so the fix is written from evidence.
dump_surface() {
    local headers=("$SDK"/System/Library/Frameworks/Metal.framework/Headers/*.h)
    local topic pattern
    while IFS='|' read -r topic pattern; do
        [[ -n "$topic" ]] || continue
        echo "check-metal4: --- $topic"
        grep -nH -A2 -E "$pattern" "${headers[@]}" | grep -vE ':[0-9]+: *(\*|//)' | head -40 | sed 's/^/    /'
    done <<'PATTERNS'
gpuResourceID|gpuResourceID
argument table setters|setAddress|setTexture:|setSamplerState:|setBytes:
MTLRenderStages|MTLRenderStages
storage mode enums|MTLStorageMode|MTLResourceStorageMode
drawIndexed on MTL4|drawIndexedPrimitives|drawPrimitives:
queue residency|addResidencySet|MTLResidencySet
drawable on queue|waitForDrawable|signalDrawable
device factories|newMTL4CommandQueue|newCommandAllocator|newCommandBuffer|newArgumentTableWithDescriptor|newCompilerWithDescriptor|newResidencySetWithDescriptor|newSharedEvent
render encoder creation|renderCommandEncoderWithDescriptor
argument table on encoder|setArgumentTable
dispatch|dispatchThreadgroups|dispatchThreads:
queue commit|commit:.*count:|signalEvent:|waitForEvent:
argument table descriptor|maxBufferBindCount|maxTextureBindCount|maxSamplerStateBindCount|supportAttributeStrides|initializeBindings
render pass descriptor width|renderTargetWidth|renderTargetHeight
suspend resume|MTL4RenderEncoderOption
PATTERNS
}

for source in mr_metal4.mm mr_metal4_milestone.mm; do
    if ! xcrun --sdk iphoneos clang++ "${FLAGS[@]}" -c "$SRC_DIR/$source" \
            -o "$OBJ_DIR/${source%.mm}.o" 2>"$OBJ_DIR/${source%.mm}.log"; then
        echo "check-metal4: FAIL -- $source does not compile against iPhoneOS $SDK_VERSION" >&2
        sed 's/^/    /' "$OBJ_DIR/${source%.mm}.log" >&2
        status=1
    else
        echo "check-metal4: compiled $source"
        OBJECTS+=("$OBJ_DIR/${source%.mm}.o")
    fi
done

if [[ "$status" != 0 ]]; then
    dump_surface >&2
    exit 1
fi

# Linking them together catches what compiling each alone cannot: a symbol one
# file defines and the other calls, and the enum bridge, which is asserted in
# mr_metal4.mm but only evaluated once the translation unit is built.
if ! xcrun --sdk iphoneos libtool -static -o "$OBJ_DIR/libmr_metal4.a" "${OBJECTS[@]}" 2>"$OBJ_DIR/libtool.log"; then
    echo "check-metal4: FAIL -- could not archive the objects" >&2
    sed 's/^/    /' "$OBJ_DIR/libtool.log" >&2
    exit 1
fi

echo "check-metal4: OK -- Metal 4 layer compiles and archives against iPhoneOS $SDK_VERSION"
echo "check-metal4:        $OBJ_DIR/libmr_metal4.a ($(du -h "$OBJ_DIR/libmr_metal4.a" | cut -f1))"
echo "check-metal4:        compile check only; running it needs a device with MTLGPUFamilyMetal4"
