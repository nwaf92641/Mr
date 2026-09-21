# What runs on iPadOS, and what does not

This is the honest answer to the question the project brief asks directly: for
each layer, say whether it works on a non-jailbroken iPad, needs a Mac, needs
JIT, needs an entitlement, or is simply not written yet.

Two rules produced this document. A layer is only called "works on iPadOS" if it
is built from public API and does not require a privilege a third-party app
cannot hold. And a layer that is not implemented is listed as not implemented,
with the file that would fill it, rather than described as planned work that
reads like a feature.

## The matrix

| Layer | State | Runs on iPadOS | Needs JIT | Needs an entitlement | Needs a Mac build |
|---|---|---|---|---|---|
| PE analyzer (`analyzer/`) | implemented, tested | yes | no | no | no |
| Launch planner (`runtime/core/mr_launch.c`) | implemented, tested | yes | no | no | no |
| Shader / pipeline cache (`runtime/core/mr_cache.c`) | implemented, tested | yes | no | no | no |
| Profiles (`runtime/core/mr_profile.c`) | implemented, tested | yes | no | no | no |
| Compatibility DB (`runtime/core/mr_compat.c`) | implemented, tested | yes | no | no | no |
| Host probe, Apple (`runtime/platform/apple/mr_host_apple.c`) | implemented, unverified on device | yes | no | no | yes, to compile |
| Metal capability probe (`runtime/platform/apple/mr_metal_probe.m`) | implemented, unverified on device | yes | no | no | yes, to compile |
| Metal 3 backend (`metal/mr_backend_metal3.m`) | **not implemented** | yes, once written | no | no | yes |
| Metal 4 backend (`metal4/mr_backend_metal4.m`) | **not implemented** | yes, once written | no | no | yes |
| FEX-Emu JIT (`third_party/src/fex/`) | **not integrated** | needs JIT | **yes** | **yes** | yes |
| Wine ARM64EC (`third_party/src/wine/`) | **not integrated** | yes, as a library | no | no | yes |
| DXMT (`third_party/src/dxmt/`) | **not integrated** | yes, as a library | no | no | yes |
| UI (`app/`) | **not implemented** | yes | no | no | yes |

The third-party sources are not in this tree. `tools/fetch-components.sh` fetches
them into `third_party/src/`, which is ignored by git, and it reports the revision
it actually obtained rather than one it assumed. Wine is LGPL: it stays a
separate library this runtime links against, it is not merged into the tree.

"Unverified on device" is a real category and not a hedge: the two Apple files
were written against Apple's published documentation, and no machine in the
development environment can compile Objective-C or link Metal. They are the first
things to build and test on a Mac.

## JIT is the gating item

Everything above except FEX-Emu runs on iPadOS without special permission. FEX-Emu
is the exception, and it is the layer that translates the game's x86-64 into
ARM64, so a build without it can analyse and plan but cannot run.

Executing code a process just wrote requires two things on iOS-derived systems:

- the memory must be mapped through the JIT path (`MAP_JIT`, and the
  `pthread_jit_write_protect_np` toggle around writes), and
- the process must hold the JIT entitlement, which is granted per app by Apple
  rather than being available to any signed app.

The project does not assume the entitlement. `mr_host_jit_selftest()`
(`runtime/core/mr_host.c`) maps a page, writes a stub that returns 42, remaps it
executable and calls it, and reports whether the answer came back. That is
deliberate: on iPadOS the debug flag can be set with no debugger attached, the
allocation then succeeds and returns zero, and the failure surfaces much later as
a placement complaint that has nothing to do with the cause. The planner reads
the self-test result, not a flag, and refuses the launch when FEX cannot be given
executable memory.

## What cannot be embedded in the app

- **Microsoft's runtime DLLs.** Visual C++ redistributables are not
  redistributable in an App Store bundle. When the analyzer finds a vcruntime
  dependency it reports the requirement and says so; it does not bundle a copy.
- **The games.** The app imports a game the user already has.
- **A jailbreak.** Nothing here assumes one, and the design avoids what would
  need it: `wineserver` runs as a thread of the main process because a
  third-party app cannot `fork`/`exec`, and Wine and FEX ship as libraries inside
  the bundle because the sandbox will not let the runtime load them from
  anywhere else.

## What needs a Mac

- Compiling anything Objective-C or linking Metal: both Apple probes, both
  backends.
- Signing the bundle, including the embedded Wine, FEX and DXMT binaries. They
  are separate Mach-O images inside the app and each has to carry a signature
  that matches the app's.
- Producing a `.metallib` from DXMT's shader sources. Metal libraries are built
  by Apple's toolchain, and precompiled shader archives cannot be produced
  off-Mac.

## Storage

iPadOS will not let the app write outside its container, and game libraries run
to tens of gigabytes. The shader cache, the FEX code cache and the profiles all
live under the container; the game library is read from a location the user grants
access to, which is why the runtime keeps only absolute paths it was given rather
than assuming a layout.

## What this document does not claim

It does not claim any game runs. The compatibility database
(`runtime/core/mr_compat.c`) exists to record what has been observed working, and
its rule for an unlisted title is "unknown", which the CLI prints as *untested,
not supported*: no configuration in this release is known to make an unlisted
title work. The first honest milestone is one DirectX 11 title that the analyzer
classifies, the planner accepts, and the GPU draws.
