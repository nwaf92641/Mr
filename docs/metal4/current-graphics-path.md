# The current Madeira graphics path, as the tree actually is

Written by reading the code, not the README. Every claim below has a file and a
line. Revisions this describes:

| Piece | Revision |
|---|---|
| Madeira (`nwaf92641/Madeira`) | `1bdeb9a` |
| DXMT (`willfaust/dxmt`, branch `ios-port`) | `b4b89f0` — the gitlink this tree records |
| DXMT branch head at time of writing | `ca8a251` (ahead of the gitlink; see "Drift" below) |

Nothing here has been executed on a device. This is a static map plus what the
shipped binaries say about themselves. It is the input to
`metal4-plan.md`, not a statement that anything renders.

## The path in one picture

```
x86-64 Windows EXE
  -> FEX-Emu (x86-64 -> ARM64 JIT)          app/Madeira/FEXBridge.mm
  -> Wine ARM64EC, wineserver as a thread   app/Madeira/WineProcessBridge.m
  -> DXMT's d3d11.dll / dxgi.dll            app/Madeira/arm64ec-windows/
       (aarch64 Windows PE)
  -> winemetal, the only Metal seam          research/dxmt/src/winemetal/
       (127 unix-call slots)
  -> ObjC++ against <Metal/Metal.h>          .../winemetal/unix/winemetal_unix.c
  -> Metal 3 objects
  -> CAMetalLayer owned by Swift             app/Madeira/IOSDisplayShim.m
  -> Apple GPU
```

There is exactly one place where Madeira talks to Metal, and that is the point
of the whole file: `winemetal` is a 127-function flat C ABI, and everything
above it (`d3d11`, `dxgi`, `d3d10core`) is Windows-side C++ that never sees an
`id<MTLDevice>`.

## 1. Where the D3D11 device is created

`research/dxmt/src/d3d11/d3d11.cpp` defines the three exports the shipped
`d3d11.dll` carries (`D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`,
`D3D11CoreCreateDevice` — confirmed by reading the PE export table of
`app/Madeira/arm64ec-windows/d3d11.dll`, which exports exactly four names).

`D3D11CreateDevice` forwards to `D3D11CreateDeviceAndSwapChain` (`d3d11.cpp:216`),
which reaches `D3D11CoreCreateDevice` (`:14`). That function:

- queries `IMTLDXGIAdapter` off the DXGI adapter (`:24`) — a non-DXMT adapter is
  rejected with `E_INVALIDARG`;
- takes the Metal device from it: `dxgi_adapter->GetMTLDevice()` (`:40`), which
  is `dxgi_adapter.cpp:271` returning the adapter's stored `WMT::Reference<Device>`;
- derives the maximum D3D feature level from the GPU family:
  `supportsFamily(Apple7) ? 11_1 : 11_0` (`d3d11.cpp:40`).

The adapter's Metal device comes from `dxgi_factory.cpp:173`,
`WMT::CopyAllDevices()`, i.e. `MTLCopyAllDevices()` on the unix side
(`winemetal_unix.c:203` at the pinned revision, `:207` at head). So there is no
`MTLCreateSystemDefaultDevice` in the render path; enumeration is explicit.

Madeira also patches the feature-level probe list to lead with `11_1`
(`patches/dxmt-11-1-default-feature-level.patch`), because a NULL feature-level
array otherwise never selected 11_1 even though the device reports it.

## 2. Does D3D11 currently go through DXMT?

Yes, and only through DXMT. `app/Madeira/arm64ec-windows/d3d11.dll` is DXMT's,
not Wine's: Wine's `d3d11` is a wined3d front end and does not export
`D3D11CoreCreateDevice`, which the shipped module does.

`tools/directx-component.txt` states the rule and `tools/extract-directx.py`
enforces it: `d3d11`, `dxgi`, `d3d10core` and `winemetal` are in `DXMT_OWNED`
and are never overlaid from Microsoft's redistributable, because doing so would
replace the Metal backend with nothing.

## 3. Where the current Metal layer is

One file. `research/dxmt/src/winemetal/unix/winemetal_unix.c` (5165 lines at
branch head) is the only Objective-C in the render path; it imports
`<Metal/Metal.h>` and `<MetalFX/MetalFX.h>` at `:25`-`:26` and implements a
`__wine_unix_call_funcs[]` table (`:4869` at head) whose entries are thin
wrappers around one Metal call each.

