# D3D11 and D3D12 on Metal 4: the plan, and what blocks it

Read `current-graphics-path.md` first; it establishes where the seam is. This
document is what to change, in what order, and how each step can be told apart
from a step that did nothing.

## Metal 4 as Apple actually defines it

The API list in the request was explicitly a guess ("verify against the SDK
before using any of it"). Checked against Apple's documentation, several of the
names are not real, and the corrections change the shape of the port:

| Assumed | Reality |
|---|---|
| `MTL4Device` | Does not exist. Metal 4 uses the same `MTLDevice` your app already has; `MTL4CommandQueue`, `MTL4CommandBuffer`, `MTL4CommandAllocator`, `MTL4Compiler` and `MTL4ArgumentTable` are all created *from* `MTLDevice`. |
| `MTL4RenderPipeline`, `MTL4RenderPass` | Do not exist. `MTL4Compiler.makeRenderPipelineState(descriptor: MTL4PipelineDescriptor, ...)` returns `any MTLRenderPipelineState` — the protocol that already exists. Compute likewise returns `MTLComputePipelineState`. The compiler is new; the pipeline objects are not. |
| `MTL4ComputeCommandEncoder` | Exists, but it absorbs more than compute: blit and acceleration-structure encoding are folded into it. There is no `MTL4BlitCommandEncoder`. |
| — | `MTL4ArgumentTable` is the part that matters most and was not in the list. It replaces per-encoder binding. |

Facts that decide feasibility:

- Availability is **iOS 26.0+ / macOS 26.0+**, and the hardware floor is **A14
  Bionic / M1 or later**. Madeira's development device is an A15, so the
  hardware supports Metal 4. The app already requires the iOS 26 SDK
  (`ContentView.swift` calls `glassEffect()`), so the deployment target is not a
  new constraint.
- The two APIs are **interoperable, not exclusive**: "you can take advantage of
  Metal 4 using the same Metal device that your app uses today", and the family
  of types the rest of the renderer uses is selected by *which queue you create*.
  That is what makes an incremental port possible at all.
- `MTL4ArgumentTable` binds buffers by **`gpuAddress`** and textures by
  **`gpuResourceID`** — 64-bit identifiers, not object handles. This is the
  change with the widest reach in DXMT, because DXMT's `wmtcmd_render_*` /
  `wmtcmd_compute_*` records carry `obj_handle_t` resource handles today.
- `MTL4CommandBuffer` gets its backing memory from an `MTL4CommandAllocator`
  that the app owns and must not reuse while work is in flight, and command
  buffers are committed in batches to the queue (`commit:count:`), not one at a
  time. DXMT's current `commandBuffer` -> `commit` -> `waitUntilCompleted` shape
  maps onto this but not one-for-one.

None of this was compiled: this environment has no Xcode and no Metal headers.
The next step that involves real Metal code must be compiled by the existing
`ipa` workflow, which selects `Xcode_26*.app`.

## Scope, from the inventory

`winemetal-metal4-map.tsv` classifies all 127 slots at the pinned revision:

```
needs Metal 4 work ... 22   (mtl4-core 13, mtl4-bind 2, mtl4-compile 7)
unchanged by Metal 4 ... 50
not Metal ............ 55   (dxmt-internal 46, host-only 9)
```

22 slots is the whole port. `MTLBuffer`, `MTLTexture`, `MTLSamplerState`,
`MTLDepthStencilState`, `MTLSharedEvent`, `MTLHeap`, `MTLResidencySet`, the
`CAMetalLayer` / `MTLDrawable` presentation objects and the device capability
queries are all shared with Metal 4 and need no change. That is the encouraging
result, and it is why this is a port rather than a rewrite.

## Stage 0 — make the tree reproducible first

Today three revisions of the winemetal interface are in play at once:

- the submodule gitlink `b4b89f0` (127 slots) — what a clean checkout gets;
- branch `ios-port` head `ca8a251` (141 slots) — what a reader of the fork sees;
- the committed PE DLLs — `winemetal.dll` exports 126 names, so they predate
  some of the gitlink.

A port where "did my change take effect" is unanswerable is not worth starting.
So: pin one DXMT revision, rebase `patches/dxmt-*.patch` onto it, rebuild and
recommit the four PE DLLs and `libdxmt_combined.a`, and let
`tools/metal4-inventory.py` hold the map to whatever revision that is. The gate
already fails when the gitlink moves, which is the point.

## Stage 1 — Metal 4 device, queue and allocator (D3D11)

New slots, no behaviour change yet. `MTLDevice_newCommandQueue` grows a
`MTL4CommandQueue` sibling; `MTLCommandQueue_commandBuffer` grows the
`MTL4CommandBuffer` + `MTL4CommandAllocator` pair. Select between them once, at
device creation, on a capability probe.

Observable, and the minimum bar for calling this stage done:

```
[mtl4] device Metal4Capable=1 queue=MTL4CommandQueue allocators=3
```

Honest status: this cannot be observed from CI. It needs a device.

## Stage 2 — the encoder and binding path

The real work, and where the design decision is. `MTL4RenderCommandEncoder`
takes bindings through `setArgumentTable(_:stages:)`; there is no
`setVertexBuffer:offset:atIndex:`. DXMT's render command list
(`wmtcmd_render_setbuffer`, `settexture`, `setbytes`, `setpso`, `draw`, ...) is
already a serialised binding-and-draw stream, so the mapping exists — a buffer
set becomes an argument-table write by `gpuAddress` plus an offset, a texture set
becomes a write by `gpuResourceID`. What has to be decided is where argument
tables live: one per pipeline layout (mirroring DXMT's existing binding model) or
one reused across encoders, which is the case Apple's documentation is nudging
you toward ("a single argument table can serve multiple encoders").

Blit goes into `MTL4ComputeCommandEncoder`, not a blit encoder.

## Stage 3 — pipelines and libraries through `MTL4Compiler`

`MTLDevice_newRenderPipelineState`, `newComputePipelineState`,
`newMeshRenderPipelineState`, and the library path move to `MTL4Compiler`.
`WMTSetMetalShaderCachePath` should become the compiler's pipeline data set
serializer, which is a better fit than the current file-based cache.

## Stage 4 — present, and Stage 5 — first frame

Present moves onto the queue's commit path. Then the ladder the request asks
for, and the only one worth reporting against:

```
D3D11 DEVICE -> Metal 4 DEVICE -> D3D11 RESOURCE -> Metal 4 RESOURCE
  -> COMMAND SUBMISSION -> PIPELINE -> PRESENT -> FIRST FRAME
```

## D3D12 — a separate track, and not yet

There is nothing to build on. As established in the path document, D3D12 today
is Wine's `d3d12.dll` over `wined3d`'s vkd3d over a Vulkan device that does not
exist on iOS. The Vulkan route is excluded, so D3D12 means writing a new D3D12
implementation with a Metal backend, against a public API surface of roughly two
hundred interfaces.

The reason to do it second is not scheduling sentiment: the Metal 4 low-level
layer that Stage 1-4 produces — explicit allocators, batched commit, argument
tables keyed by `gpuAddress` / `gpuResourceID`, residency sets — is a closer
match to D3D12's model than to D3D11's. Building it once for D3D11 and then
targeting D3D12 at the same layer is the difference between one graphics backend
and two. Writing D3D12 first would mean designing that layer twice, or designing
it for D3D12 and then forcing D3D11 through it, which the request explicitly
cautions against.

So: no D3D12 code before D3D11 reaches first frame on Metal 4.

## Verification, split honestly

**BUILD TEST** — runs in CI, on GitHub's `macos-15` runners: `gates.yml` runs
`tools/check-all.sh` on every push, and `ipa.yml` selects `Xcode_26*.app` and
builds the full IPA. This is where Metal 4 code can be shown to compile at all,
and `tools/metal4-inventory.py` belongs in that gate list so the plan cannot
silently stop matching the interface.

**REAL GPU RUNTIME TEST** — a device, a debugger for JIT, and a sideload. Nothing
in this document has been through it. CI passing means "it builds"; it does not
mean a frame was drawn, and no report should say otherwise.

## What is blocking, right now

The work cannot proceed in this environment at all: it is x86-64 Linux with no
Xcode, no iOS SDK, no Metal headers and no GPU, so not one line of the Metal 4
work can be compiled, let alone run. Stages 1-5 land as source changes that CI
can compile; the first place a mistake shows up as behaviour is a device.
