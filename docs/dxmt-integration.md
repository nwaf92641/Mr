# DXMT integration

How DXMT becomes part of the runtime, what is proven at each stage, and what will
have to change for Metal 4 later without changing it now.

The architecture decision is in `docs/graphics-path-study.md`. This file is the
operational half: pins, stages, and the per-file reasoning.

## Provenance, pinned

From `third_party/dxmt.lock`, which the workflows read so the documents and the
build cannot disagree.

| Component | Pin | Licence | Note |
| --- | --- | --- | --- |
| DXMT | `3Shain/dxmt` `7c8dee1c` (2026-09-16) | LGPL-2.1 | owns the graphics path |
| LLVM | 15.0.7, `clang+llvm-15.0.7-arm64-apple-darwin22.0` | Apache-2.0 with LLVM exception | `airconv` links it; DXMT documents no path around it |
| mingw directx headers | `misyltoad/mingw-directx-headers` | see upstream | DXMT's build check requires `include/native/directx/d3d11.h` |
| Wine | not pinned yet | LGPL-2.1-or-later | no build exists, so no pin exists |
| FEX | not pinned yet | MIT | not required for the graphics stages |

Toolchain and platform are recorded by the CI job itself rather than asserted
here, because a document cannot know what a runner had. The values are in the
`dxmt-build-evidence` artifact.

## Evidence from the first successful build

