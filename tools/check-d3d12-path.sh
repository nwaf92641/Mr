#!/bin/bash
# Where D3D12 stands, decided by probing the machine rather than asserted.
#
# What is established (and does not need re-establishing) is that this tree has
# no D3D12 front end: DXMT is d3d10/d3d11, and the d3d12.dll in app/Madeira is
# Wine's builtin, which is a vkd3d-shaped front end over a Vulkan device. The
# Vulkan route is excluded, so the question is whether Apple ships something
# usable, and this script answers it against the actual machine.
#
# The Apple pieces, and where they sit:
#
#   D3DMetal.framework, libd3dshared.dylib
#       The D3D12 implementation -> Metal. GPTK's evaluation environment places
#       them in a Wine tree at lib/external/. They are Mach-O host libraries,
#       macOS-only, and Apple's licence covers evaluating a game for porting,
#       not redistribution -- so they can never be committed here, and this
#       script fails if they are tracked in git.
#
#   metal-shaderconverter
#       DXIL -> metallib. A macOS build tool. Apple's own wording is that the
#       library it produces is ready for Mac, iPad and iPhone, so the output
#       travels; the tool that produces it does not. Game shaders arrive as DXIL
#       at run time, which is the reason a converted-output path is not the same
#       as a translation path.
#
# So the verdict this prints is per target, and the iOS verdict is the one that
# matters: the PE side of D3D12 could be overlaid the way the Microsoft DirectX
# redist already is, but the host side it must talk to is macOS-only.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
status=0

echo "check-d3d12: --- Apple D3D12 components on this machine"

found_host=""
for candidate in \
    "/Applications/Game Porting Toolkit.app/Contents/Resources/wine/lib/external" \
    "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib64/apple_gptk/external" \
    "$HOME/Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine/lib/external" \
    /Volumes/Evaluation*for*Windows*games*/redist/lib/external; do
    if [[ -d "$candidate" ]]; then
        echo "check-d3d12:   directory: $candidate"
        [[ -e "$candidate/D3DMetal.framework" ]] &&
            echo "check-d3d12:     D3DMetal.framework present" && found_host="yes"
        [[ -e "$candidate/libd3dshared.dylib" ]] &&
            echo "check-d3d12:     libd3dshared.dylib present" && found_host="yes"
    fi
done
[[ -z "$found_host" ]] && echo "check-d3d12:   no GPTK external library directory found"

converter="$(command -v metal-shaderconverter 2>/dev/null || true)"
[[ -z "$converter" ]] && converter="$(xcrun --find metal-shaderconverter 2>/dev/null || true)"
if [[ -n "$converter" ]]; then
    echo "check-d3d12:   metal-shaderconverter: $converter"
else
    echo "check-d3d12:   metal-shaderconverter: not installed"
fi

# The licence guard, and the reason it is a failure rather than a warning: these
# are Apple's binaries under a licence that does not permit redistribution, so a
# commit that adds them is a licensing problem, not a build problem.
if git -C "$ROOT" ls-files | grep -qiE "D3DMetal|libd3dshared|metal-shaderconverter|metalirconverter"; then
    echo "check-d3d12: FAIL -- an Apple D3D12 or shader-converter binary is tracked in git"
    git -C "$ROOT" ls-files | grep -iE "D3DMetal|libd3dshared|metal-shaderconverter|metalirconverter" | sed 's/^/check-d3d12:   /'
    echo "check-d3d12:   Apple's licence permits evaluating a game for porting, not redistribution"
    status=1
else
    echo "check-d3d12: no Apple licensed binary is tracked (correct)"
fi

echo "check-d3d12: --- verdict"
if [[ -n "$found_host" ]]; then
    echo "check-d3d12:   D3DMetal host libraries are present on this machine."
    echo "check-d3d12:   They are macOS-only: the pair is a Mach-O framework plus a dylib, and the"
    echo "check-d3d12:   PE side has to talk to them through Wine's unix-call path."
    echo "check-d3d12:   target macOS: a path exists, as an external toolchain the user supplies."
    echo "check-d3d12:   target iOS  : NOT AVAILABLE -- the host side is not built for iOS and"
    echo "check-d3d12:                  cannot be shipped inside an IPA."
else
    echo "check-d3d12:   target macOS: no D3DMetal on this machine; nothing to run against."
    echo "check-d3d12:   target iOS  : NOT AVAILABLE -- no host implementation exists to talk to,"
    echo "check-d3d12:                  and none is distributable if it did."
fi
echo "check-d3d12:   D3D11 is unaffected: it goes DXMT -> Metal 4 and does not involve any of this."

if [[ "$status" != 0 ]]; then
    exit "$status"
fi
exit 0
