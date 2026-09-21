# Madeira: what it does, and what Mr should take from it

Read from `nwaf92641/Madeira`, clone at `/tmp/madeira`, for the
purpose of deciding what Mr reuses, what it redesigns, and what it must not
touch. Everything below is either quoted from a file or clearly marked as
inference. Where I could not check something, it says so.

## 1. The shape of the thing

One app, one Mach process. There is no Wine process and no wineserver process;
both run as threads inside the app, and the app owns the window.

| File | Lines | What it is |
|---|---|---|
| `app/Madeira/WineProcessBridge.m` | 1317 | Wine's `main` on a pthread, prefix seeding and repair |
| `app/Madeira/WineServerBridge.m` | 198 | wineserver's `main` on a pthread |
| `app/Madeira/FEXBridge.h` | 40 | FEXCore init/shutdown and a self-test |
| `app/Madeira/JITAllocator.h` | 95 | dual-mapped JIT regions, the iOS 26 protocol |
| `app/Madeira/IOSDisplayShim.m` | 198 | the `macdrv_*` functions DXMT looks for |
| `app/Madeira/Winios/Winios.m` | 1353 | the app layer and the Metal view |

## 2. wineserver as a thread, and the socketpair trick

`WineServerBridge.h` says it outright: "Run Wine's wineserver as a thread on
iOS". The mechanism, from the bridge and its comments:

- `wineserver_main()` — wineserver's `main` renamed so it can be called.
- Called on a pthread, `pthread_attr_set_qos_class_np(..., QOS_CLASS_USER_INTERACTIVE, 0)`.
- `fatal_error` is overridden to log and call `pthread_exit(NULL)` instead of
  `exit(1)`. A server-level fatal error therefore kills a thread, not the app.
- `ws_log_quiet` suppresses os_log from wineserver: "Prevents os_log buffer
  contention from blocking the main thread."
- **The client connection is a `socketpair`.** The app creates one, keeps one
  end, and calls `wineserver_inject_client_fd(pair[0])`; the wineserver's event
  loop "picks this up and calls create_process/create_thread on it."

That last part is the whole trick. Wine's client/server traffic normally travels
over a Unix socket in a filesystem path that iOS will not give an app. Handing
the server a pre-connected fd means the path is never used.

### Ordering and repair, which are easy to get wrong

- The prefix is seeded inside `wineserver_start()`, *before* Wine starts
  ("wineserver parses user.reg at boot"). Seeding later silently destroys the
  registry.
- `ios_reg_unmangle` rewrites registry paths for iOS, and
  `ios_reg_pin_graphics_null` pins the graphics driver key.
- `madeira_repair_profile`, `madeira_undo_appdata_skeleton`,
  `ios_merge_move` — the prefix is treated as something that gets corrupted and
  is repaired on launch, not as something written once.
- On shutdown: "Stop wineserver to prevent CPU spin (iOS kills for excessive
  CPU)." A spins-forever wineserver is a jetsam kill on iOS.

Environment for Wine: `WINEPREFIX`, `HOME` set to the prefix,
`WINELOADERNOEXEC=1`, `WINEDLLPATH` pointing into the bundle, `MADEIRA_WIN32U=1`.

## 3. The JIT, which is the hardest constraint on iOS

From `JITAllocator.h`, and this is the part Mr cannot design around:

- **Dual mapping.** `jit_region_create` makes two views of the same physical
  memory: an RW view to write into and an RX view to execute from.
- **Jetsam.** `jit_make_region_no_footprint` marks a region
  `VM_LEDGER_FLAG_NO_FOOTPRINT` so the JIT pool does not count against
  `phys_footprint`. Without this the code cache is what gets the app killed.
- **`jit_check_debugged()`** tests `CS_DEBUGGED`.
- **iOS 26's protocol is a debugger protocol.** `jit_install_trap_handler()`
  installs a SIGTRAP handler so `BRK` does not crash when no debugger is
  attached. `jit26_prepare_region(addr, len)` asks the attached debugger (the
  comments name StikDebug) to make a region executable. `jit26_detach()` ends it.
  `jit_test_execute` distinguishes -2 "no debugger attached" from -3 "fault
  loop", which is the difference between a policy problem and a bug.

Two strategies exist for getting executable pages: write-then-prepare, and let
the debugger allocate RX via `_M` and dual-map RW over it. Both are needed
because they fail in different situations.

