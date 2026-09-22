# Where Metal 4 belongs

See `docs/apple-graphics-stack.md` for where each Apple technology sits on the path, what is build-time versus runtime, and what is claimed today.

This answers a question that had to be settled before a line of Metal 4 code was
written: is a `DXMT -> MR Metal Backend -> Metal 4` layer worth its cost, or does
it add an abstraction and a translation that the real path does not pay?

The short answer is that it does not pay, `include/mr/mr_backend.h` is not viable
as the runtime's graphics interface, and Metal 4 belongs inside DXMT. The ABI
keeps a real job, but not the one its header currently claims.

This is written from the source, not from memory: upstream `3Shain/dxmt` at
`7c8dee1c` (2026-09-16), LGPL-2.1, plus the Madeira study in
`docs/madeira-analysis.md`.

---

## 1. The path as it actually exists

```
  EXE (PE, x86-64)
      |
      v
  d3d11.dll  (PE)                     src/d3d11/
      |
      v
  winemetal  (PE DLL, thunks)         src/winemetal/{main.c,winemetal_thunks.c}   1230 lines of Metal.hpp
      |                                   |
      |  WINE_UNIX_CALL(...)              |  every call crosses here, handle-based
      v                                   v
  winemetal unixlib (unix side)       src/winemetal/unix/winemetal_unix.c        3464 lines
      |  <- the actual [MTLDevice ...] calls live here
      |
      v
  Metal -> Apple GPU

  while the D3D11 semantics live in:
  src/dxmt/  (static library `dxmt_lib`)    device, command queue, context,
                                            resources, pipelines, presenter
  and shader translation lives in:
  src/airconv/                              DXBC -> AIR -> metallib
```

Two things about this diagram matter and neither is obvious from the outside.

**The D3D11-to-Metal translation is on the Unix side.** `src/dxmt/` is linked
into the unix library, not into the PE DLL. The PE side is thunks. A draw call
does not cross the Windows/Unix boundary; a resource creation does.

**DXMT already has two Metal abstractions.** `WMT::` in `Metal.hpp` wraps Metal
behind handle-based thunks that cross the boundary. `src/dxmt/` turns D3D11 into
`WMT::` calls. An MR layer between them would be the third.

## 2. How DXMT gets its CAMetalLayer

This is the finding that decides everything else.

`winemetal_unix.c` does **not** create a Metal device. It looks up Wine's display
driver at runtime and asks it:

```c
struct macdrv_functions_t {
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  macdrv_metal_view  (*macdrv_view_create_metal_view)(macdrv_view, macdrv_metal_device);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view);
  void (*on_main_thread)(dispatch_block_t b);
};

if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) { ... }
else { /* native build: dlsym(RTLD_DEFAULT, "get_win_data") */ }
```

and the chain is:

```
HWND -> macdrv_functions->get_win_data(hwnd)
         -> macdrv_win_data.client_cocoa_view
           -> macdrv_view_create_metal_view(view, device)
             -> macdrv_view_get_metal_layer(view)
               -> CAMetalLayer   -> Presenter::layer_
```

`Presenter` (`src/dxmt/dxmt_presenter.cpp`) takes that layer and that device:

```cpp
Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, ...)
  layer_props_.device = device;
  layer_props_.pixel_format = Forget_sRGB(format);
  layer_.setProps(layer_props_);
  auto drawable = layer_.nextDrawable();
```

Two consequences, and they are the load-bearing ones:

