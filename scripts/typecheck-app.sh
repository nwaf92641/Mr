#!/bin/bash
# Compile-check the app's Swift and its C bridging header, with no native
# dependencies and no xcodebuild. Two minutes instead of twenty-five.
#
# Why this exists: the app's Swift can only be compiled on a Mac with the iOS
# SDK, so every mistake in it -- a C function the bridging header does not
# expose, a property a framework does not have -- was first found by a full
# pipeline that builds FEX, Wine, LLVM and DXMT before it reaches xcodebuild.
# ml808 lost three runs that way, at ~25 minutes each.
#
# It is the same compiler and the same SDK as the build (swiftc -typecheck with
# SWIFT_OBJC_BRIDGING_HEADER and the project's SWIFT_VERSION and deployment
# target), so anything it rejects, xcodebuild rejects. What it does not cover is
# linking, the C/ObjC sources, and Swift that is only wrong for the *runtime* --
# it is a fast front-of-queue gate, not a replacement for the IPA job.
#
# Usage: scripts/typecheck-app.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$REPO_ROOT/app/Madeira"
BRIDGE="$APP/Madeira-Bridging-Header.h"

if [[ "$(uname -s)" != Darwin ]]; then
    echo "ERROR: this needs macOS with Xcode (the iOS SDK has no Linux build)." >&2
    exit 1
fi

# ContentView.swift calls glassEffect() behind #available(iOS 26.0), so the iOS
# 26 SDK is required -- the same rule the IPA workflow applies. DEVELOPER_DIR
# rather than xcode-select so a runner that needs sudo does not need it here.
if [[ -z "${DEVELOPER_DIR:-}" ]]; then
    newest="$(ls -d /Applications/Xcode_26*.app 2>/dev/null | sort -V | tail -1 || true)"
    if [[ -n "$newest" ]]; then
        export DEVELOPER_DIR="$newest/Contents/Developer"
    fi
fi
SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
SDK_VERSION="$(xcrun --sdk iphoneos --show-sdk-version)"
if [[ "${SDK_VERSION%%.*}" -lt 26 ]]; then
    echo "ERROR: the app needs the iOS 26 SDK for glassEffect(); found $SDK_VERSION." >&2
    echo "       Set DEVELOPER_DIR to an Xcode 26 install." >&2
    exit 1
fi

# Every Swift file in the app, as the Xcode target compiles them. tools/check-xcodeproj.py
# is what keeps that target's file list honest; this reads the directory.
sources=()
while IFS= read -r file; do
    sources+=("$file")
done < <(find "$APP" -name '*.swift' | sort)
[[ ${#sources[@]} -gt 0 ]] || { echo "ERROR: no Swift sources under $APP." >&2; exit 1; }

echo "typecheck-app: swiftc $(xcrun swiftc --version 2>/dev/null | head -1) | SDK $SDK_VERSION"
echo "typecheck-app: ${#sources[@]} Swift files, bridging header $(basename "$BRIDGE")"

# -swift-version 5 and the deployment target come from project.pbxproj; keeping
# them in step is what makes a clean typecheck mean the build will compile.
# -I so the bridging header's #import "Winios/Winios.h" resolves; every header
# it pulls in is self-contained (system includes only), which is why no
# FEX/Wine/LLVM include paths are needed here.
xcrun swiftc -typecheck \
    -swift-version 5 \
    -sdk "$SDK_PATH" \
    -target arm64-apple-ios17.0 \
    -module-name Madeira \
    -I "$APP" \
    -import-objc-header "$BRIDGE" \
    "${sources[@]}"

echo "typecheck-app: OK -- the app's Swift and bridging header compile"
