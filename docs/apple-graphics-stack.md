# The Apple graphics stack, and what each part is for

```
Windows x86-64 EXE
  → FEX JIT                  x86-64 instructions into arm64 at run time
  → ARM64 / ARM64EC
  → Wine ARM64EC             Windows API, PE loading, a window
  → D3D11                    the game's own calls, unchanged
  → DXMT                     D3D11 translation, DXBC shaders via airconv
  → winemetal                DXMT's thin Metal layer
  → Metal 3 / Metal 4        Apple's GPU API
  → Apple GPU
```

Everything below is placed on that path, and nothing is included because it
exists. A technology with no role on this path is named as such, with the reason,
in the section that says so.

One structural decision holds the whole document together: **Metal 4 belongs
inside DXMT, not in a Mr backend.** `mr_backend_metal3.m` is a conformance and
test harness, not the frame path; the frame path is Wine → winemetal → DXMT. So
there is no `mr_backend_metal4.m`, and there must not be one: D3D11 semantics --
resource states, the deferred context, the way a D3D11 texture maps to an MTL
resource -- live in DXMT, and a Metal 4 layer written outside DXMT would have to
re-derive every one of them. See `docs/graphics-path-study.md` for the file-level
argument.

## Where each decision is taken

| Decision | Taken in | Why there |
| --- | --- | --- |
| Which backend this machine can use | `mr_host_pick_backend`, `runtime/core/mr_host.c` | one place that reads capability and nothing else, testable off-device |
| What the machine can do | `mr_metal_probe.m`, `mr_metalfx_probe.m` | the only Objective-C, kept out of the portable core |
| Metal 4 versus Metal 3 inside the frame path | DXMT | it owns the D3D11 semantics the choice has to preserve |
| Shader translation | DXMT's `airconv` | it already consumes the DXBC that D3D11 produces |

## The Apple technologies, one at a time

### Metal 3

- **Where:** the compatibility half of DXMT's Metal layer, through winemetal.
- **Build-time or runtime:** runtime. No build-time counterpart.
- **macOS and iPadOS:** both. Apple documents Metal 3 from iOS/iPadOS 16.
- **Needs Metal 4?** No. It is the fallback when Metal 4 is absent.
- **Needs Apple Silicon?** Effectively yes for this project. The device has to be
  an Apple GPU family device; a paravirtual device is not usable.
- **Optional?** No. It is the floor: without it there is no frame path.
- **Benefit to the DX11 path:** it is the path, on any device without Metal 4. A
  DX11 game cannot tell the difference, which is the point of putting it here.

### Metal 4

- **Where:** inside DXMT and winemetal, as an alternative to their Metal 3 code
  paths. **Not** a Mr backend, and not a separate translation layer.
- **Build-time or runtime:** runtime entirely. The API is used from the shipped
  binary.
- **macOS and iPadOS:** both, from macOS 26 / iPadOS 26.
- **Needs Metal 4?** It is Metal 4.
- **Needs Apple Silicon?** It needs an Apple GPU that reports the Metal 4
  family. Per the note in `mr_metal_probe.m`, Apple's support list puts Metal 4
  on A14 and later, which includes every Apple Silicon Mac.
- **Optional?** Yes, and this is the load-bearing sentence: Metal 4 is the fast
  path, Metal 3 is the fallback, and a device with neither is unsupported rather
  than degraded.
- **Benefit to the DX11 path:** the parts that remove CPU work per frame --
  explicit command allocators instead of per-frame allocation, argument tables
  instead of per-draw binding calls, residency sets instead of per-resource
  tracking by the driver. Each of those is a cost DXMT pays today on the Metal 3
  path. None of them changes what the game sees.
- **What is implemented:** nothing yet, and that is deliberate. The seam is
  designed and the capability layer decides between the two paths, but a wrapper
  added only to have one would be a fake. The user-facing rule for this project
  is that Metal 4 is not "working" until there is runtime evidence on a device
  that reports Metal 4, and there is no such device in CI.

### Metal Shader Converter

- **Where:** nowhere on the DX11 shader path, and the reason is specific.
- **What it is:** Apple's tool and dynamic library (`metal-shaderconverter`,
  `libmetalirconverter`) that converts **DXIL** into Metal bytecode,
  via LLVM IR. Apple documents it as requiring macOS 13 and Xcode 15 or later,
  with a separate Windows build. There is no iOS/iPadOS build.
- **Why it is not on our path:** it consumes DXIL. D3D11 shaders are **DXBC**.
  DXIL is the D3D12-era container, and DXMT reaches DXIL only on its D3D12 path,
  which this project does not implement. Adding Metal Shader Converter to the
  DX11 path would require a D3D11→DXIL step that does not exist, and the correct
  answer to "should we adopt it" is not yet.
- **Where it would be relevant:** a future DX12 path. Recorded here so the next
  person does not rediscover it, and so nobody adds it to the DX11 path by
  reflex.
- **Build-time or runtime:** both, in Apple's design: CLI for asset cooking,
  library for run-time compilation. Neither is available on iPadOS, so on this
  project it could only ever be a build-time macOS tool.
- **Licensing and distribution:** Apple's downloads carry Apple's terms, and
  whether a converted `.metallib` may be shipped inside a distributed app is a
  question to answer before shipping one. Nothing here ships one.