Recorded by the job, copied here from its log. Run
[35664608244](https://github.com/nwaf92641/Mr/actions/runs/35664608244), commit
`33d61f40`, job `DXMT on Apple Silicon`.

| | |
| --- | --- |
| runner | `macos-15`, arm64 |
| OS | macOS 15.7.9 |
| Xcode | 16.4 (build 16F6) |
| Apple clang | 17.0.0 (clang-1700.0.13.5) |
| SDK | 15.5 |
| LLVM | 15.0.7, official arm64 prebuilt |
| `meson compile` | 93 seconds |
| commands | `meson setup build --buildtype release -Dnative_llvm_path=$LLVM_ROOT`, then `meson compile -C build` |

What it produced, every one of them `Mach-O 64-bit dynamically linked shared
library arm64` where it is a library:

| Artifact | What it is |
| --- | --- |
| `src/d3d11/d3d11.dylib` | the D3D11 entry points |
| `src/dxgi/dxgi.dylib` | DXGI |
| `src/d3d10/d3d10core.dylib` | D3D10 core |
| `src/nativemetal/winemetal.dylib` | the Metal layer, and the artifact the job asserts exists |
| `src/dxmt/libdxmt.a` | the D3D11-to-Metal translation |
| `src/dxmt/libdxmt.a.p/dxmt_command.metallib` | DXMT's own Metal shaders, compiled by Apple's Metal compiler |
| `libs/DXBCParser/libDXBCParser{,Native}.a`, `src/airconv/darwin/libairconv.a`, `src/util/libutil.a` | the DXBC parser, the DXBC-to-AIR translator, and shared utilities |

Two things worth reading off that list. The `.metallib` means DXMT's Metal shader
source compiled at build time with the real Metal compiler, so that path is not
merely declared. And `d3d11.dylib` existing is what makes stage 3 possible without
Wine: the D3D11 entry points are now linkable by an ordinary macOS program.

**This is BUILD VERIFIED and nothing more.** Nothing has called
`D3D11CreateDevice`. Nothing has created a `CAMetalLayer`. The runner's GPU is
paravirtual and reports no GPU families, so it could not render if it tried.
`REQUIRES REAL APPLE GPU VALIDATION`.

## Stages, and what each one is allowed to claim

Each stage claims exactly one level and no more. The levels are defined in
`AGENTS.md`.

| Stage | Work | Level it can reach | State |
| --- | --- | --- | --- |
| 1 | DXMT builds on arm64 macOS | BUILD VERIFIED | **done**, run 35664608244 |
| 2 | Wine ARM64EC loads DXMT's path | BUILD VERIFIED, then INSTALL/RUNTIME on hardware | not started |
| 3 | `D3D11CreateDevice` returns a device and context | RUNTIME VERIFIED on a Mac | not started |
| 4 | A triangle through DXMT into a readback buffer | REAL APPLE GPU VERIFIED | not started |
| 5 | Presentation through a `CAMetalLayer` drawable | REAL APPLE GPU VERIFIED | not started, needs Wine's window model |
| 6 | Same stages under iPadOS, from an IPA | INSTALL, then iPadOS VERIFIED | not started |

Stages 3 and 4 are reachable **without Wine**, because DXMT has a native build
mode (`nativemetal`, `-DDXMT_NATIVE=1`) in which `winemetal` is an ordinary dylib.
That matters: it means the D3D11-to-Metal path can be proven on real Apple
hardware before Wine is built, and if the triangle fails, Wine is not a suspect.

Stage 5 cannot be reached without Wine, and the study says why: the drawable comes
from `macdrv`, which means an `HWND` and a Cocoa view.

## Metal 4 preparation: files, and why each one

**None of this is implemented, and none of it should be until stage 4 has passed
on real hardware.** A Metal 4 backend with no Metal 3 baseline is an unmeasurable
claim. Recorded here so the work is a plan rather than a discovery.

`grep -rn 'MTL4\|MTLGPUFamilyMetal4' src/ include/` over DXMT `7c8dee1c` returns
nothing: there is no Metal 4 surface to extend, only one to add.

| FILE | WHY IT NEEDS CHANGE | WHAT WILL CHANGE | WHY THE CHANGE IS REQUIRED |
| --- | --- | --- | --- |
| `src/winemetal/winemetal.h` | thunks exist for Metal 3 objects only; `MTL4*` types have no declarations at all | add `MTL4*` handle types and thunk codes beside the existing ones | a Metal 4 object that is not in the thunk enumeration cannot be reached from the PE side, so nothing above it can use it |
| `src/winemetal/winemetal_thunks.c` | the PE-side boundary is a fixed table; every call above it is a `WINE_UNIX_CALL` | add the forwarding bodies for the new objects | this is where the boundary is implemented, and the boundary is why the objects have handles rather than pointers |
| `src/winemetal/unix/winemetal_unix.c` (3464 lines) | the actual `[MTLDevice ...]` calls live here and nowhere else | implement `MTL4CommandQueue`, `MTL4CommandAllocator`, `MTL4CommandBuffer`, `MTL4CommandEncoder`, `MTL4ArgumentTable`, `MTL4Compiler`, `MTLResidencySet` against Metal 4, keeping the Metal 3 path for devices without it | there is no layer below this file, so it is the only place the new API can be implemented; and the Metal 3 path must survive, because Metal 4 requires `MTLGPUFamilyMetal4` |
| `src/winemetal/Metal.hpp` (1230 lines) | `WMT::` wrappers exist for Metal 3 objects; `src/dxmt/` calls Metal only through them | add wrappers for the new objects with the same handle discipline | without wrappers `src/dxmt/` would either call thunks directly, bypassing its own abstraction, or not use Metal 4 |
| `src/dxmt/dxmt_command_queue.cpp` (237 lines; worker thread, `cmdbuf.commit()`, `waitUntilCompleted()`) | a Metal 3 queue owns its command-buffer memory; Metal 4 moves that to `MTL4CommandAllocator`, and commit takes explicit options | back the queue with `MTL4CommandQueue` where supported and allocate buffers from an allocator | the ownership of command buffer memory is the thing Metal 4 changes, and this file is where that ownership currently lives |
| `src/dxmt/dxmt_command.cpp` (439 lines, 10 Metal-creation sites) | recording uses per-index `setBuffer:`/`setTexture:` binding calls, which are the Metal 3 vocabulary | record into `MTL4CommandEncoder` with argument-table writes, and issue explicit barriers | Metal 4 encoders take an argument table; the per-index calls do not exist in that vocabulary. This is also where hazard tracking disappears, so this file carries the highest correctness risk: a missing barrier is a wrong image, not a slow frame |
| `src/dxmt/dxmt_context.cpp` | Metal 3 tracks hazards implicitly, so DXMT never tracked dependencies itself | hold residency-set membership and barrier decisions at context level | once tracking is implicit no longer, the information has to live somewhere, and the context is where the pass and binding state already is |
| `src/dxmt/dxmt_fence.cpp` | fences and events are the Metal 3 synchronisation primitives | use the Metal 4 equivalents | nothing that relied on implicit tracking can be left as it is; synchronisation is where that reliance is concentrated |
| `src/dxmt/dxmt_presenter.cpp` (with `CAMetalLayer`) | presentation is `nextDrawable` on the borrowed layer | change only if Metal 4's present path requires it, to be determined by reading the API rather than assuming | the frame path ends here, so it must be re-verified; whether it changes at all is unknown and will not be guessed |
| `src/airconv/` and pipeline creation | shaders compile DXBC to AIR and metallib, then compile a render pipeline synchronously | `MTL4Compiler` with `MTL4LibraryFunctionDescriptor`, and `MTL4PipelineDataSetSerializer` for pipeline archives | synchronous pipeline compilation inside a frame is the stutter case; Metal 4's compiler is the API built to move that work off the frame, which is the one performance argument for this work that is not speculative |

Metal 4 also requires `MTLGPUFamilyMetal4`, which `mr_host_pick_backend` already
decides on. The runtime does not have to guess which generation it is on.

## What is deliberately not here

No UI, no launcher, no game library, no EXE importer, no D3D12, no Vulkan, no
Android, no Intel, no Windows ARM64 as a target. `enable_d3d12` and `enable_nvapi`
are left off, which is DXMT's default.
