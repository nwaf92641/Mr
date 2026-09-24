#!/usr/bin/env python3
"""Verify the shipped NLS tables match the pinned Wine source.

The port resolves every codepage, casemap, normalisation and sort table through
two paths (build/ntdll-unix/env_ios.c: get_nls_file_path / read_nls_file):

  1. <app resource dir>/nls/<name>.nls          -- the bundle copy
  2. C:\\windows\\system32\\<name>.nls             -- the NT fallback

A table present in neither path makes NtGetNlsSectionPtr return
STATUS_OBJECT_NAME_NOT_FOUND. Nothing degrades and nothing warns; locale setup
stalls and a title that needs its own codepage dies at startup. Four of the
sixty-eight tables reached the bundle for a long time (see ml809), so this
checks the whole set against the source of truth instead of a hard-coded list:

  * wine/nls is the submodule the loader is built from, and it carries the
    tables as committed files. When it is present, its set is the expected set:
    every name must exist in app/Madeira/nls and be byte-identical, so a Wine
    bump that adds or changes a table cannot ship a stale bundle silently.
  * Without a Wine tree (cache-only or prebuilt-libs checkout) the exact
    comparison is skipped, but the absolute floor still applies, because
    "the four always-shipped tables" is exactly the state this exists to
    prevent.

Run from the repo root; wired into tools/check-all.sh.
"""
from __future__ import annotations

import argparse
import filecmp
import os
import sys

# The absolute floor. 68 codepages ship in the pinned Wine; anything near four
# is the bug, and a number this low cannot be a Wine that merely renumbered a
# few tables.
MIN_CODEPAGES = 60

# Tables the loader asks for by name, independent of the codepage set.
REQUIRED = (
    "l_intl.nls",       # NLS_SECTION_CASEMAP
    "locale.nls",       # locale.nls, via read_nls_file
    "normnfc.nls",      # NormalizationC
    "normnfd.nls",      # NormalizationD
    "normnfkc.nls",     # NormalizationKC
    "normnfkd.nls",     # NormalizationKD
    "normidna.nls",     # id 13
    "sortdefault.nls",  # NLS_SECTION_SORTKEYS
)


def tables_in(directory: str) -> dict[str, str]:
    if not os.path.isdir(directory):
        return {}
    return {
        name: os.path.join(directory, name)
        for name in os.listdir(directory)
        if name.endswith(".nls")
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-nls", default="app/Madeira/nls",
                        help="bundle copy of the tables (default: app/Madeira/nls)")
    parser.add_argument("--wine-src", default="wine/nls",
                        help="pinned Wine source tables (default: wine/nls)")
    args = parser.parse_args()

    app = tables_in(args.app_nls)
    if not app:
        print(f"check-nls-set: FAIL -- no .nls tables in {args.app_nls}", file=sys.stderr)
        return 1

    codepages = sorted(n for n in app if n.startswith("c_"))
    problems: list[str] = []

    for name in REQUIRED:
        if name not in app:
            problems.append(f"missing required table {name}")

    src = tables_in(args.wine_src)
    if src:
        missing = sorted(set(src) - set(app))
        extra = sorted(set(app) - set(src))
        differing = sorted(
            n for n in set(src) & set(app)
            if not filecmp.cmp(src[n], app[n], shallow=False)
        )
        src_cp = len([n for n in src if n.startswith("c_")])
        for name in missing:
            problems.append(f"{name} is in {args.wine_src} but not in {args.app_nls}")
        for name in differing:
            problems.append(f"{name} differs from {args.wine_src} (stale bundle copy)")
        # Extra tables are not fatal: a renamed codepage can leave one behind,
        # and shipping one table Wine no longer reads costs a few kilobytes.
        if extra:
            print(f"check-nls-set: note -- {len(extra)} table(s) not in the Wine source: "
                  + ", ".join(extra))
        print(f"check-nls-set: source has {len(src)} table(s) "
              f"({src_cp} codepages), bundle has {len(app)} ({len(codepages)} codepages)")
    else:
        print(f"check-nls-set: {args.wine_src} not present -- skipping the exact "
              f"comparison (cache-only or prebuilt-libs checkout)")

    if len(codepages) < MIN_CODEPAGES:
        problems.append(
            f"only {len(codepages)} codepage table(s) in {args.app_nls}; the pinned "
            f"Wine ships 68 and a missing codepage fails NtGetNlsSectionPtr with "
            f"STATUS_OBJECT_NAME_NOT_FOUND"
        )

    if problems:
        print("check-nls-set: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print("  Stage from the submodule with scripts/stage-nls.sh.", file=sys.stderr)
        return 1

    print(f"check-nls-set: OK -- {len(codepages)} codepages, {len(app)} tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
