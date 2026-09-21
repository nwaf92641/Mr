# patches/dxmt

Patches against the DXMT commit pinned in `../../third_party/dxmt.lock`.

**This directory is empty on purpose.** No patch exists until a build fails or a
capability is missing, and inventing one beforehand would be a guess about
upstream dressed up as a decision.

## Rules

1. **Patches here carry LGPL-2.1**, not this project's MIT. A patch is a
   modification of DXMT, and DXMT is LGPL-2.1. Each patch header says so.
2. **Written against upstream**, with the upstream commit named in the header.
   Nothing is copied from Madeira: its patches are GPL-3.0 code, and copying one
   would relicense this work.
3. Every patch records:
   - `FILE` -- what it touches.
   - `WHY IT NEEDS CHANGE` -- the failure or missing capability, as observed.
   - `WHAT WILL CHANGE` -- the edit, in a sentence.
   - `WHY THE CHANGE IS REQUIRED` -- why upstream's behaviour cannot be used as
     is, and what upstream does about it, checked before writing anything new.

A patch is a last resort. Upstream first, configuration second, patch third.

## Metal 4 will land here

When Metal 3 is proven on a real Apple GPU and the comparison becomes meaningful,
the Metal 4 work arrives as a patch series in this directory. The files it will
touch, and the reason for each, are listed in `docs/graphics-path-study.md`
section 7. None of it is written yet, and none of it should be until the Metal 3
baseline exists to compare against: a Metal 4 backend with nothing to compare
against is an unmeasurable claim.
