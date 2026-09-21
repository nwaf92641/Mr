# Mr — repository notes for future sessions

Mr is an Apple Silicon runtime for Windows x86-64 games that use Direct3D 11.
The intended path is:

    x86-64 EXE -> FEX-Emu JIT -> ARM64 -> Wine ARM64EC -> DXMT -> Metal 4/3 -> GPU

Read `docs/ipados-support-matrix.md` before believing anything about what works.
It separates implemented-and-tested from implemented-but-unverified-on-device
from not-implemented, and that distinction is the point of the document.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure

# Sanitizers, which is how the core is normally checked
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMR_ENABLE_ASAN=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

The suite is a single CTest test, `mr_tests`, built from `tests/c/test_mr.c`. It
prints `N checks, M failures` and exits non-zero on any failure.

Development happens on x86-64 Linux. That host builds the portable core, the
analyzer, the cache, the profiles and the CLI, and runs the whole test suite.
It deliberately reports `gpu_available = false`, so the planner refuses to produce
a runnable plan — that refusal is correct behaviour, not a bug to work around.

## Invariants that are easy to break

- **The portable core must not include Metal, FEX, Wine or DXMT headers**
  (`runtime/include/mr/`, `runtime/core/`, `analyzer/`). That is what keeps the
  decisions testable off-device. The only Objective-C in the project is
  `runtime/platform/apple/mr_metal_probe.m`.
- **`-Werror` with `-Wconversion -Wsign-conversion` is on.** Casts are required,
  not optional.
- **The duplicate-key check in `mr_profile.c` is per file, not per profile.** It
  used to compare against every key already loaded, which made a saved profile
  unloadable the moment the caller seeded a default that a file also set. Import,
  save, relaunch is the whole user flow: a regression here breaks everything.
- **`mr_pe.is_truncated` is separate from `!is_valid`.** A truncated download
  parses as a valid image whose sections are missing; the analyzer and the
  planner both have to reject it for the specific reason.
- **`needs_translation` is not `uses_fex`.** Guest-needs-translation and
  host-can-translate are different questions; conflating them produced a blocker
  meant for devices that cannot run translated code.
- **Do not introduce a graphics/emulation component that the ABI in
  `runtime/include/mr/mr_backend.h` does not describe.** That header is shaped by
  Metal 4 on purpose and documents the Metal 3 emulation mapping.

## What is not implemented

- `metal/mr_backend_metal3.m` and `metal4/mr_backend_metal4.m` — the graphics
  backends. Their absence is reported by CMake at configure time, and
  `mr_gfx_backend_create` returns NULL with a reason that distinguishes
  "not written yet" from "built on the wrong machine".
- FEX-Emu, Wine and DXMT integration. `tools/fetch-components.sh` fetches the
  sources into `third_party/src/` (git-ignored); nothing links them yet.
- `app/` (the UI).

`MR_NO_APPLE_GFX_BACKEND` is what keeps `mr_gfx_backend_create_apple` out of the
link on Apple when no backend source exists. The flag and the source paths are
set together in the top-level `CMakeLists.txt` so they cannot disagree.

## Verifying user-facing output

The CLI is `build/runtime/mrctl`. Commands: `analyze`, `profile`, `plan`,
`compat`, `cache`, `host`. Global options go before the command:

```sh
./build/runtime/mrctl --root /tmp/mr-demo host
./build/runtime/mrctl --root /tmp/mr-demo profile some/game.exe
./build/runtime/mrctl --root /tmp/mr-demo plan some/game.exe
```

After changing the analyzer or the planner, run `profile` and then `plan` on the
same executable. That round trip is the app's main flow and it is not covered by
any single unit test.

## Verifying Objective-C on a host with no Apple SDK

This host cannot compile Objective-C, so `metal/` and `tests/metal/` are
otherwise compiled exactly nowhere:

```sh
tools/check-objc.sh
```

It runs `clang -fsyntax-only` against the stubs in `tools/objc-stub-sdk/`, twice
per file (macOS and iOS) plus a third pass for the backend with
`OS_OBJECT_USE_OBJC=0`. Read that directory's README before trusting a result:
it proves the ObjC is well-formed and that every vtable assignment matches
`mr_backend.h`, and it proves nothing about whether the stubs match Apple's
headers. It found six real bugs on first run, including
`MTLResourceOptionsStorageModeShared` (not an Apple name) and `id<CAMetalLayer>`
(a class used as a protocol), so it is worth running before every push that
touches those files.

## CI, and what a green run means

`.github/workflows/apple-silicon.yml` runs on an arm64 macOS runner. It compiles
and links the Objective-C backend against Apple's real SDK, which is the thing
`check-objc.sh` cannot do.

**A green run proves the Apple platform layer builds. It does not prove it
draws.** GitHub's arm64 runners expose a paravirtual Metal device that reports no
GPU families, so `mr_metal_test` cannot answer any of its questions there. It
exits 77, CTest reports it as skipped through `SKIP_RETURN_CODE`, and the workflow
turns 77 into a `::warning::` annotation reading "Metal frame path unverified".
That annotation is the honest summary of every green run: built and linked, frame
path not exercised.

Do not remove that warning to make a run look cleaner, and do not read a green
check as evidence that the frame path works. Verifying it needs a Mac or an iPad.

Two notes for whoever touches this next. Workflows are only registered for a
repository once one exists on the default branch, so the first Apple run needed
this file on `main` before anything on a branch was picked up. And the runner's
device is worth knowing about for another reason: the probe reports "macOS Apple
Paravirtual device, gpu family 0", which independently reproduces what Madeira
measured about `AppleParavirtDevice`.

## Madeira, and the licence boundary

`docs/madeira-analysis.md` is the study of `nwaf92641/Madeira`, which is the
closest existing thing to this project. Read it before designing anything that
touches FEX, Wine, DXMT or the iOS JIT.

**Madeira is GPL-3.0-or-later and Mr is MIT, so no Madeira code may be copied.**
Its architecture, its findings about Apple's APIs and its build recipe are
knowledge, not code, and are fine to use.