Above the boundary, the Windows-side half is `winemetal_thunks.c` plus the
hand-written wrappers in `winemetal.h` / `Metal.hpp`. The generated
`wmt_api_names.h` names every slot; `wmt_api_census.c` counts them and can dump
the control-plane call order with `DXMT_API_CENSUS=1`.

The classification of all 127 slots — which are Metal 3 objects that Metal 4
replaces, and which are unchanged — is in `winemetal-metal4-map.tsv`, and
`tools/metal4-inventory.py` fails if it stops covering the table.

## 4. How the MTLDevice is created

`MTLCopyAllDevices()` (`winemetal_unix.c:207`), reached from DXGI's factory.
Each device becomes one DXGI adapter (`dxgi_factory.cpp:173`). Madeira is a
single-window app and the code says so: a comment at `winemetal_unix.c:730`
records that a cached capability probe is safe "because Madeira runs a single
MTLDevice".

Two places create a second device for a one-shot question rather than for
rendering: `query_bc_support()` (`winemetal_unix.c:731`), which calls
`MTLCreateSystemDefaultDevice` and releases it, and the `ioDisplayShim`.

## 5. How command submission happens

All through `winemetal`, in the Metal 3 shape:

| Step | Site (pinned revision) |
|---|---|
| queue | `_MTLDevice_newCommandQueue` `winemetal_unix.c:358` -> `newCommandQueueWithMaxCommandBufferCount:` |
| command buffer | `_MTLCommandQueue_commandBuffer` `:379` |
| blit encoder | `_MTLCommandBuffer_blitCommandEncoder` `:1162` |
| compute encoder | `_MTLCommandBuffer_computeCommandEncoder` `:1177` |
| render encoder | `_MTLCommandBuffer_renderCommandEncoder` `:1195` |
| encode a command list | `_MTLRenderCommandEncoder_encodeCommands` `:2072` |
| commit | `_MTLCommandBuffer_commit` `:393` |
| wait | `_MTLCommandBuffer_waitUntilCompleted` `:409` |

The command lists are not individual Metal calls: the Windows side builds
`struct wmtcmd_*` records (`winemetal.h:872` onward — `wmtcmd_render_setbuffer`,
`wmtcmd_render_settexture`, `wmtcmd_render_setpso`, `wmtcmd_render_draw`, and so
on) and hands the head of a pointer-linked list to one unix call, which walks it
and issues the Metal calls. That is the hot path.

## 6. How resources are created

`MTLDevice_newBuffer` (`:474`), `newTexture` (`:891`), `newSamplerState` (`:600`),
`newDepthStencilState` (`:643`), texture views (`MTLTexture_newTextureView`,
`:992`), and — at head only — placement heaps
(`MTLDevice_newPlacementHeap`, `MTLHeap_newTextureAtOffset`) and residency sets
(`MTLDevice_newResidencySet`, `MTLResidencySet_addAllocation`).

Two Madeira-specific behaviours sit in this code and must survive a Metal 4
port untouched, because they are correctness fixes rather than optimization:

- BC texture remapping. `remap_unsupported_bc()` and `query_bc_support()`
  (`winemetal_unix.c:684`-`:757`) swap BC formats for RGBA8 when the GPU cannot
  sample them, because A15 reports `supportsBCTextureCompression = NO`.
- The video-memory budget is the process's, not the GPU's
  (`madeira_ml1042_video_budget`, referenced at `:304`), bounded by
  `os_proc_available_memory`.

## 7. How shader compilation happens

Two stages, and both are Metal-3-shaped.

**Compile (Windows side).** `airconv` translates DXBC to LLVM IR and writes a
`metallib`: `research/dxmt/src/airconv/*.cpp`, built into `libdxmt_unix.a` by
`build/dxmt-ios/build.sh`. The same script compiles airconv's three helper
shaders with

```sh
xcrun --sdk macosx metal -std=metal3.1 --target=air64-apple-macos14.0
```

which is a Metal 3.1 / macOS 14 AIR target baked into the build, with a comment
in the script noting that "runtime conversion to an iOS AIR target requires
separate device validation". This is a Metal-version dependency that lives
outside DXMT's source and is easy to miss.

