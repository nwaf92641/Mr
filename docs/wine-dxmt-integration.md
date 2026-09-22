# Wine and DXMT: where the parts meet

Pinned: `third_party/wine.lock` (Wine `wine-11.0`, commit `db11d0fe`) and
`third_party/dxmt.lock` (DXMT `7c8dee1c`). See `docs/apple-graphics-stack.md` for
where Wine sits in the whole stack.

```
Windows x86-64 EXE            PE image, x86-64 machine type
  → FEX                       not in this stage
  → arm64ec PE module         Wine's own modules
  → Wine PE loader + ntdll    dlls/ntdll, loader/
  → dlls/d3d11                PE, arm64ec: the D3D11 implementation
  → winemetal                 Mach-O dylib, owned by DXMT, not by Wine
  → DXMT                      d3d11 → Metal translation
  → Metal 3 / Metal 4
```

## The one boundary that matters, and the thing that is not where people expect

Wine's Windows modules are **PE** files. On macOS the part that talks to the
operating system is **Mach-O**: the loader, `ntdll.so`, `win32u.so`, and the
graphics driver. The two halves talk through Wine's unixlib mechanism, and that
boundary is where this project's integration lives.

**`winemetal` is not a Wine module.** Wine has no `dlls/winemetal`: the
`dlls/` listing at `wine-11.0` contains `d3d11`, `dxgi`, `d3dcompiler_47`,
`ntdll`, `kernel32`, `win32u`, `winemac.drv` and `winex11.drv`, and no winemetal.
`winemetal` is DXMT's own component -- `src/winemetal` in the pinned DXMT tree,
which the DXMT build produces as `winemetal.dylib` (confirmed in the DXMT build
evidence artifact, `docs/dxmt-integration.md`). So the path

```
d3d11 (PE) → winemetal (Mach-O) → DXMT (Mach-O) → Metal
```

crosses the PE/unix boundary **once**, at winemetal, and that crossing is a
property of the DXMT integration rather than of Wine. It also means a Wine-only
change cannot put Metal behind D3D11: that work belongs to DXMT and winemetal.

## ARM64EC, from the source rather than from the name

`configure.ac` at the pinned tag:

| Line | What it says |
| --- | --- |
| 377 | `-enable-archs` accepts `i386\|x86_64\|arm\|aarch64\|arm64ec` |
| 398 | `arm64ec) test ${extra_arch+y} \|\| extra_arch=x86_64 ;;` -- arm64ec pulls in an extra x86_64 arch |
| 419-420 | probes `arm64ec-w64-mingw32-clang`, then `arm64ec-w64-mingw32-gcc`, then `clang` |
| 2387, 2394, 2417 | `vcruntime140_1`, `xtajit64` and `dpnsvr` are enabled for arm64ec by default |

So ARM64EC is a supported cross architecture upstream, and it is **not** the same
thing as aarch64. That distinction is the reason the workflow reads the PE
machine type out of each DLL it builds:

| | machine type | what it is |
| --- | --- | --- |
| `IMAGE_FILE_MACHINE_ARM64` | `0xAA64` | plain arm64 Windows code |
| `IMAGE_FILE_MACHINE_ARM64EC` | `0xA641` | arm64 code with the ARM64EC ABI, which can call and be called by x86-64 code in the same process |
| `IMAGE_FILE_MACHINE_ARM64X` | `0xA64E` | one PE carrying both an ARM64 and an ARM64EC path, loadable by either kind of process |

A module built for the wrong one of those links and then fails at load. Nothing
in this project treats a DLL as ARM64EC because the build was asked for ARM64EC,
or because it is in a directory named arm64ec.

### What the build actually emits: ARM64X

The first run of the Wine workflow reached the machine-type check with a built
`d3d11.dll` and this:

```
d3d11.dll | dlls/d3d11/aarch64-windows/d3d11.dll | Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E)
```

`--enable-archs=aarch64,arm64ec` does not produce two DLLs per module. It produces
one **ARM64X** module per module: a hybrid PE with both paths inside it, sitting in
the `aarch64-windows` directory. That is a better fit for this project than what
was expected, because ARM64X is exactly the mixed-process format -- the same
property that makes it loadable by an ARM64EC process is the property this runtime
needs when translated x86-64 code and Wine's arm64 code share one process.

The check in the workflow demanded `0xA641` and therefore failed a correct build.
The machine type was the fact and it was read correctly; the judgement about which
machine types are acceptable was wrong, and that is the kind of mistake the check
exists to surface rather than hide. It now accepts ARM64EC or ARM64X, prints each
module's sections into `hybrid-details.txt` so the EC half is evidence in the
artifact, and still fails on a plain ARM64 module.

### Why this project wants ARM64EC at all

Because of the process model. The design is one Mach process on Apple Silicon:
the game's x86-64 code is translated by FEX and runs alongside Wine's own arm64
code, and the two have to call each other. ARM64EC is Microsoft's ABI for exactly
that mixture, which is why Wine uses it for Windows on ARM and why Madeira uses
it here. A straight aarch64 build would give a process that cannot call into the
x86-64 side.

