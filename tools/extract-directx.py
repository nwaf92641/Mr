#!/usr/bin/env python3
"""Extract the DirectX component DLLs from Microsoft's redistributable.

The DirectX End-User Runtimes redistributable is a self-extracting archive whose
payload is a cabinet of cabinets: one .cab per SDK generation per architecture,
each holding the DLLs, .inf files and a catalog. Everything the component needs is
a few levels in, and the only structure that matters here is the file names, so
this walks the nesting and picks the names listed in tools/directx-component.txt.

Input is the redistributable itself (directx_Jun2010_redist.exe) or an
already-extracted .cab. The redistributable is a self-extracting archive, but it
is not "an MSCF cabinet preceded by a stub": the file carries byte sequences that
read as MSCF without being cabinet headers, and carving at the first one produced
a file that is not a cabinet -- 7-Zip's "Cannot open the file as [Cab] archive".
So the file is handed to the extractor as it stands, because both cabextract and
7-Zip read self-extracting cabinets, and carving is kept only as a fallback and
only at offsets whose header is self-consistent. Nested cab extraction uses
cabextract, which is the only tool that reads Microsoft cabinets everywhere --
macOS has no built-in one and the CI runners have neither 7-Zip nor bsdtar
guaranteed.

Usage:
  python3 tools/extract-directx.py IN --out DIR [--sevenzip none] [--list]

Exits non-zero when a requested name is not in the redistributable, because a
component that silently ships 15 of 16 DLLs is how a title fails on the one that
was missing.
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pe_reader  # noqa: E402  (after sys.path so the script runs from anywhere)

REPO_ROOT = Path(__file__).resolve().parent.parent
COMPONENT = REPO_ROOT / "tools/directx-component.txt"

# The modules this component must never contain. WineProcessBridge.m overlays the
# component onto the x64 session's system32 *after* linking the ARM64EC builtins
# and unlinks first, so a Microsoft d3d11.dll here would replace DXMT's and take
# the Metal backend with it -- silently, because the app would still start. The
# same list is tools/check-pe-module-set.py's DXMT_OWNED and the PROTECTED array
# in scripts/build-wine-pe-modules.sh; keep them in step.
DXMT_OWNED = frozenset({
    "d3d11", "dxgi", "d3d10core", "winemetal",
    "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
})
MSCF = b"MSCF"
MAX_NESTING = 3


def component_names(path: Path) -> list[str]:
    names = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        names.append(line if line.lower().endswith(".dll") else f"{line}.dll")
    if not names:
        raise SystemExit(f"error: {path} lists no modules")
    return names


def which_extractor() -> list[str]:
    for tool in ("cabextract", "7zz", "7z", "bsdtar"):
        found = shutil.which(tool)
        if found:
            return [found]
    raise SystemExit(
        "error: no cabinet extractor found.\n"
        "       Install cabextract (Debian/Ubuntu: apt-get install cabextract;\n"
        "       macOS: brew install cabextract)."
    )


def run(tool: list[str], args: list[str]) -> None:
    result = subprocess.run(tool + args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            f"error: {' '.join(tool + args)} failed ({result.returncode})\n"
            f"{(result.stderr or result.stdout).strip()[:2000]}"
        )


def cabinet_header_ok(data: bytes, index: int) -> bool:
    """Whether a cabinet header at index is self-consistent.

    The redistributable contains MSCF byte sequences that are not cabinet
    headers. Two of them in the June 2010 file claim coffFiles well past the end
    of the file, a spanned-part number in the hundreds and a file count in the
    thousands -- carving there writes a file that no extractor will open, which
    is the failure this check exists to prevent.
    """
    header = data[index : index + 24]
    if len(header) < 24:
        return False
    (cb_cabinet, coff_files, _vmin, _vmaj, _c_folders, c_files, _flags, _set_id,
     _i_cabinet) = struct.unpack("<IIBBHHHHH", header[4:24])
    if c_files == 0:
        return False
    remaining = len(data) - index
    if cb_cabinet > remaining:
        return False
    if cb_cabinet != 0 and coff_files >= cb_cabinet:
        return False
    return True


def candidate_offsets(data: bytes) -> list[int]:
    offsets: list[int] = []
    index = data.find(MSCF)
    while index >= 0:
        if cabinet_header_ok(data, index):
            offsets.append(index)
        index = data.find(MSCF, index + 1)
    return offsets


def extractor_args(tool: list[str], cabinet: Path, destination: Path) -> list[str]:
    if tool[0].endswith("cabextract"):
        return ["-q", "-s", "-d", str(destination), str(cabinet)]
    if tool[0].endswith(("7z", "7zz")):
        return ["x", "-y", f"-o{destination}", str(cabinet)]
    return ["-xf", str(cabinet), "-C", str(destination)]


def extract(cabinet: Path, destination: Path) -> None:
    tool = which_extractor()
    destination.mkdir(parents=True, exist_ok=True)
    run(tool, extractor_args(tool, cabinet, destination))


def try_extract(cabinet: Path, destination: Path) -> bool:
    """Extract if the tool accepts the file, and say whether it produced anything.

    Used to test a candidate rather than to commit to one: an extractor that
    rejects a file returns non-zero, and one that accepts it and yields nothing
    has still not found the payload.
    """
    tool = which_extractor()
    destination.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(tool + extractor_args(tool, cabinet, destination),
                            capture_output=True, text=True)
    if result.returncode != 0:
        return False
    return any(destination.iterdir())


def open_top(source: Path, work: Path) -> Path:
    """Return a directory holding the redistributable's first level.

    The input is tried as it stands first. Preferring it is not an optimisation:
    it is the only approach that does not depend on knowing where a cabinet
    begins in the file, and both extractors read self-extracting archives.

    Carving is the fallback, at offsets whose header is self-consistent, and each
    candidate has to extract to be accepted -- so a wrong offset costs a temp
    directory instead of the whole step.
    """
    direct = work / "direct"
    if try_extract(source, direct):
        return direct

    data = source.read_bytes()
    offsets = candidate_offsets(data)
    for index in offsets:
        carved = work / f"payload-{index}.cab"
        carved.write_bytes(data[index:])
        target = work / f"carved-{index}"
        if try_extract(carved, target):
            print(f"  carved the cabinet at offset {index}")
            return target

    raise SystemExit(
        "error: no cabinet in this redistributable could be opened.\n"
        "       Tried the file as given and every self-consistent MSCF header at "
        f"offset(s): {offsets or 'none found'}.\n"
        "       A file that is not the June 2010 redistributable will land here; "
        "set DIRECTX_URL or DIRECTX_INSTALLER to the right one.")


def flatten_cabinets(root: Path) -> tuple[list[Path], list[Path]]:
    """Expand nested cabinets until only non-cabinet files are left.

    Returns (all extracted files, every directory created) so the caller can
    clean up. Nesting is three deep in the June 2010 redistributable: the outer
    cabinet holds one cabinet per SDK generation per architecture, and those hold
    the DLLs.

    Only the x64 cabinets are descended into. The redistributable ships the same
    file names for both architectures, so recursing into the x86 ones too would
    make "which d3dx9_43.dll did we pick" depend on cabinet order -- and picking
    the x86 one would produce a component that cannot load in an x64 session at
    all. x86 is not a fallback here: the guest is x64 and runs under FEX.
    """
    temps = [root]
    level = [root]
    for _ in range(MAX_NESTING):
        next_level = []
        for directory in level:
            for path in sorted(directory.iterdir()):
                if path.is_dir() or path.suffix.lower() != ".cab":
                    continue
                if directory is not root and "_x64" not in path.stem.lower():
                    continue
                target = Path(tempfile.mkdtemp(prefix="dxcab-"))
                temps.append(target)
                extract(path, target)
                next_level.append(target)
        if not next_level:
            break
        level = next_level

    files = [p for d in temps for p in d.rglob("*") if p.is_file()]
    return files, temps


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path, help="Redistributable .exe or .cab")
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "app/Madeira/x86_64-directx")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--list", action="store_true", help="List what was found and stop")
    args = parser.parse_args()

    if not args.input.is_file():
        raise SystemExit(f"error: {args.input} does not exist")

    wanted = component_names(args.component)
    stolen = sorted(DXMT_OWNED & {name.lower().removesuffix(".dll") for name in wanted})
    if stolen:
        raise SystemExit(
            "error: the component list names module(s) DXMT owns: "
            + " ".join(stolen)
            + "\n  WineProcessBridge.m overlays the component onto the x64 session's"
            "\n  system32 after the ARM64EC builtins, so installing one would replace"
            "\n  DXMT's module -- and the Metal backend with it.")
    work = Path(tempfile.mkdtemp(prefix="directx-"))
    try:
        top = open_top(args.input, work)
        files, _ = flatten_cabinets(top)

        by_name: dict[str, Path] = {}
        for path in files:
            by_name.setdefault(path.name.lower(), path)

        if args.list:
            print(f"{len(by_name)} files in the redistributable")
            for name in wanted:
                print(f"  {'FOUND' if name.lower() in by_name else 'MISSING':8} {name}")
            return 0

        args.out.mkdir(parents=True, exist_ok=True)
        missing: list[str] = []
        wrong_arch: list[str] = []
        installed: list[Path] = []
        for name in wanted:
            path = by_name.get(name.lower())
            if path is None:
                missing.append(name)
                continue
            try:
                machine = pe_reader.load(path).machine
            except pe_reader.PEFormatError as exc:
                wrong_arch.append(f"{name} is not a PE image ({exc})")
                continue
            if machine != 0x8664:
                wrong_arch.append(
                    f"{name} is {pe_reader.MACHINE_NAMES.get(machine, hex(machine))}, "
                    f"not x64")
                continue
            # Lowercased on install. The cabinet spells some of these
            # D3DCompiler_43.dll / XAPO... and the rest of the bundle is lower
            # case; Windows resolves names case-insensitively, but this directory
            # is read by name in a zip archive (tools/validate-ios-bundle.py),
            # where the difference is the difference between a hit and a miss.
            destination = args.out / path.name.lower()
            shutil.copyfile(path, destination)
            installed.append(destination)

        for path in installed:
            print(f"  {path.name:24} {path.stat().st_size / 1e6:6.2f} MB")
        total = sum(p.stat().st_size for p in installed)
        print(f"installed {len(installed)}/{len(wanted)} DirectX modules into {args.out} "
              f"({total / 1e6:.1f} MB)")

        if wrong_arch:
            print(f"error: {len(wrong_arch)} module(s) have the wrong architecture:",
                  file=sys.stderr)
            for problem in wrong_arch:
                print(f"       {problem}", file=sys.stderr)
            return 1
        if missing:
            print(f"error: {len(missing)} requested module(s) are not in the "
                  f"redistributable: {', '.join(missing)}", file=sys.stderr)
            print("       Either the redistributable is older than the name, or the "
                  "name is misspelled in tools/directx-component.txt.", file=sys.stderr)
            return 1
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
