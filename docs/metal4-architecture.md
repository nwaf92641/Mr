# Metal 4 architecture

Mr is an Apple Silicon runtime. Its graphics target is Metal 4.

This is not a statement about a future version. It is the shape of the code now:
`mr_host_pick_backend` returns Metal 4 or nothing, and a machine that cannot do
Metal 4 gets a refusal with a reason rather than a Metal 3 frame path. Everything
below follows from that.

```
Windows x86-64 EXE
        |
       FEX              not started
        |
   Wine ARM64X         PE modules built and machine type verified
        |
      D3D11            d3d11.dll built, D3D11CreateDevice exported
        |
 winemetal / DXMT      not linked
        |
     Metal 4           API surface being established from the SDK
        |
 Apple Silicon GPU     REQUIRES HARDWARE
```

## Why Metal 4 and not Metal 3

Because of what this project is for. A compatibility layer that targets the older
API targets the older API's limits, and on Apple Silicon those limits are the ones
worth escaping: command submission overhead, resource binding cost, and the
synchronisation this runtime has to do between x86-64 code that thinks it is on a
CPU and a GPU that is on the same die as the CPU.

Metal 3 is not a runtime path here. The Metal 3 code from the earlier stages stays
in `runtime/metal/` as a conformance harness and a comparison point, reachable
deliberately and never selected by `mr_host_pick_backend`. If Metal 4 is absent, the
runtime says so:

| situation | what the runtime does |
| --- | --- |
| no Metal device | `MR_BACKEND_NONE`, "no Metal device is available to this process" |
| paravirtual device only | `MR_BACKEND_NONE`, "REQUIRES REAL APPLE GPU VALIDATION" |
| OS predates Metal 4 | `MR_BACKEND_NONE`, "UNSUPPORTED TARGET", names the OS |
| no MTL4 command queue | `MR_BACKEND_NONE`, "UNSUPPORTED TARGET", names the device |
| disabled in the profile | `MR_BACKEND_NONE`, "UNSUPPORTED TARGET", names the profile |

A fallback would hide the one failure this project most needs to see. It would also
be a lie the layer above cannot detect: any gap between "Metal 4 is implemented" and
"Metal 4 is running" would fill with Metal 3 and be reported as a working title.

## The API surface is read from the SDK, never recalled

Before any Metal 4 name is used in Mr, it has to appear in the output of
`.github/workflows/metal4-sdk.yml`, which greps the SDK's own `Metal.framework` for
MTL4 headers and records each declared type beside the `API_AVAILABLE` line that
guards it.

Two facts have to be established for each API, and they are different facts:

- **In the SDK.** The name can be compiled against here. This is what lets Mr's
  Metal 4 code be written at all.
- **Answered at runtime.** This device, on this OS, responds to it. This is what
  makes the API usable on a given machine.

`tests/metal/metal4_sdk_probe.m` reports both, using `__has_include` for candidate
headers and `NSClassFromString` for candidate class names, so a name that does not
exist reports "absent" instead of failing to compile somewhere unrelated. This is
the same technique `mr_metal_probe.m` uses for capability detection, where the
requirement is deliberately two answers and not one: `supportsFamily:MTLGPUFamilyMetal4`
is the OS and the GPU reporting the feature set, and responding to
`newCommandAllocator` is the device being able to create the object the command
model is built on. Either alone turns a missing capability into a nil object far
from the cause.

An API table for Mr's Metal 4 path -- name, header, availability, DX11 use, DXMT
integration point -- is generated from that output as the path is written, not
before it. A table written from memory would be the failure mode this whole
approach exists to avoid.

## Where Metal 4 lives: inside DXMT

Mr does not get a Metal backend of its own. There is no `mr_backend_metal4.m` and
there will not be one. The translation from D3D11 semantics to Metal is DXMT's job,
and Metal 4 belongs on DXMT's side of that seam:

```
D3D11 semantics          implemented in DXMT
        |
       DXMT              owns the device, the contexts, the command model
        |
  Metal 4 native objects  what DXMT should emit
        |
 Apple Silicon GPU
```

The integration points are DXMT's own files, in the order a command travels through
them:

