#!/usr/bin/env python3
"""Gate the DirectX module aliases in app/Madeira/DLLAliases.h.

The alias table is a claim about what the bundle can serve: "the name a title
imports resolves to a module that is present in the session's system32, and that
module exports everything the name was supposed to export". Every way that claim
can be false is silent at runtime, so all of them are checked here instead.

  1. Internal consistency: no duplicate alias, none pointing at itself, none
     whose target is itself an alias (a chain would depend on iteration order).

  2. Every alias target must be a module this bundle ships, for the architecture
     the x64 session uses (arm64ec-windows) and for the native one
     (aarch64-windows). WineProcessBridge.m links into system32 from whichever
     the session selected, so a target missing from either is a target that is
     absent for somebody.

  3. No alias name may be a module this bundle ships, will build (the wine rows
     of tools/pe-module-manifest.txt), or install as a component
     (tools/directx-component.txt). A real implementation always wins over the
     symlink, so such an entry is a comment pretending to be a mechanism -- and
     the older the entry, the more likely it is masking a module that was
     supposed to be built.

  4. Every alias must have a reference set in tools/pe-alias-exports.txt, and
     every export in it must exist in the target's real export table. This is the
     check that matters: the table this replaces pointed d3dx9_24 at d3dx9_43,
     and Microsoft's d3dx9_24 exports D3DXCreateFragmentLinker and
     D3DXGatherFragments{,FromFileA,...} which d3dx9_43 does not. Nothing caught
     it, so a title importing d3dx9_24 loaded the module and then failed at the
     import. An alias with no recorded reference set fails here, which is what
     stops an unverified alias being added back.

Exit status is 0 when every property holds.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pe_reader  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
TABLE = REPO / "app/Madeira/DLLAliases.h"
REFERENCES = REPO / "tools/pe-alias-exports.txt"
MANIFEST = REPO / "tools/pe-module-manifest.txt"
COMPONENT = REPO / "tools/directx-component.txt"

# Which bundle directory the runtime copies into system32 depends on the guest,
# and the two are not the same set. `arm64ec-windows` is the set every real game
# uses -- WineProcessBridge.m selects it for an x64 target, and an x64 executable
# is what a DirectX title is. `aarch64-windows` serves the native ARM64 session;
# the asymmetry between them is reported by tools/check-pe-module-set.py against
# the manifest rather than failed here, because an alias cannot fix a module the
# bundle does not build.
TARGET_ARCH = "arm64ec-windows"
SHADOW_ARCHS = ("arm64ec-windows", "aarch64-windows")

# `{ "alias.dll", "target.dll" },`
ROW = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}')
REF = re.compile(r"^REF\s+(\S+)\s+(\S+)\s*$")


def parse_table(text: str) -> list[tuple[str, str]]:
    body = text.split("kMadeiraDLLAliases[] = {", 1)
    if len(body) != 2:
        raise SystemExit(f"{TABLE}: no kMadeiraDLLAliases[] initializer")
    return ROW.findall(body[1].split("};", 1)[0])


def parse_references(path: Path) -> dict[str, tuple[str, set[str]]]:
    """{alias-stem: (target-stem, required exports)} from the reference file."""
    if not path.is_file():
        raise SystemExit(f"{path}: missing; run tools/gen-alias-exports.py")
    references: dict[str, tuple[str, set[str]]] = {}
    current: str | None = None
    target = ""
    required: set[str] = set()
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            if current is not None and not raw.lstrip().startswith("#"):
                references[current] = (target, required)
                current, required = None, set()
            continue
        match = REF.match(line.strip())
        if match:
            if current is not None:
                references[current] = (target, required)
            current, target, required = match.group(1).lower(), match.group(2).lower(), set()
            continue
        if current is not None:
            required.add(line.strip())
    if current is not None:
        references[current] = (target, required)
    return references


def parse_stems(path: Path, column: int) -> set[str]:
    """Stems from a `<kind> <module> <component>` manifest."""
    stems = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) > column:
            stems.add(fields[column].lower())
    return stems


def parse_component(path: Path) -> set[str]:
    names = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line.lower())
    return names


def shipped(arch: str) -> dict[str, Path]:
    directory = REPO / "app/Madeira" / arch
    if not directory.is_dir():
        raise SystemExit(f"missing bundle directory: {directory}")
    return {p.name.lower(): p for p in directory.iterdir()
            if p.is_file() and p.suffix.lower() == ".dll"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", type=Path, default=TABLE)
    parser.add_argument("--references", type=Path, default=REFERENCES)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()

    rows = parse_table(args.table.read_text())
    if not rows:
        print("check-dll-aliases: FAIL -- table is empty", file=sys.stderr)
        return 1

    problems: list[str] = []
    names = [name.lower() for name, _ in rows]
    targets = [target.lower() for _, target in rows]

    # 1. internal consistency
    seen: set[str] = set()
    for name in names:
        if name in seen:
            problems.append(f"{name}: listed twice")
        seen.add(name)
    for name, target in zip(names, targets):
        if name == target:
            problems.append(f"{name}: aliases itself")
    name_set = set(names)
    for name, target in zip(names, targets):
        if target in name_set:
            problems.append(f"{name}: target {target} is itself an alias")

    # 2. the runtime claim, per architecture
    per_arch = {arch: shipped(arch) for arch in SHADOW_ARCHS}
    targets_present = per_arch[TARGET_ARCH]
    for name, target in zip(names, targets):
        if target not in targets_present:
            problems.append(f"{name}: target {target} is not shipped in {TARGET_ARCH}")

    # 3. no alias may shadow anything the bundle ships, builds or installs
    will_ship = parse_stems(args.manifest, 1) | parse_component(args.component)
    for arch, present in per_arch.items():
        for name in names:
            if name in present:
                problems.append(
                    f"{name}: alias would shadow a module the bundle ships in {arch}")
    for name in names:
        if name in will_ship:
            problems.append(
                f"{name}: alias shadows a module the build is supposed to produce "
                f"(it is in the manifest or the DirectX component); remove the alias "
                f"or the module")

    # 4. the exports, against the real binaries
    references = parse_references(args.references)
    for name, target in zip(names, targets):
        stem = name[:-4] if name.endswith(".dll") else name
        reference = references.get(stem)
        if reference is None:
            problems.append(
                f"{name}: no reference set in {args.references.name}, so the alias is "
                f"unverified; run tools/gen-alias-exports.py")
            continue
        reference_target, required = reference
        if reference_target != (target[:-4] if target.endswith(".dll") else target):
            problems.append(
                f"{name}: table points at {target} but the reference set is for "
                f"{reference_target}.dll")
            continue
        if not required:
            problems.append(f"{name}: reference set is empty")
            continue
        try:
            exports = pe_reader.load(targets_present[target]).exports()
        except pe_reader.PEFormatError as exc:
            problems.append(f"{name}: cannot read exports of {target}: {exc}")
            continue
        missing = sorted(required - exports)
        if missing:
            problems.append(
                f"{name}: {len(missing)} export(s) of the aliased-away module are not "
                f"in {target}: {', '.join(missing[:6])}"
                f"{' ...' if len(missing) > 6 else ''}")

    if problems:
        print("check-dll-aliases: FAIL", file=sys.stderr)
        for problem in sorted(set(problems)):
            print(f"  {problem}", file=sys.stderr)
        return 1

    checked = sum(len(references[name[:-4] if name.endswith('.dll') else name][1])
                  for name, _ in rows)
    print(f"check-dll-aliases: OK -- {len(rows)} aliases, {checked} exports verified "
          f"against {TARGET_ARCH}, "
          f"aarch64-windows={len(per_arch['aarch64-windows'])} modules")
    return 0


if __name__ == "__main__":
    sys.exit(main())