## The D3D11 path, module by module

| Module | Machine type | Role here | State |
| --- | --- | --- | --- |
| `dlls/d3d11` | arm64ec PE | the D3D11 implementation the game's `D3D11CreateDevice` reaches | builds (see the workflow) |
| `dlls/dxgi` | arm64ec PE | adapters, swap chains, `CreateDXGIFactory` | builds |
| `dlls/d3dcompiler_47` | arm64ec PE | compiles **HLSL to DXBC** -- the only shader compiler this project needs | builds |
| `dlls/ntdll`, `dlls/kernel32` | arm64ec PE | as Wine defines them | not built in this stage |
| `loader/`, `ntdll.so` | Mach-O arm64 | the Unix half | not built in this stage |
| `dlls/winemac.drv` | Mach-O arm64 | the window and the presentation surface | not built in this stage |

### Why `d3dcompiler_47` and not an HLSL compiler of our own

DXMT's D3D11 path consumes **DXBC**. `d3dcompiler_47` produces DXBC from HLSL.
Those two sentences fit together, and nothing has to be written to make them fit.
`docs/apple-graphics-stack.md` records the other half of this decision: Apple's
Metal Shader Converter consumes DXIL, which is the D3D12-era container, so it is
not a candidate for a D3D11 path no matter how good the tool is.

```
HLSL → d3dcompiler_47 → DXBC → d3d11 → DXMT → Metal
```

Whether that chain holds at runtime is **not** established. What the workflow
establishes is that the module exists, is the right machine type, and exports
`D3DCompile`.

## The libc++ question PR #5 left open

DXMT's native build links against **LLVM's** `libc++`, not the system one, and
records `@rpath/libc++.1.dylib`, because `src/airconv/darwin/meson.build` puts
LLVM's lib directory into the link flags and `airconv_dep_darwin` propagates them
to everything linking airconv, `winemetal` included. CI works around it with a
load path. An app has no load path into a build directory.

Two options were named, and the decision is **A, build against the system
libc++**, for these reasons in order:

1. **The dependency is not real in the shipped case.** The only consumer of that
   libc++ is `winemetal` and DXMT's own code compiled by Apple's clang for macOS.
   Apple's clang links the system `libc++` by default; LLVM's copy arrives only
   because DXMT passes `-L<llvm>/lib` to airconv's dependents. Removing that
   propagation is a build-flag change inside DXMT, not a code change.
2. **Option B duplicates a runtime.** Shipping `libc++.1.dylib`,
   `libc++abi.1.dylib` and `libunwind` inside a bundle means two C++ runtimes in
   one process whenever anything else -- Wine's unix side, Apple's frameworks --
   uses the system one. Two libc++ instances in one process is a class of bug
   that shows up as a crash in an unrelated place, and the FEX JIT's code cache
   and Wine's threading are the last places to want that.
3. **It is the option with the smaller surface.** A is one link flag; B is a
   library set to track, sign and keep in step with the SDK.

What has to be checked before this is called done, and has not been:
that DXMT still links and its tests still pass with LLVM's directory removed
from the flags. That is a DXMT-side patch, it is small, and it belongs with the
DXMT work rather than with Wine.

## What the presenter needs, and who owns what

Not in this stage, and written down so it is not improvised later:

```
Wine window (winemac.drv)
  → a CAMetalLayer
  → winemetal
  → DXMT's presenter
```

Ownership is the part worth stating explicitly:

| Thing | Owner | Why |
| --- | --- | --- |
| `MTLDevice` | Wine's `winemac.drv` | one device per process, and the process is Wine's |
| `CAMetalLayer` | Wine's `winemac.drv` | it belongs to the Cocoa window, and only the driver has the window |
| Swap chain | `dlls/dxgi` | the D3D11 contract is DXGI's |
| Metal resources, encoders, command buffers | DXMT | they carry D3D11 semantics |
| Metal 3 versus Metal 4 inside the frame path | DXMT | the choice has to preserve D3D11 semantics |

So the next stage's Wine work is one thing: `winemac.drv` handing a layer to the
Metal side instead of compositing the frame itself. Nothing in Mr creates a Metal
device, and no `mr_backend_metal4.m` exists.

## What this stage can and cannot claim

| | |
| --- | --- |
| Wine source pinned | PASS, tag and commit in `third_party/wine.lock` |
| ARM64EC tools exist for a macOS host | PASS, llvm-mingw `20260908` ships `arm64ec-w64-mingw32-clang` |
| Wine ARM64EC build | see the workflow run; a build is not a run |
| `d3d11` / `dxgi` / `d3dcompiler_47` machine type | read from each DLL's PE header, not from a flag |
| Wine loads a DLL | NOT VERIFIED -- needs a built Wine runtime, not just modules |
| DXBC path | NOT VERIFIED -- needs Wine running, `d3dcompiler_47` used, and DXMT linked |
| GPU rendering | REQUIRES REAL APPLE GPU VALIDATION |
| CAMetalLayer | NOT VERIFIED |