| DXMT file | what Metal 4 changes there |
| --- | --- |
| `src/dxmt/dxmt_device.cpp` | the `MTLDevice` and every capability query: what this device can do decides the command model |
| `src/dxmt/dxmt_command_queue.cpp` | the queue object, and where a Metal 4 command queue replaces the Metal 3 queue model |
| `src/dxmt/dxmt_command.cpp` | command buffer recording and the encoders commands are recorded into |
| `src/dxmt/dxmt_context.cpp` | resource binding, which is where argument tables change the shape |
| `src/dxmt/dxmt_texture.cpp`, `src/dxmt/dxmt_buffer.cpp` | resource creation and the lifetime that residency works from |
| `src/dxmt/dxmt_fence.cpp` | synchronisation: what a D3D11 fence becomes |
| `src/dxmt/dxmt_presenter.cpp` | the swapchain, and the `CAMetalLayer` DXMT already owns |
| `src/d3d11/d3d11_pipeline.cpp`, `d3d11_pipeline_cache.cpp` | pipeline state, compilation, and the cache that persists it |
| `src/airconv/` | DXBC translation, which is the DX11 shader path |

Nothing here is implemented yet. These are the seams, named so that the work has a
place to go, and each one is a place where Metal 4 changes an existing object rather
than adding a parallel path beside it.

## What follows from the Metal 4 model

The five things Metal 4 changes about how this runtime can work, each tied to a DX11
problem rather than to the API's name:

**Command submission.** DX11's immediate context pretends work happens when it is
asked for. The Metal 4 command queue, buffer and allocator separate the recording of
work from its submission, which is what lets Mr accumulate a D3D11 frame's commands
without a GPU round trip per call. Integration point: `dxmt_command_queue.cpp`,
`dxmt_command.cpp`.

**Argument tables.** D3D11 binds resources to fixed slots and rebinds per draw. A
Metal 4 argument table is a resource the GPU reads while drawing, so binding becomes
a table update rather than an encoder-level cost. The mapping is D3D11 resource and
slot to DXMT resource state to argument table index, starting with the resources
that dominate D3D11 games: constant buffers, shader resource views, unordered access
views, samplers, textures, buffers. Integration point: `dxmt_context.cpp`.

**Residency.** D3D11 lets an application describe what it is about to use.
Residency lets the GPU scheduler know the same thing instead of discovering it
through faults. Mr's resource lifetime and DXMT's resource state are the inputs.
Integration point: `dxmt_texture.cpp`, `dxmt_buffer.cpp`. Not to be added before
there is a clear seam: a residency system invented for its own sake is scheduling
work the driver already does.

**Synchronisation.** D3D11 resource transitions are explicit and cheap on paper;
on a unified-memory part the cost is the synchronisation, not the memory. Metal 4's
barriers are the mechanism, and DXMT's state tracking is the input. Integration
point: `dxmt_fence.cpp` and `dxmt_context.cpp`. This stays inside DXMT; Mr does not
grow a parallel synchronisation abstraction.

**Shader and pipeline compilation.** The DX11 path is HLSL to DXBC in
`d3dcompiler_47`, then DXBC to a Metal shader representation, then a pipeline. Metal
4's compilation and pipeline caching is where the second half belongs, and the
existing pipeline cache is where the result should persist rather than a new cache
beside it. Integration point: `src/airconv/`, `d3d11_pipeline.cpp`,
`d3d11_pipeline_cache.cpp`. Apple's Metal Shader Converter is a build-time tool and
is treated as one: it does not ship inside the runtime.

Each of the five gets measured before it is believed. The measurements that decide
whether any of them helped are CPU overhead per call, JIT overhead, Wine overhead,
DXMT overhead, shader compilation time, GPU utilisation, frame time, memory use and
synchronisation wait. An optimisation without a number is a change.

## What is verified, and what is not

| | |
| --- | --- |
| Metal 4 capability detection | VERIFIED as detection, in CI, on the runner's paravirtual device |
| Metal 4 API surface | recorded from the SDK per run, see the `metal4-sdk` job |
| Metal 4 command, shader, pipeline execution | NOT IMPLEMENTED |
| Real Apple GPU | REQUIRES HARDWARE |
| D3D11 to DXMT to Metal 4 | NOT VERIFIED |

The CI runner has a paravirtual GPU and reports `real_apple_gpu=0`. That is the
correct answer for it and it is not evidence about Apple Silicon. Every claim about
Metal 4 running has to come from hardware, and until it does the tables say
REQUIRES HARDWARE rather than SUCCESS.
