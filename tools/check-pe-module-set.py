#!/usr/bin/env python3
"""Report and gate the Windows PE modules the bundle ships against the manifest.

The committed app/Madeira/arm64ec-windows set was the imports of Wine's own test
executables rather than a compatibility decision, so the common case of "this game
does not start" is a LoadLibrary of a module that was never built.
tools/pe-module-manifest.txt is the set a DirectX-era title actually imports,
derived from Winlator's component definitions, and this gate validates the three
ways a module in it can be satisfied:

  wine      Wine builds it. scripts/build-wine-pe-modules.sh produces it and the
            IPA workflow runs that script before packaging, so a missing one is a
            defect the build can fix. --strict fails on it.
  native    Wine has no implementation at all. Microsoft's DirectX
            redistributable is the only source, installed into
            app/Madeira/x86_64-directx/ by tools/fetch-directx.sh and overlaid on
            the x64 session by WineProcessBridge.m. --strict fails when one of
            these is listed in tools/directx-component.txt and not installed,
            because that means the component step did not run.
  native, not in the component
            No source exists in either place (the DirectPlay service providers,
            wmcodecdspuuid). Always reported, never failed: no step in this repo
            can produce them, and pretending otherwise would just move the
            failure.

On top of the set, every shipped module's machine word is checked against its
directory, because a module in the wrong one of those directories loads in the
session it does not belong to -- silently, since the loader does not check.

Without --strict the exit status covers only the invariants that hold on a
source-only checkout, and the gap is printed so it stays visible in CI instead of
being rediscovered title by title. --strict is what the IPA workflow runs *after*
it has built the modules and fetched the component.
"""

from __future__ import annotations

import argparse
import re
import runpy
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "tools/pe-module-manifest.txt"
COMPONENT = REPO / "tools/directx-component.txt"
ALIASES = REPO / "app/Madeira/DLLAliases.h"
VALIDATOR = REPO / "tools/validate-ios-bundle.py"
BUNDLE = REPO / "app/Madeira"
COMPONENT_DIR = BUNDLE / "x86_64-directx"

# The machine word each directory must carry. arm64ec-windows is 0x8664 because
# ARM64EC images are marked AMD64 -- the hybrid's whole point is that unmodified
# x64 tooling and the loader accept them -- so this is the same check
# tools/validate-ios-bundle.py applies to the modules it knows by name, applied
# here to every file in the directory. The DirectX component is x64 for the same
# reason and gets the same check: it overlays the arm64ec directory in an x64
# session, so an x86 or ARM64 binary in it would never load.
ARCHS = (("arm64ec-windows", 0x8664), ("aarch64-windows", 0xAA64))
COMPONENT_MACHINE = 0x8664

# Modules DXMT builds, plus the XInput set patches/wine-xinput-virtual-pad.patch
# owns. Nothing here may come from the DirectX component: the overlay runs after
# the ARM64EC builtins are linked into system32 and unlinks first, so a
# Microsoft d3d11.dll in the component would replace the Metal backend with a
# D3D11 that has no Metal behind it -- the app would still start and every title
# would render nothing. The same list is the PROTECTED array in
# scripts/build-wine-pe-modules.sh and the by-name check in
# tools/validate-ios-bundle.py; keep them in step.
DXMT_OWNED = frozenset({
    "d3d11", "dxgi", "d3d10core", "winemetal",
    "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
})


def parse_manifest(path: Path, columns: int = 3) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != columns or parts[0] not in ("wine", "native"):
            raise SystemExit(
                f"{path}:{number}: expected `<wine|native> <module> <component>`, got {line!r}")
        entries.append((parts[0], parts[1].lower(), parts[2]))
    if not entries:
        raise SystemExit(f"{path}: no entries")
    return entries


def parse_component(path: Path) -> set[str]:
    names: set[str] = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line.lower().removesuffix(".dll"))
    return names


def parse_alias_names(path: Path) -> set[str]:
    """Modules the bundle serves through DLLAliases.h rather than by shipping them.

    d3dcompiler_44 and d3dcompiler_45 are in the manifest as natives with no
    source, and that is half true: Microsoft never released them, and what the
    bundle does instead is link the name to d3dcompiler_47, whose export set is a
    superset (tools/check-dll-aliases.py verifies that). Reading the table here
    keeps the report honest -- a module served by an alias is not a gap.
    """
    if not path.is_file():
        return set()
    return {name.lower().removesuffix(".dll")
            for name, _ in re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}', path.read_text())}


