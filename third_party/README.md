# third_party

Pins, and the policy for everything this repository depends on but does not own.

`dxmt.lock` is the single source of truth for versions. The workflows source it
rather than repeating version numbers, so a document and a build cannot disagree
about what was tested.

## Nothing here is vendored

Every component is fetched at build time into a directory outside the source tree
(`$RUNNER_TEMP` in CI). Nothing is copied into this repository, and that is a
licence decision rather than tidiness:

| Component | Licence | What that means here |
| --- | --- | --- |
| DXMT | LGPL-2.1 | may be linked and redistributed; a patched DXMT is a modification of DXMT and its patches carry LGPL-2.1, not this project's MIT |
| Wine | LGPL-2.1-or-later | may be linked; may not be swallowed into this tree |
| FEX | MIT | may be vendored, when there is something to vendor |
| Madeira | GPL-3.0-or-later | **read-only**. Architecture and findings are knowledge and are fine to use; its code and its patch text are not |

Mr itself is MIT. Using a GPL-3.0 patch text from Madeira would relicense this
work, so every patch in `../patches/` gets written against upstream, with the
problem and the upstream check recorded, and none of them are copied.

## Adding a component

1. Pin the exact commit in `dxmt.lock`, with the date and the licence.
2. State the licence in the table above before the first build that uses it.
3. If it needs a patch, put the patch in `../patches/<component>/` with an
   LGPL-2.1 header where the component is LGPL, and record in the patch header
   what the problem is and what upstream does about it.
4. Never vendor it into this tree to make a build simpler.
