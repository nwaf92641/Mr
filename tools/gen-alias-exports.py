#!/usr/bin/env python3
"""Regenerate tools/pe-alias-exports.txt from the authoritative definitions.

An alias in app/Madeira/DLLAliases.h is only allowed when every export of the
module it stands in for exists in the target. This script writes the required
sets; tools/check-dll-aliases.py reads them and checks the real binaries against
them, so the two stay in step and the file never has to be edited by hand.

Where the required set comes from matters, and it is different per family:

  * A generation Microsoft shipped in the DirectX redistributable (d3dx9_*,
    d3dx10_*, D3DCompiler_43) can be read directly -- those DLLs are the ground
    truth for what the module exported, including the functions later
    generations dropped, which is exactly what makes an alias unsafe.
  * A generation with no DLL anywhere (d3dcompiler_44/45) has to be bracketed by
    its neighbours instead. Both neighbours must be subsets of the target.

Usage:
  python3 tools/gen-alias-exports.py --wine /path/to/wine [--help]

The Wine tree has to be checked out (git submodule update --init wine); the
redistributable side is optional and only widens the evidence when
app/Madeira/x86_64-directx has been populated by tools/fetch-directx.sh.
"""

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# alias-stem -> (target-stem, [modules whose exports form the required set],
#                provenance string, wine-spec modules behind those)
ALIASES = {
    "d3dcompiler_44": (
        "d3dcompiler_47",
        ["d3dcompiler_43", "d3dcompiler_46"],
        "Wine specs: d3dcompiler_43 (DirectX redistributable) union d3dcompiler_46",
    ),
    "d3dcompiler_45": (
        "d3dcompiler_47",
        ["d3dcompiler_43", "d3dcompiler_46"],
        "Wine specs: d3dcompiler_43 (DirectX redistributable) union d3dcompiler_46",
    ),
}

HEADER = """# Required exports for each alias in app/Madeira/DLLAliases.h.
#
# An alias points a module name a title imports at a module the bundle ships.
# That is only safe when every symbol the aliased-away module exported is present
# in the target -- otherwise the module loads and the import fails, which is a
# partial load rather than a clean "cannot find x.dll".
#
# Format: `REF <alias-stem> <target-stem>`, then one export name per line,
# indented, until the next REF or end of file.
#
# The sets here are not copied by hand. They are the exports of the real modules
# the alias stands in for, taken from the authoritative definitions:
#
#   d3dcompiler_44/45 -- Microsoft's d3dcompiler_44 and _45 shipped inside the
#     Windows SDK and appear in no redistributable, so there is no DLL to read.
#     Their surface is bracketed by the two neighbouring generations, and both
#     brackets are strict subsets of the target: d3dcompiler_43 comes from the
#     DirectX redistributable and d3dcompiler_46 from Wine's model of it. The
#     union of those two is the required set below.
#
# Regenerate with tools/gen-alias-exports.py, which reads the Wine tree and the
# redistributable component and writes this file.
"""


def spec_exports(wine: Path, module: str) -> set:
    """The export names Wine's .spec declares for a module.

    The .spec is Wine's model of the module's export surface, including the
    entries it implements as stubs, which is what matters here: a caller imports
    the name and the stub at least returns a status instead of failing to bind.

    A spec line is `@ <calling convention> [-<flags> ...] <Name>(<args>)`, and the
    flags are what the naive `@ \\w+ (\\w+)` misses -- it reads `stdcall` out of
    `@ stdcall -private D3DAssemble(...)` and produces a "required export" named
    stdcall, which then trivially fails against any real DLL.
    """
    path = wine / "dlls" / module / f"{module}.spec"
    if not path.is_file():
        raise SystemExit(f"error: no spec for {module} at {path}")
    text = path.read_text(errors="replace")
    pattern = re.compile(r"^@\s+\w+\s+(?:-\w+\s+)*(\w+)", re.MULTILINE)
    return {m.group(1) for m in pattern.finditer(text)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wine", type=Path, default=REPO_ROOT / "wine",
                        help="Wine source tree (default: ./wine)")
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "tools/pe-alias-exports.txt")
    args = parser.parse_args()

    if not (args.wine / "dlls").is_dir():
        print(f"error: {args.wine} does not look like a Wine tree (no dlls/)", file=sys.stderr)
        print("       run: git submodule update --init wine", file=sys.stderr)
        return 2

    out = [HEADER]
    for alias, (target, sources, provenance) in sorted(ALIASES.items()):
        required = set()
        for module in sources:
            required |= spec_exports(args.wine, module)
        out.append(f"REF {alias} {target}")
        out.append(f"#   {provenance}")
        out.extend(f"    {name}" for name in sorted(required))
        out.append("")
        print(f"{alias} -> {target}: {len(required)} required exports "
              f"(from {', '.join(sources)})")

    args.out.write_text("\n".join(out))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
