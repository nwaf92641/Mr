# JIT on iPadOS: the real requirements

See `docs/apple-graphics-stack.md` for where each Apple technology sits on the path, what is build-time versus runtime, and what is claimed today.

FEX translates x86-64 into ARM64 at run time. That means writing machine code
into memory and then executing it, on a platform designed to prevent exactly
that. This file records what is actually required, and what an application cannot
do for itself.

It is written to be accurate rather than encouraging. Where something is not
established by Apple documentation, it says so.

## What the layers need

| Layer | Needs JIT? | Why |
| --- | --- | --- |
| FEX (x86-64 -> ARM64) | **yes** | it emits ARM64 and runs it |
| Wine ARM64EC | no | native ARM64 code, Windows ABI |
| DXMT | no | its D3D shaders are compiled by Metal's runtime, which is a system service, not app-emitted code |
| Wine's own msvcrt/mscoree | no | no shipped Windows binary needs it for DX11 games |

So the entitlement requirement is FEX's, and it is the one hard blocker for a
device build. DXMT being JIT-free is worth noticing: it means the graphics path
is not the part of this project that iPadOS blocks.

## The two mechanisms, and neither is a preference

**1. `MAP_JIT` plus the JIT entitlement.** Apple documents
`com.apple.security.cs.allow-jit` ("Allow execution of JIT-compiled code") as the
entitlement that lets an app call `mmap` with `MAP_JIT`; without it, calls with
that flag fail. Two details from Apple's own documentation matter here:

- "When your app has the Hardened Runtime capability and the
  `com.apple.security.cs.allow-jit` entitlement, it can only create **one** memory
  region with the `MAP_JIT` flag set."
- Related entitlements exist for the write paths:
  `com.apple.security.cs.allow-unsigned-executable-memory`,
  `com.apple.security.cs.jit-write-allowlist`, and the calls
  `pthread_jit_write_with_callback_np()` / `pthread_jit_write_freeze_callbacks_np()`
  for the W^X write-then-freeze discipline.

The "one region" limit is not a footnote. It is why the Madeira study found two
FEXCore copies -- one in the app, one inside Wine's `xtajit64.dll` -- sharing a
single JIT pool, with the second told the real offset of the pool's RW alias
because it is mapped `VM_FLAGS_ANYWHERE`. A design that assumes each component
can allocate its own JIT region does not survive contact with this limit.

**2. The debugger route, which is what Madeira uses.** On iOS, the entitlement
that gates `MAP_JIT` is `dynamic-codesigning`, and it is not granted to ordinary
third-party applications; it goes to system processes that need it (JavaScriptCore
being the canonical one). What does work on a non-jailbroken device is:

- the process must carry `get-task-allow`, and
- `ptrace(PT_TRACE_ME, 0, NULL, 0)` must have been called on it,

after which W^X JIT works by flipping page permissions with `mprotect` rather than
mapping RWX. A true RWX mapping still requires `dynamic-codesigning`.

This is why the Madeira study describes a BRK-based protocol and a requirement that
the process be attached to a debugger by the time JIT memory is needed.

## What this means for Mr, stated plainly

- **The app cannot grant itself these permissions.** Entitlements come from a
  provisioning profile signed by Apple. `get-task-allow` is a development
  entitlement; a build carrying it is not an App Store submission. Nothing in
  this repository changes that, and no code path in it should behave as if it
  could.
- **A non-jailbroken iPad can run this runtime only in a development or
  self-signed configuration** with `get-task-allow` and a debugger attach. That
  is a distribution limit, not a technical one that more engineering removes.
- **The JIT must be written for one region, shared.** Any design where FEX, Wine
  and anything else each map their own `MAP_JIT` region is wrong on hardware even
  if it works on a Mac.
- **`VM_LEDGER_FLAG_NO_FOOTPRINT` is a hard requirement**, per the Madeira study,
  so the code cache does not count against `phys_footprint` and get the app
  killed by jetsam. This is a documented-by-measurement fact, not an Apple
  documented one.
- **Metal 4 does not change any of this.** It is worth saying because it is a
  tempting confusion: the graphics work is not blocked by iPadOS's JIT policy.
  The translator is.

## What is not claimed here

Whether `com.apple.security.cs.allow-jit` can be provisioned for a third-party
iPadOS application in the current App Store review environment is not something
this repository can establish, and it is not claimed either way. What is
established is that the entitlement is documented by Apple, that it carries the
one-region limit, and that the debugger route is what an unsigned or
development-signed build actually uses.