**Load (unix side).** `MTLDevice_newLibrary` (`:1039`) ->
`newLibraryWithData:` (`:1075`), then `MTLLibrary_newFunction` (`:1081`), then
`MTLDevice_newRenderPipelineState` (`:1316`) and
`newComputePipelineState` (`:1123`). Intermediate state caches exist as
`CacheReader_*` / `CacheWriter_*` plus `WMTSetMetalShaderCachePath`.

## 8. How presentation happens

The swap chain never touches Metal directly. `d3d11_swapchain.cpp:134` asks for a
view:

```c
native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hWnd, pDevice->GetMTLDevice(), layer_weak_);
```

On the unix side `_CreateMetalViewFromHWND` (`winemetal_unix.c:2672`) looks up
`macdrv_functions` with `dlsym(RTLD_DEFAULT, ...)` (`:2696`) and calls
`macdrv_view_get_metal_layer`. On iOS the Wine mac driver does not exist, so
Madeira supplies that symbol: `app/Madeira/IOSDisplayShim.m` implements the
`macdrv_*` entry points and resolves every HWND to the one `CAMetalLayer` that
Swift owns, registered through `madeira_display_set_layer`. This is the seam
that makes DXMT's Cocoa-shaped presentation work on iOS at all.

Per frame: `MetalLayer_nextDrawable` (`:3002`) ->
`MetalDrawable_texture` (`:2987`) -> draw -> `MTLCommandBuffer_presentDrawable`
(`:2682`).

## 9. Is there a D3D12 implementation?

No. Not in Madeira and not in DXMT.

- DXMT has no `d3d12` directory; `src/` is `airconv d3d10 d3d11 dxgi dxmt
  nativemetal nvapi nvngx util winemetal`.
- `app/Madeira/arm64ec-windows/d3d12.dll` exists, but it is Wine's builtin. It
  exports 11 names (`D3D12CreateDevice`, `D3D12CoreRegisterLayers`, ...) and is
  the vkd3d-shaped front end, not a renderer.
- The renderer behind it is Vulkan. `wined3d.dll` in the same directory exports
  `vkd3d_create_instance`, `vkd3d_acquire_vk_queue`, `vkd3d_create_device` and
  `vkd3d_create_image_resource`.
- There is no Vulkan loader and no MoltenVK anywhere in the tree — `find . -iname
  '*vulkan*'` outside `.git` returns nothing, and there is no `vulkan-1.dll`.

So D3D12 today resolves to a Vulkan device that cannot be created. This matches
`tools/directx-component.txt`, which lists `d3d12` among the modules it
deliberately does not overlay because they are "owned by DXMT and wined3d".

`ARCHITECTURE_ANALYSIS.md:431`-`:438` contains a D3D12-to-Metal mapping table.
It is a proposal, not a description of anything in the tree, and its suggested
route (vkd3d-proton and MoltenVK) is the Vulkan route the target explicitly
excludes.

## 10. Where Metal 4 can enter with the least change

At `winemetal_unix.c`, and nowhere else.

Everything above the unix-call boundary is Windows PE code that speaks
`wmtcmd_*` records and opaque `obj_handle_t` values; it does not know what a
`MTLCommandBuffer` is. Everything below is one file. Metal 4's own guidance
supports exactly this split: the app keeps its `MTLDevice` and opts in by
creating an `MTL4CommandQueue` instead of an `MTLCommandQueue`, and "the type of
queue the app creates determines which family of types the rest of the
rendering code uses". The plane to change is therefore the 22 slots the inventory
marks `mtl4-core`, `mtl4-bind` and `mtl4-compile`; the other 105 either concern
objects Metal 4 shares or are not Metal at all.

The one thing that cannot stay behind that boundary is the AIR target in
`build/dxmt-ios/build.sh`, because it is a compile-time constant of the shader
toolchain rather than a Metal call.

## Drift between the gitlink and the branch

The submodule gitlink is `b4b89f0` (127 slots). Branch `ios-port` head is
`ca8a251` (141 slots): it adds residency sets, placement heaps, the
`madeira_ir_convert` / `madeira_ctl` hooks and the `rmg_` prefix on the
remote-metal helpers. The committed PE DLLs are a third thing again —
`winemetal.dll` exports 126 names.

Three revisions of one interface is a problem for a port of this size, because
the enumeration above is only true for one of them. Before any Metal 4 work, pin
one revision and rebuild the committed DLLs from it.