- **The offset problem, which is subtle and would cost days to find.**
  `fex_get_jit_write_offset()` "Returns the runtime RX->RW distance of the
  dual-mapped JIT pool ... or 0 if the pool is not yet initialized", and it is
  "Published to xtajit64.dll via the `MADEIRA_JIT_WRITE_OFFSET` env var so its
  own FEXCore copy uses the true offset (the RW alias is placed with
  `VM_FLAGS_ANYWHERE` and is NOT guaranteed to sit at RX+0x10000000)."

  So there are **two copies of FEXCore**: one in the app, and one inside Wine's
  `xtajit64.dll`. They share a JIT pool, and the second one cannot assume the
  fixed offset it normally assumes. Mr has to solve this the same way or
  differently, but it has to know about it.

## 4. Where Metal actually lives, and it is not where I assumed

This changes Mr's design, so it is worth being precise. From
`IOSDisplayShim.h`:

> DXMT's winemetal unix side calls `dlsym(RTLD_DEFAULT, "macdrv_functions")`,
> and if that's present, uses `get_win_data` → `client_cocoa_view` →
> `macdrv_view_create_metal_view` → `macdrv_view_get_metal_layer` to obtain a
> CAMetalLayer from an HWND. On iOS there's one window (the device screen), so
> the shim resolves every HWND to the single Swift-owned CAMetalLayer.

So the seam between Wine and Metal is **Wine's display driver interface**, and
the only thing that crosses it for graphics is *a layer*. `winemetal` creates
its own `MTLDevice` and command queues in the Unix half of DXMT and does its own
encoding. `madeira_display_set_layer()` registers the one CAMetalLayer before
the first swapchain exists.

The consequence for Mr: `runtime/include/mr/mr_backend.h` proposes a C ABI
*underneath* winemetal, between it and Metal. Madeira does not have such a layer
at all — it patches DXMT instead (below). Both are defensible; they buy
different things, and this should be a deliberate choice rather than an
accident of who wrote the header first. See section 8.

There is also a `research/remote-metal/` spike that forwards Metal out of a
virtualised guest to the host GPU. Its measurements are worth keeping even
though Mr does not need the forwarding: `AppleParavirtDevice` reports **no GPU
family at all** ("Apple7=0 Apple8=0 Apple9=0 Mac2=0"), has no BC texture
compression, and implements no mesh encoder. That is a description of the
paravirtual device, not of an M-series GPU, and it is the same thing that would
make a CI runner report a low `gpu_family`.

## 5. Building DXMT for iOS: the recipe

From `build/dxmt-ios/README.md`, this is the most directly reusable knowledge in
the repository. DXMT's iOS patches live in a **fork** (`willfaust/dxmt`, branch
`ios-port`), not in Madeira.

Two halves come out:

- `libdxmt_combined.a`, ~79 MB: DXMT's Unix/Metal side, the airconv DXBC
  translator, and LLVM 15 static libraries, all linked into the app.
- Four **aarch64-windows PE DLLs** — `d3d11.dll`, `dxgi.dll`, `winemetal.dll`,
  `d3d10core.dll` — cross-built with **llvm-mingw** (aarch64-w64-mingw32, UCRT),
  which Wine loads inside the app. These are *committed binaries*, refreshed by
  `build/dxmt-ios/build-pe.sh`.

Prerequisites, all of them real work:

1. llvm-mingw at `toolchains/llvm-mingw-*-ucrt-macos-universal/`.
2. **LLVM 15.0.7 cross-built for iOS-aarch64**, in two stages: `llvm-tblgen` for
   the host first, then iOS target libraries reusing it, with
   `LLVM_TABLEGEN=<host tblgen>`, no targets, util off. It needs a one-line edit
   to `llvm/cmake/modules/AddLLVM.cmake` changing `MATCHES "Darwin"` to
   `MATCHES "Darwin|iOS"` so Apple's linker gets `-dead_strip` instead of
   `--gc-sections`.
3. The Metal toolchain, via `xcodebuild -downloadComponent MetalToolchain`.
4. Wine's aarch64-windows static libraries, built first.

And the shader toolchain problem, solved by a wrapper
(`build/dxmt-ios/xcrun-ios-shaders.sh`): meson's host compiler is macOS, but
DXMT's embedded Metal runs on the device, so the wrapper rewrites
`-sdk macosx metal` to `-sdk iphoneos` and `--target=air64-apple-macos*` to
`--target=air64-apple-ios<deployment>`.

## 6. None of the three upstreams works unpatched

This is the most important finding for planning. `patches/` contains:

| Patch | What it tells you |
|---|---|
| `wine-ios-ml435-ml482.patch`, `ml483-ml488`, `ml489-ml504`, `ml505-ml518` | Wine needed a large, ongoing, internally versioned iOS patch series |
| `wine-arm64ec-fastfail.patch` | ARM64EC needed changes |
| `wine-makedep-per-arch-pe.patch` | Wine's build system needed a per-architecture PE fix |
| `fex-ios-arm64-mbi.patch` | FEX needed an iOS patch |
| `dxmt-11-1-default-feature-level.patch` | DXMT's default D3D feature level needed fixing |
| `dxmt-no-abort-on-optional-features.patch` | DXMT **aborts** on optional features it cannot provide; that had to be removed |
| `dxmt-resource-residency-and-reclaim.patch` | DXMT's resource residency needed work, i.e. memory pressure on iOS is a real problem that shows up in the renderer |

