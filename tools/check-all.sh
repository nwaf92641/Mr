#!/bin/sh
# Pre-release gate: run every consistency check that guards a shipped artifact.
#
# Each check here exists because the thing it guards has already shipped broken
# once:
#   - jit-script-sync: the JIT debugger script lives base64-embedded in
#     StikJITHelper.swift and madeira-jit.js is NOT in the Xcode target, so
#     editing the file changes nothing until the literal is regenerated.
#   - check-prefix-template: the prefix template shipped absolute host symlinks
#     that dangle on every device (ml719).
#   - check-xcodeproj: the Xcode project lists every source by hand in four
#     places, so a file that exists but was never registered is silently not
#     part of the program -- the same shape of bug as jit-script-sync, and
#     invisible on any machine that cannot open Xcode.
#   - test-device-capabilities: the JIT pool size is now derived from the
#     device's memory budget, and the override-file parsers are shared by two
#     call sites. Both are pure functions with a table of expected values; the
#     harness only needs swiftc, which a release build has anyway. It skips
#     (exit 0) rather than fails when no toolchain is present.
#   - test-app-ui: same idea for LayoutPolicy. That one comparison is what kept
#     every iPad out of fullscreen, and it is invisible until a device is held.
#   - check-swift-syntax: `swiftc -parse` every app source. The UI is SwiftUI,
#     so nothing in it can be type-checked off a Mac; this at least proves the
#     files parse, which is what a dropped brace costs a build for.
#   - check-swift-c-symbols: every C function Swift calls must be declared in a
#     header the bridging header imports, because that is the only way Swift can
#     see it. Parsing does not resolve names, so this is the gap between
#     check-swift-syntax and a real compile -- and the one ml808 fell into twice.
#     The full typecheck (scripts/typecheck-app.sh) needs the iOS SDK and runs
#     as its own CI job instead.
#   - check-wine-pe-stamp: the XInput PE DLLs are committed binaries and no
#     runner rebuilds them, so a patch edited without re-running
#     scripts/build-wine-xinput-pe.sh would ship behavior the tree says it does
#     not have. Same shape as the stale arm64ec DXMT set (ml807), and invisible
#     for the same reason: the binaries are opaque.
#   - test-xinput-pad: the app's half of the XInput ABI and the unix-call table
#     the guest dispatches through. The struct is written twice -- app and
#     guest -- and a field added on one side only compiles in both places and
#     hands the guest garbage. Needs a C compiler; skips (exit 0) without one.
#   - check-nls-set: the codepage tables the loader resolves by name. Four of
#     the sixty-eight the pinned Wine ships reached the bundle for months (ml809)
#     and every title that asked for its own codepage died in locale setup with
#     STATUS_OBJECT_NAME_NOT_FOUND. Compares the bundle against wine/nls.
#   - test-graphics-driver-pin: the registry edit that stops Wine looking for
#     winemac/x11/wayland.drv (ml810). It rewrites a user's user.reg on disk, so
#     a bug there corrupts their settings rather than failing a build. Asserts
#     both that it writes when it should and that it is byte-for-byte inert when
#     it should be, against fixtures and the shipped prefix template.
#   - check-dll-aliases: the DirectX name aliases (ml812). The alias table is a
#     claim that a name a title imports resolves to a module the bundle ships
#     from the session's system32 *and that the module exports what the name was
#     supposed to export*; the second half is the one that bit, when the table
#     pointed d3dx9_24 at d3dx9_43 and nine exports of the aliased-away module
#     were not in it. Requires a reference set per alias, so an unverified alias
#     cannot be added back.
#   - check-pe-module-set: the modules a title imports and whether the bundle
#     serves them (ml812). The shipped ~120 DLLs are the test binaries' imports,
#     so "the game does not start" is usually a LoadLibrary that was never going
#     to succeed. Every shipped module's machine word is checked against its
#     directory, and the manifest is accounted for in three ways: wine modules
#     (the IPA workflow builds them), native modules the DirectX component
#     provides (tools/fetch-directx.sh installs them), and native modules with no
#     source anywhere, which are reported and never failed. --strict (the IPA and
#     PE-module workflows) fails when either of the first two is incomplete.
#   - metal4-inventory: the Metal 4 plan's completeness. DXMT reaches Metal
#     through one flat C table, so docs/metal4/winemetal-metal4-map.tsv is the
#     plan's definition of the work. A slot added to that table without a
#     disposition, or a submodule revision bumped without re-classifying, would
#     leave a port that follows the map reaching for Metal 3 objects through the
#     Metal 4 path -- invisible until a device. Skips when the DXMT submodule is
#     not checked out, like the other gates that need an input a clean clone
#     does not have.
#
# Run from the repo root before tagging/packaging a release. Exits non-zero on
# the first failure.
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

echo "== JIT script sync =="
tools/jit-script-sync.py

echo "== prefix template =="
tools/check-prefix-template.sh

echo "== xcode project =="
tools/check-xcodeproj.py

echo "== swift syntax =="
tools/check-swift-syntax.sh

echo "== swift -> C symbols =="
tools/check-swift-c-symbols.py

echo "== wine PE stamp =="
tools/check-wine-pe-stamp.py

echo "== NLS codepage set =="
python3 tools/check-nls-set.py

echo "== graphics driver pin =="
sh tools/test-graphics-driver-pin.sh

echo "== DirectX DLL aliases =="
python3 tools/check-dll-aliases.py

echo "== shipped PE module set =="
python3 tools/check-pe-module-set.py

echo "== Metal 4 plan completeness =="
python3 tools/metal4-inventory.py

echo "== Metal 4 layer compiles =="
tools/check-metal4.sh

echo "== device capabilities =="
tools/test-device-capabilities.sh

echo "== XInput pad ABI =="
tools/test-xinput-pad.sh

echo "== app layout =="
tools/test-app-ui.sh
