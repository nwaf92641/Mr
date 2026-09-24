#!/usr/bin/env python3
"""Fail when the shipped XInput DLLs were built from patches/ that have since changed.

The ten XInput PE DLLs in app/Madeira/{aarch64,arm64ec}-windows/ are committed
build products, like the four DXMT modules, because no CI runner builds them:
the workflow restores native archives from cache and would overwrite a
checked-in DLL with whatever an earlier run produced. scripts/build-wine-xinput-pe.sh
writes app/Madeira/.wine-pe-xinput-stamp -- the sha256 of the patch it built
from -- so that a patch edited without a rebuild is visible.

ml807 is why this exists. The arm64ec DXMT set shipped at a revision older than
every fix in patches/ because a warm cache skipped the rebuild, and nothing in
the tree could tell: the binaries are opaque and the patch was in the diff. A
binary plus the patch that produced it needs a recorded pairing, or the next
person reads the patch and believes the shipped DLLs behave that way.

The patch and the binaries are both in the checkout, so this runs as a
repository gate (tools/check-all.sh) as well as next to the build. What the
shipped bundle contains is a separate question, and
tools/validate-ios-bundle.py answers it: every shipped xinput module must have
the right machine word and carry the pad's unix-call import.

Usage: tools/check-wine-pe-stamp.py
Exit 0 when the DLLs match the patch, 1 when they do not.
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, "patches", "wine-xinput-virtual-pad.patch")
STAMP = os.path.join(ROOT, "app", "Madeira", ".wine-pe-xinput-stamp")


def main():
    try:
        with open(PATCH, "rb") as patch:
            digest = hashlib.sha256(patch.read()).hexdigest()
    except OSError as error:
        print(f"FAILED: cannot read patches/wine-xinput-virtual-pad.patch: {error}", file=sys.stderr)
        return 1

    try:
        with open(STAMP) as stamp:
            recorded = stamp.read().strip()
    except OSError as error:
        print(f"FAILED: cannot read {os.path.relpath(STAMP, ROOT)}: {error}", file=sys.stderr)
        print("        Build the DLLs with scripts/build-wine-xinput-pe.sh and commit its stamp.",
              file=sys.stderr)
        return 1

    if not re.fullmatch(r"[0-9a-f]{64}", recorded):
        print(f"FAILED: {os.path.relpath(STAMP, ROOT)} is not a sha256 digest.", file=sys.stderr)
        print("        scripts/build-wine-xinput-pe.sh writes this file; do not hand-edit it.",
              file=sys.stderr)
        return 1

    if recorded != digest:
        print("FAILED: patches/wine-xinput-virtual-pad.patch changed since the shipped XInput DLLs "
              "were built.", file=sys.stderr)
        print(f"        stamp {recorded}", file=sys.stderr)
        print(f"        patch {digest}", file=sys.stderr)
        print("        Run scripts/build-wine-xinput-pe.sh on an Apple Silicon Mac and commit the "
              "rebuilt", file=sys.stderr)
        print("        app/Madeira/{aarch64,arm64ec}-windows/xinput*.dll together with the stamp.",
              file=sys.stderr)
        return 1

    print("XInput PE stamp matches patches/wine-xinput-virtual-pad.patch.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