So: "integrate Wine + FEX + DXMT" is not a linking exercise. Each component is a
patched fork, and the patch set is the actual product. Mr should expect the same
and plan for maintaining patches rather than pretending they will not be needed.

## 7. Licensing, and the thing Mr cannot copy

- Madeira's `AGENTS.md` says its committed DLLs are "GPL-licensed like the rest
  of the project", and the repository is GPL-3.0-or-later.
- Mr is MIT.

So **no Madeira code may be copied into Mr.** What may be taken is what is not
Madeira's: the architecture and its constraints, the observed behaviour of
Apple's APIs, the build recipe, and the list of things that must be patched.
Upstream Wine (LGPL-2.1+), FEX-Emu (MIT) and DXMT (LGPL-2.1-or-later, per its own
repository) are separate matters to be handled when they are actually
integrated, each with its own obligations; DXMT's LGPL in particular interacts
with static linking into one app binary and needs a deliberate decision, not a
default. I have not verified DXMT's or FEX's license text myself in this
session — only Madeira's, which is GPL.

## 8. What Mr takes, and what it redesigns

**Take as knowledge, copy nothing:**

- The single-Mach-process model, with wineserver as a thread and the
  `socketpair` + injected fd for client/server IPC.
- The seed-before-start ordering, and the assumption that the prefix needs
  repair on every launch.
- The JIT constraints: dual mapping, no-footprint ledger flag, the debugger
  protocol, and the two-copies-of-FEXCore offset problem.
- The DXMT iOS build recipe and the shader target rewrite.
- The patch inventory as a work plan.
- The `AppleParavirtDevice` measurements, for reading CI results correctly.

**Redesign deliberately:**

- **The graphics seam.** Madeira patches DXMT. Mr's ABI puts a C boundary
  between winemetal and Metal. Mr's version is more separable and makes the
  frame path testable with no Wine present at all — which is what
  `tests/metal/mr_metal_test.m` now does. What it costs is a translation layer
  that Madeira does not pay for, and that cost is only worth it if the layer
  earns its keep, which at this point it does not yet: the Metal 3 backend is
  correct but it is not faster than letting winemetal call Metal directly. The
  justification has to arrive with the Metal 4 backend, and if it does not, the
  right move is to drop the layer and patch DXMT instead.
- **Game analysis.** Madeira's launch path encodes a lot of per-title knowledge
  in the app (`SteamAppPath`, `FNA3D_FORCE_DRIVER`, per-game environment). A
  profile system is the better place for it. Mr's analyzer and profiles are a
  redesign, not a port.

## 9. What this means for Metal 4

`MTL4` is a real, separate API surface in the iOS 26 / macOS 26 SDK, and it is
the reason the ABI in `mr_backend.h` has allocators, argument tables, explicit
barriers and residency sets in it. Madeira does **not** use Metal 4 — its
`remote-metal` note measures `supportsFamily` and BC support, and its DXMT is
DXMT's Metal 3 path with patches. So there is nothing to reuse here; the Metal 4
backend is Mr's own work, and the mapping from the ABI onto `MTL4CommandQueue`,
`MTL4CommandAllocator`, `MTL4CommandBuffer`, `MTL4ArgumentTable`,
`MTL4RenderCommandEncoder` and `MTL4ComputeCommandEncoder` is Mr's problem to
get right.

The sequence that follows: get the Metal 3 backend right first, which is done and
tested, because a Metal 4 backend needs something to be compared against and a
fallback for devices and OS versions without it.

## 10. What is still unknown

Stated so that nobody reads the above as more complete than it is.

- I have not run any of this. Everything about Madeira is from reading its source
  and documentation, and everything about the Apple side is from Apple's
  documentation.
- The DXMT fork is not in Madeira's tree (the submodule is uninitialised), so I
  have not read DXMT's own iOS patches. The `patches/dxmt-*.patch` files are in
  the tree and are readable when that work starts.
- Wine's iOS patch series is thousands of lines across four files and I have not
  read them.
- The `winios.drv` display driver directory referenced in the documentation was
  not present in the clone at `research/winios.drv`, so how Wine's iOS display
  driver is structured is described here only through `IOSDisplayShim`'s
  comments.
- Whether `MTLCreateSystemDefaultDevice`, `nextDrawable` and Metal 4 work on a
  particular iPadOS 26 build is a question for a device, and nothing in this
  document answers it.