1. **Wine's macdrv creates the MTLDevice.** `grep -rn MTLCreateSystemDefaultDevice
   src/` over the whole of DXMT returns nothing. The device belongs to Wine's
   display driver, and DXMT borrows it, along with the view and the layer.
2. **Presentation requires an `HWND` and a Cocoa view.** There is no path to the
   drawable that does not start from a Wine window.

## 3. Where every object is created

| Object | Created by | File |
| --- | --- | --- |
| `MTLDevice` | Wine's macdrv, borrowed | `winemetal_unix.c` (`macdrv_create_metal_device`) |
| `CAMetalLayer` | Wine's macdrv, borrowed | `winemetal_unix.c` -> `dxmt_presenter.cpp` |
| command queue | DXMT | `src/dxmt/dxmt_command_queue.cpp` (237 lines, worker thread, `ready_for_commit` cv, `cmdbuf.commit()`, `waitUntilCompleted()`) |
| command buffer | DXMT | `dxmt_command_queue.cpp` |
| render / compute encoder | DXMT | `dxmt_context.cpp`, `dxmt_command.cpp` (439 lines) |
| textures | DXMT | `dxmt_texture.cpp` |
| buffers | DXMT | `dxmt_buffer.cpp`, `dxmt_ring_bump_allocator.hpp` |
| pipelines | DXMT | `dxmt_command.cpp`, `dxmt_pipeline*`, `dxmt_presenter.cpp` |
| shaders (DXBC -> AIR -> metallib) | airconv, loaded via winemetal | `src/airconv/`, `metallib_writer.cpp` |
| presentation | DXMT, through the borrowed layer | `dxmt_presenter.cpp` |

Metal object creation is spread over eleven files, concentrated in
`dxmt_command.cpp` (10 call sites). That concentration is the useful part: it
means the Metal-4-shaped changes have known places to go.

## 4. Why the MR layer cannot be the runtime path

**(a) It would own a different device.** `metal/mr_backend_metal3.m` opens with
`MTLCreateSystemDefaultDevice()`. In the real runtime that returns a device that
is *not* Wine's and has no relationship to the `CAMetalLayer` Wine's view owns.
Metal resources cannot be shared across devices, so a texture our backend
creates could never be presented through Wine's layer. The frame would have to be
copied back through the CPU, per frame, from one device's private texture into a
drawable of the other's. That is a copy of a full frame at display rate, and it
is not a trade-off, it is a dead end.

The alternative -- have our backend borrow the device from macdrv, exactly as
DXMT does -- is the honest shape of option A, and it is worth stating plainly
what it is: it is DXMT's job, duplicated, sitting one layer lower.

**(b) It would duplicate an abstraction that already exists.** `WMT::`
(`Metal.hpp`) plus `src/dxmt/` is already "a Metal abstraction that D3D11 talks
to". A third layer translates `WMT::` calls into `MR ABI` calls that our backend
translates back into Metal. Everything between DXMT and Metal is a pass-through
with a handle table in it.

**(c) It would sit on the hottest path.** Per draw and per resource binding, not
per resource creation. DXMT crosses the PE/unix boundary once per resource, and
draws stay on the Unix side where the encoders are. A layer between `WMT::` and
Metal puts a dispatch in front of every draw, every binding and every encoder
transition, in a runtime whose spend is already x86-64 -> ARM64 translation plus
Wine plus DXMT plus DXBC -> AIR. The budget for a graphics abstraction that adds
no capability is zero.

**(d) The layer is only reachable from inside Wine's window model.** The view,
the layer and the drawable all start from an `HWND`. An MR backend that presents
must either live inside that model -- where DXMT already lives -- or not present.

## 5. Options

| | A: MR Metal layer as the runtime path | B: patch DXMT directly | C: hybrid with explicit roles |
| --- | --- | --- | --- |
| Who owns the device | Wine's macdrv, borrowed by MR (the only viable shape) | Wine's macdrv, as today | Wine's macdrv, as today |
| Metal 4 lives in | new MR backend + a rewrite of `winemetal_unix.c`'s body into ABI calls | `winemetal*` + `Metal.hpp` + `src/dxmt/dxmt_command*.cpp` | same as B |
| Per-draw cost added | a dispatch and a handle lookup on every call | none | none |
| D3D11 semantics | unchanged | unchanged | unchanged |
| New code in Mr | a second backend, a second handle table, a second lifetime model | a patch series | a patch series, plus an unchanged test harness |
| Upstreamable | no | yes, in principle | yes |
| Debuggability win | can test Metal with no Wine | needs Wine to test | keeps the harness, because the harness is separate from the path |
| Maintenance | a translation to keep in step with DXMT forever | tracks DXMT | tracks DXMT |

Option A's only real argument is testability, and option C gets that without
paying for it -- by keeping the harness as a harness.

## 6. Decision

**Option C.** Metal 4 is implemented inside DXMT, as a patch series against
upstream, and `include/mr/mr_backend.h` is reclassified as what it actually is: a
graphics conformance interface used by tests, not the runtime's frame path.

What that means concretely:

- The runtime frame path is `Wine -> winemetal -> DXMT -> Metal`, exactly as
  Madeira has it. There is no Mr layer in it.
- `metal/mr_backend_metal3.m` stays, and stays useful. It is the only way to
  assert pixels on a Metal path with no Wine, no FEX and no DXMT in the way, which
  is what makes a failure attributable to one layer. It is a test double that
  draws into its own texture, and it must be described as one.
- `mr_backend.h`'s header comment currently says it is "the contract between
  DXMT's Unix-side Metal layer (winemetal) and the Metal implementation
  underneath it". That claim is false, and it is the claim that led here. It is
  corrected in this change.
- Metal 4 is not started. It is a decision made, not a feature built.

## 7. Where Metal 4 would land, if and when it does

Ordered by what has to exist before it can help:

1. `src/winemetal/unix/winemetal_unix.c` and `winemetal.h` -- the objects do not
   exist yet. `MTL4CommandQueue`, `MTL4CommandAllocator`, `MTL4CommandBuffer`,
   `MTL4CommandEncoder`, `MTL4ArgumentTable`, `MTL4Compiler`, `MTLResidencySet`
   all need surface here, as thunks and as unix-side implementations.
2. `src/winemetal/Metal.hpp` -- `WMT::` wrappers for the same, so `src/dxmt/` can
   use them.
3. `src/dxmt/dxmt_command_queue.cpp` -- already a worker thread that owns
   submission and calls `commit()`. Metal 4's explicit model (`MTL4CommandQueue`
   commit with options, allocator-owned buffers) fits this structure rather than
   fighting it, which is the strongest argument that this is the right home.
4. `src/dxmt/dxmt_command.cpp` and `dxmt_context.cpp` -- encoders become
   `MTL4CommandEncoder`, bindings become argument table writes, and hazard
   tracking becomes explicit barriers. This is where the real work is, and where
   correctness risk is highest: Metal 4 removes implicit tracking, so a missed
   barrier is a wrong image, not a slow frame.
5. `src/airconv/` + pipeline creation -- `MTL4Compiler` with
   `MTL4LibraryFunctionDescriptor` and `MTL4PipelineDataSetSerializer` for
   pipeline archives, replacing the present-library + per-pipeline compile path.
6. Residency -- `MTLResidencySet` replaces `useResource:`/`useHeap:`.

None of this is a drop-in. It is a re-architecture of DXMT's submission and
synchronisation, and it is only worth doing on hardware that reports
`MTLGPUFamilyMetal4`, which is what `mr_host_pick_backend` already decides.

## 8. What any of this will do for performance

Stated as consequences of the above rather than hopes:

- **Will help, on Metal 4 hardware:** removing per-pipeline synchronous
  compilation from the frame (`MTL4Compiler` + async tasks + pipeline archives),
  which is the stutter case DXMT already fights; explicit barriers replacing
  hazard tracking, which removes tracking work DXMT pays per encoder; argument
  tables, which turn a sequence of binding calls into one write.
- **Will not help:** anything about x86-64 -> ARM64 translation, Wine, or
  DXBC -> AIR. Those are the runtime's real costs and Metal 4 does not touch
  them.
- **Would add overhead:** the MR layer of option A, on every draw.

## 9. Licence boundary

DXMT is **LGPL-2.1** (`COPYING.LIB`), not MIT. A patched DXMT linked by Mr is
fine -- that is what LGPL is for -- but the patch series is a modification of
DXMT and must carry LGPL-2.1, not this repository's MIT. Madeira is GPL-3.0 and
none of its code or its patch text may be copied here; the patches must be
written against upstream. Both boundaries are recorded in `AGENTS.md`.

## 10. Verification levels, and which one is which

A build is not a run, and a run on a paravirtual GPU is not a run on an Apple
GPU. Four levels, never substituted for each other:

| Level | Means | Established by | State |
| --- | --- | --- | --- |
| BUILD VERIFIED | the platform layer compiles and links on arm64 against Apple's real SDK | arm64 macOS CI | yes |
| RUNTIME VERIFIED | the frame path executed and the pixels were checked | `tests/metal/mr_metal_test.m` on a machine with a device | no -- needs hardware |
| REAL APPLE GPU VERIFIED | the above on an M-series GPU, not paravirtual | a Mac or a real iPad | no |
| iPadOS VERIFIED | the above inside the iPadOS sandbox, signed, with a working JIT path | a device build, see `docs/jit-requirements.md` | no |

CI is BUILD VERIFIED only. GitHub's arm64 runners expose a paravirtual device
reporting no GPU families, so the Metal test exits 77 and CTest reports it
**Skipped**, never Passed. A green check means the code compiles; it does not
mean it draws.