### Apple GPU capability detection

- **Where:** `mr_metal_probe.m` (device facts) and `mr_metalfx_probe.m` (MetalFX),
  read into `mr_host_caps` and turned into a recommendation by
  `mr_host_pick_backend` and `mr_host_describe_graphics`.
- **Build-time or runtime:** runtime. The probe is the only reliable answer.
- **macOS and iPadOS:** both, with the APIs guarded for availability.
- **Needs Metal 4?** No; it answers that question.
- **Optional?** No. Every adaptive decision reads from it.
- **Benefit to the DX11 path:** it is the difference between choosing the Metal 4
  path and guessing at it, and it is what stops a paravirtual device from being
  reported as verified hardware. Capabilities are never inferred from a chip
  name: "M4" does not imply "Metal 4", a device that answers the Metal 4 query
  implies it.

### MetalFX

- **Where:** an optional enhancement around the finished frame. Never inside the
  DX11 translation, and never required.
- **What is documented:** `MTLFXSpatialScalerDescriptor.supportsDevice:`,
  `MTLFXTemporalScalerDescriptor.supportsDevice:` and
  `MTLFXTemporalDenoisedScalerDescriptor.supportsDevice:`, all class methods,
  from iOS/iPadOS 16 and macOS 13. MetalFX also has Metal 4 variants
  (`MTL4FXSpatialScaler`, `MTL4FXTemporalScaler`, `MTL4FXFrameInterpolator`).
- **What is implemented:** detection only. `mr_metalfx_probe.m` asks each of the
  three availability selectors and records the answers in `mr_host_caps`. No
  effect is created, and nothing in the frame path consults the result.
- **Frame interpolation:** deliberately not queried. The class exists, its
  availability query was not confirmed against Apple's documentation, and a
  capability flag guessed from a class name is worse than an absent one.
- **macOS and iPadOS:** both.
- **Optional?** Yes, and that is the whole point of it being a separate flag.
- **Benefit to the DX11 path:** a game rendering at a lower internal resolution
  can be upscaled on the GPU instead of by the CPU, when the device supports it
  and when the user asks for it. It changes the presentation of the frame, not
  the frame.

### Apple Game Porting Toolkit

- **Where:** a technical reference. Not a dependency, not a component, not
  copied into this repository.
- **Why it cannot be a component:** it is a macOS toolkit around D3DMetal, which
  is a different D3D implementation with its own licence and no iPadOS build. It
  is not a runtime for running an EXE on iPadOS.
- **What is taken from it:** the shape of the problems, and the public
  documentation of Apple's own techniques. The shader-compilation guidance in it
  is what established that Metal Shader Converter consumes DXIL, which is the
  reason it is not on our path.

### FEX-Emu and Wine

Covered in `docs/jit-requirements.md` and to be pinned in `third_party/`. On this
path FEX is the translation stage and Wine is the Windows API stage; neither is a
graphics backend, and neither chooses a Metal version.

## Build-time Apple tools versus iPadOS runtime components

The rule this project holds to: **a macOS build tool is not an iPadOS runtime
component, and nothing built for one may be assumed to run on the other.**

| | Build-time, macOS | Runtime, iPadOS |
| --- | --- | --- |
| Xcode, clang, the iOS SDK | required | not present |
| `metal`, `metallib`, `xcrun` | required, they build the shaders and the dylibs | not present |
| Metal Shader Converter | usable in principle, only if a DXIL path ever exists | unavailable, no Apple build for it |
| Metal / MetalFX frameworks | headers and link-time names | the frameworks themselves, at run time |
| LLVM 15 (for DXMT's airconv) | required to build DXMT | whatever ships inside the app |
| DXMT, winemetal, Wine, FEX | not needed | required, as built binaries and libraries |

The LLVM row is not hypothetical. DXMT's native build links against LLVM's
`libc++` rather than the system one, as recorded in `docs/dxmt-integration.md`,
which CI resolves with a load path. Inside an app there is no load path into a
build directory, so an iPadOS build has to ship that library or build DXMT
against the system one. That decision belongs to the packaging stage, and it is
written down because it will not announce itself until a dyld error does.

## What is claimed today

| Layer | State |
| --- | --- |
| Capability detection | BUILD VERIFIED, and the portable half is covered by `mr_tests` |
| Backend choice, Metal 4 → Metal 3 → unsupported | BUILD VERIFIED, tested off-device with fabricated hosts |
| MetalFX detection | BUILD VERIFIED (compiles and links); never run against a device with MetalFX |
| DXMT native build on arm64 | BUILD VERIFIED, CI, with `d3d11.dylib` and `winemetal.dylib` produced |
| D3D11 device, clear, read-back | RUNTIME UNVERIFIED. Built; not run, because the runner has no usable GPU |
| Metal 3 frame path | RUNTIME UNVERIFIED |
| Metal 4 frame path | NOT IMPLEMENTED, seam designed |
| Wine ARM64EC | NOT STARTED |
| FEX | NOT STARTED |
| iPadOS / IPA | NOT STARTED |

`REQUIRES REAL APPLE GPU VALIDATION` is the honest state of every row marked
unverified, and the CI runner is not hardware: it reports
`device_name=Apple Paravirtual device`, `gpu_family=0`, `real_apple_gpu=0`.