def shipped(directory: Path) -> dict[str, Path]:
    modules: dict[str, Path] = {}
    for path in directory.iterdir():
        if path.is_file() and path.suffix.lower() == ".dll":
            modules[path.stem.lower()] = path
    return modules


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--strict", action="store_true",
                        help="fail when a wine module is missing, or a native module "
                             "that the component provides is not installed")
    parser.add_argument("--report", action="store_true",
                        help="print the set grouped by the component that asks for it")
    args = parser.parse_args()

    entries = parse_manifest(args.manifest)
    component_names = parse_component(args.component)
    aliases = parse_alias_names(ALIASES)
    validator = runpy.run_path(str(VALIDATOR))

    problems: list[str] = []
    present: dict[str, dict[str, Path]] = {}
    for arch, machine in ARCHS:
        directory = BUNDLE / arch
        if not directory.is_dir():
            raise SystemExit(f"missing bundle directory: {directory}")
        modules = shipped(directory)
        present[arch] = modules
        for stem, path in sorted(modules.items()):
            try:
                validator["pe"](path.read_bytes(), machine)
            except Exception as error:  # noqa: BLE001 - the validator raises ValueError
                problems.append(f"{arch}/{path.name}: {error}")

    component: dict[str, Path] = {}
    if COMPONENT_DIR.is_dir():
        component = shipped(COMPONENT_DIR)
        for stem, path in sorted(component.items()):
            try:
                validator["pe"](path.read_bytes(), COMPONENT_MACHINE)
            except Exception as error:  # noqa: BLE001
                problems.append(f"x86_64-directx/{path.name}: {error}")

    reference = "arm64ec-windows"
    others = [arch for arch, _ in ARCHS if arch != reference]
    # Not conditional on --strict: this is not a gap a later step fills, it is a
    # list that must never name one of these. Checked from the list rather than
    # from the installed files, so it fails at the edit that introduces it.
    stolen = sorted(DXMT_OWNED & component_names)
    if stolen:
        problems.append(
            "tools/directx-component.txt names module(s) DXMT owns: "
            + " ".join(stolen)
            + " -- the overlay would replace them and the Metal backend with them")
    missing_wine = [stem for kind, stem, _ in entries
                    if kind == "wine" and stem not in present[reference]]
    # A native module counts as satisfied when the component installs it. The
    # ones the component does not name have no source in this repo at all.
    native_from_component = [stem for kind, stem, _ in entries
                             if kind == "native" and stem in component_names]
    native_installed = [stem for stem in native_from_component if stem in component]
    native_aliased = [stem for kind, stem, _ in entries
                      if kind == "native" and stem in aliases]
    native_unbuildable = [stem for kind, stem, _ in entries
                          if kind == "native" and stem not in component_names
                          and stem not in aliases]
    # Present for one guest and absent for the other is its own defect: which
    # session a title breaks in then depends on the architecture of its executable,
    # and the failing set is invisible from either directory alone.
    asymmetric = [f"{arch}/{stem}.dll" for kind, stem, _ in entries
                  if kind == "wine" and stem in present[reference]
                  for arch in others if stem not in present[arch]]

    if problems:
        print("check-pe-module-set: FAIL", file=sys.stderr)
        for problem in sorted(set(problems)):
            print(f"  {problem}", file=sys.stderr)
        return 1

    def wrap(label: str, items: list[str]) -> None:
        line = f"  {label}"
        for item in items:
            if len(line) + len(item) + 1 > 100:
                print(line)
                line = "    " + item
            else:
                line += " " + item
        print(line)

    wines = sum(1 for kind, _, _ in entries if kind == "wine")
    natives = sum(1 for kind, _, _ in entries if kind == "native")
    print(f"check-pe-module-set: shipped {len(present[reference])} modules in {reference}, "
          f"{len(present['aarch64-windows'])} in aarch64-windows, "
          f"{len(component)} in x86_64-directx; machine words verified")
    print(f"  gap manifest: {wines} wine modules, "
          f"{wines - len(missing_wine)} present / {len(missing_wine)} still missing")
    print(f"                {natives} native modules, "
          f"{len(native_installed)}/{len(native_from_component)} provided by the "
          f"DirectX component, {len(native_aliased)} served by DLLAliases.h, "
          f"{len(native_unbuildable)} with no source here")
    if missing_wine:
        wrap("wine modules not shipped:", sorted(missing_wine))
    if asymmetric:
        wrap("shipped for one architecture only:", sorted(asymmetric))
    if native_from_component and len(native_installed) != len(native_from_component):
        wrap("component modules not installed:",
             sorted(set(native_from_component) - set(native_installed)))
        wrap("  run:", ["tools/fetch-directx.sh"])
    if native_unbuildable:
        wrap("no source anywhere (documented gap):", sorted(native_unbuildable))

    if args.report:
        by_component: dict[str, list[str]] = {}
        for kind, stem, group in entries:
            satisfied = ((stem in present[reference]) if kind == "wine"
                         else (stem in component or stem in aliases))
            by_component.setdefault(group, []).append(
                f"{stem}{'' if satisfied else '(missing)'}")
        print()
        for group in sorted(by_component):
            print(f"  {group}:")
            wrap("    ", sorted(by_component[group]))

    if args.strict:
        missing_component = sorted(set(native_from_component) - set(native_installed))
        if missing_wine or missing_component:
            print("check-pe-module-set: FAIL --", file=sys.stderr)
            if missing_wine:
                print(f"  {len(missing_wine)} wine module(s) missing from {reference}; "
                      f"run scripts/build-wine-pe-modules.sh", file=sys.stderr)
            if missing_component:
                print(f"  {len(missing_component)} component module(s) not installed; "
                      f"run tools/fetch-directx.sh", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
