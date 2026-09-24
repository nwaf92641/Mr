# AGENTS.md

Repository memory for Madeira. Read this before touching the JIT path — most of
the traps below have already shipped a bug once.

## What this is

Madeira runs Windows PC games on a non-jailbroken iPhone. Wine (ARM64EC), FEX-Emu
(x86-64 → ARM64) and DXMT (D3D11 → Metal) run as a single Mach process, with
wineserver as a thread rather than a separate process. It is sideloaded — JIT
requires a debugger to attach, so it cannot go through the App Store.

## Layout

- `app/Madeira/` — the iOS app (Swift + the C/C++ JIT and Wine bridge code).
- `app/Madeira/AppLayout.swift` — the tooling-vs-fullscreen decision, pure and
  unit-tested. Read the header before touching `ContentView`'s body.
- `app/Madeira/GamepadMap.swift` — controller → key/mouse mapping and the
  XInput frame, pure and unit-tested. `app/Madeira/GamepadBridge.swift` is the
  only place a controller is *read* (`GCController`); `XInputBridge.swift` looks
  one up only to find a haptic actuator to rumble.
- `app/Madeira/MadeiraXInput.c` / `.h` — the app's half of the virtual XInput
  pad: the frame the guest reads, the unix-call table it dispatches through, and
  the ABI header the wine patch mirrors. `tools/test-xinput-pad.sh` checks all
  three.
- `app/Madeira/VirtualPad.swift` — the on-screen PlayStation-style pad, as pure
  layout and touch-state functions. `app/Madeira/VirtualPadView.swift` is the
  SwiftUI window that draws it and the gestures that drive it.
- `app/Madeira/SettingsModel.swift` — the settings struct, the resolution
  policy and the engine-switch catalogue; pure and unit-tested (`test-app-ui`).
  It renders override files, it does not write them.
- `app/Madeira/SettingsStore.swift` — persistence (`UserDefaults`, JSON) and
  the file writing. This is the ONLY place settings reach the engine.
- `app/Madeira/MadeiraUI.swift` — the theme and `HomeView`.
- `app/Madeira/SettingsView.swift` — the settings screen, a front-end for the
  engine's existing `madeira-*.txt` override channel.
- `app/Madeira/madeira-jit.js` — the debugger-side JIT protocol script.
- `app/Madeira/JITAllocator.c` / `.h` — pool allocation, dual-map, BRK protocol,
  no-footprint (Jetsam) handling, trap handler.
- `app/Madeira/FEXBridge.mm` — the separate 64 MB FEX JIT pool.
- `FEX/`, `wine/`, `research/dxmt` — submodules pointing at iOS forks. Upstream
  clones will not build.
- `build/*/` — native build scripts and prebuilt test binaries
  (`proc-tests`, `x64-tests`, `dxmt-tests`, `net-tests`); there is no
  single top-level build.
- `tools/` — pre-release gates (see below).
- `research/` — investigation notes keyed by `ml###` revision numbers.

## Conventions

- Comments and commit messages reference `ml###` revision ids (e.g. `ml345`).
  Keep the highest id monotonic when adding a substantive change note.
- Two JIT pools exist and are unrelated: the app's ~896 MB pool via
  `jit26_prepare_region` (JITAllocator.c) and FEX's 64 MB pool in
  `FEXBridge.mm`. A fix to one does not affect the other.
- Credit upstream correctly: FEX-Emu prohibits AI-generated contributions (see
  `README.md` and `CONTRIBUTING.md`). Only the Madeira tree is yours to change
  from the app side.

## The embedded JIT script trap (read this twice)

`StikJITHelper.swift` ships `madeira-jit.js` as a **base64 string literal**
(`scriptBase64`). `madeira-jit.js` is NOT a member of the Xcode target, so the
literal — not the file — is what StikDebug actually runs. Editing the `.js` and
rebuilding changes nothing until you regenerate the literal:

```sh
tools/jit-script-sync.py --write    # regenerate literal from madeira-jit.js
tools/jit-script-sync.py            # check (exit 1 if drifted)
```

This is enforced by `tools/check-all.sh`, which is the pre-release gate. Run it
before shipping. (Same idea as `tools/check-prefix-template.sh`, which exists
because the prefix template once shipped absolute host symlinks, and
`tools/check-xcodeproj.py`, which exists because a source file that is not
registered in the Sources phase is silently not part of the app — invisible on
any machine that cannot open Xcode. `tools/test-device-capabilities.sh` covers
the JIT pool and override-parser policy; see Build / test constraints.)

## JIT lifecycle (app JIT pool, sized to the address space)

1. `jit_check_debugged()` reads `CS_DEBUGGED` via `csops`.
2. `enableJIT` opens StikDebug with the script in the URL; `pollForJIT` waits
   for the flag (bounded — do not make it unbounded again).
3. `allocatePool` pre-pins low address space so the pool lands above
   ~0x119000000 (FEX dispatcher encoding is address-dependent), allocates via
   `brk #0xf00d` (`x16=1`), and applies the no-footprint ledger.
   **It also measures the hole before it asks for it (ml793).** Pinning only
   moves the address-space frontier; it does not guarantee that a hole the pool
   fits in exists there. A 7GB device derives a 1760MB pool from
   `recommendedPoolMB()`, the contiguous space below the guest window is about
   a gigabyte, and the kernel's first fit then lands in 0x7000000000 — the
   guest x86-64 window, where executing pool code hangs the first call. The
   retry loop that used to sit here re-rolled the same address three times
   (ml595) and then aborted, which is a hard crash on launch, not a slow start.
   The pool size is therefore whatever the measured hole allows, floored at
   256MB, and everything downstream (the RW alias, the ledger,
   `WINE_IOS_JIT_SIZE`) uses the size that was allocated rather than the size
   that was requested.
   **It never exits the process (ml794).** The old path scheduled an `exit(0)`
   when every placement failed, so a launch that could not place the pool
   closed the app — the user sees that as "the desktop auto-shuts down", with
   no way to tell it apart from a crash. Failure now returns `nil`, the run
   sequence reports it through `RunStatus`, and the home screen shows it with a
   retry hint. Placement depends on the memory layout at that instant, so a
   second attempt is genuinely worth making. The size ladder is
   `[requested, requested/2, requested/4, floor]`, largest first, with a pause
   between waves — the waves are separated in time because the layout is not
   static, and a run that cannot place a pool now can place one a moment later.
4. `detachDebugger()` emits `brk #0xf00d` with `x16=0`. It is **one-shot** on
   purpose: the early-detach and post-Wine paths both call it, and the second
   call would trap into our own task-level exception port. `enableJIT` re-arms
   it for a fresh attach. Do not replace this with a permanent C-level flag —
   `JITAllocator.c` cannot tell a re-attach from a duplicate call.

### JIT is two requirements, and only one of them is "is JIT on" (ml801)

`jit_check_debugged()` (CS_DEBUGGED) and `isDebuggerAttached()` (P_TRACED) are
different facts, and a launch needs both:

- The pool is allocated with `brk #0xf00d`, which is answered by a LIVE
  debugger. With CS_DEBUGGED set but nothing attached, our own SIGTRAP handler
  skips the instruction, the allocation comes back zero, and the failure
  surfaces deep inside `allocatePool` as a placement complaint that has nothing
  to do with the cause.
- `detachDebugger()` is deliberate, so the second launch starts detached. That
  is the normal state after any successful run, not a fault.

`runWineFullSequence` therefore pre-flights both: it refuses to launch with
CS_DEBUGGED clear (`RunStatus.fail`), and re-attaches silently when only
P_TRACED is missing (`StikJITHelper.enableJIT`, then retries once —
`reattachAttempted` bounds the loop). The home screen reports CS_DEBUGGED,
because that is the only part the user controls, so "JIT enabled" no longer
flips to "off" the moment they enter the desktop. `isDebuggerAttached()` lives
in `EntitlementChecker.swift` and reads `kinfo_proc.kp_proc.p_flag & P_TRACED`.
Note that `EntitlementChecker.swift` needs `import Combine` for `JITState`.

### Stop-loop invariants (`madeira-jit.js`)

The script is the ONLY thing servicing traps while StikDebug is attached. Its
comment header is authoritative; the short version:

- Only genuine `BRK` instructions are skipped (`pc+4`). Everything else that
  escalates to us is a real fault and is handed back to the process as a unix
  signal — never blindly `pc+4`, that silently corrupts threads (ml344/ml345).
- Soft-signal stops (`metatype 5`, `EXC_SOFT_SIGNAL`) forward the original
  signal from `metadata[1]` and are never guarded.
- Never detach on a fault. With the StikDebug window still open the task
  exception port stays registered but unserviced, parking threads forever
  (ml345). Kill the inferior instead — a visible death beats a parked thread.
- Every reply is parsed defensively (`parseHexBigInt`); a `_M` error string must
  never be turned into a pool base address. An uncaught throw ends the script,
  which StikDebug treats as session teardown — JIT vanishes with no crash
  report. Logs are budgeted (`ulog`) because each `log()` drives a SwiftUI
  update and scene-update stalls are what the StikDebug watchdog kills.

## Is performance pinned to A15? (asked; answer verified — do not re-investigate blindly)

No. The gameplay translator does not use the hardcoded A15 block. Evidence:

- `FEXBridge.mm` ~430-450 builds a `FEXCore::HostFeatures` with A15 values
  (`CPUMIDRs.resize(8, 0x611F0250)`, `DCacheLineSize = 64`, ...). That block is
  real but its scope is tiny: this FEXCore instance only runs the in-app self
  test (`fex_test_execute()` → an embedded x86-64 ELF that returns 42) and
  allocates the shared pool. It never translates the game. The only things it
  publishes to the guest are `MADEIRA_JIT_WRITE_OFFSET` and `MADEIRA_FEX_ARENA`.
- The game is translated by `app/Madeira/arm64ec-windows/xtajit64.dll`, a
  separate FEXCore. Its symbols include
  `FEX::Windows::CPUFeatures::FetchHostFeatures(bool, HostTypeEnum)`, which in
  upstream FEX reads the live CPU (`ReadRegU64(Key, "CP 4000")` per core into
  `CPUMIDRs`, plus `mrs ctr_el0` / `mrs midr_el1`). Verified the DLL contains no
  `0x611F0250` bytes and no fixed MIDR table — it detects the real chip.
- `CPUMIDRs` could not cap codegen even if pinned: upstream FEX says so outright
  (`// Skip CPUMIDRs as it doesn't affect codegen.` in
  `FEXCore/include/FEXCore/Core/HostFeatures.h`). Its only jobs are errata
  workarounds and the guest hybrid-CPUID flag, and no Apple part has errata
  there.
- Nothing sets `FEX_HOSTFEATURES` or `FEX_FORCESVEWIDTH`. Core count is not
  pinned either — Wine derives `peb->NumberOfProcessors` from the host.

What *does* make every device behave alike, and is the real lever to pull. As of
ml787 the first two have a device-aware default plus a no-rebuild override:

- Desktop resolution was hardcoded: `1024x768` (`ContentView.swift:1159`) and
  `960x540` for the services path (`:1431`). Pixel work was therefore identical
  on an A15 and an A18, so a faster GPU bought nothing. The defaults are still
  those two — they are load-bearing for window fitting and unvalidated
  elsewhere — and `Documents/madeira-resolution.txt` (`WIDTHxHEIGHT`) overrides
  them per run.
- The JIT pool was a fixed `896 MB` (`ContentView.swift:1862`), so the
  translation cache was the same size regardless of device RAM.
  `DeviceCapabilities.recommendedPoolMB()` now derives it from the measured
  jetsam budget: exactly 896 MB at or below the 4096 MB budget of the device
  this was developed against (so nothing already validated moves), scaling
  above it and capping at 1792 MB. `Documents/madeira-pool.txt` still overrides.
- FEX has no per-microarchitecture tuning; it targets an ARMv8 baseline plus
  detected features. A15→A18 ISA gains are minor, so the translator gains
  little from the newer part. What switches exist are compiled in, so
  `Documents/madeira-fex.txt` exports `KEY=VALUE` pairs as `FEX_<KEY>` — the
  same no-rebuild channel as `madeira-dxmt.txt` on the renderer side.

To confirm on-device, read the startup log line the fork emits:
`FEX: HostFeatures={} (ml538: ...)`. Identical content on two different chips
would be the smoking gun; it should differ.

## Performance work must start by removing instrumentation (ml803)

When asked to "actually and strongly improve performance", the first real win
was not a knob. The Steam launch path had three investigation probes still armed
after their questions were answered, and each one costs time in the run it
measures — worst of all at game startup:

- `MADEIRA_SURF_SEQ=10` (`ContentView.launchSteamTesting`). Sequential surface
  dumping, and **ml556 believed it had been turned off**: the `unsetenv("MADEIRA_DUMP_SURFACES")`
  only kills the throttled dump. The seq arm is a second dump path that
  PNG-encodes ten consecutive full window surfaces per burst, 14 bursts per
  window, on a background queue, for the first ~84s of every session. PNG
  encoding a 1024x768 BGRA surface is tens of milliseconds, so a "clean
  baseline" run was never clean. Do not re-enable it to measure anything.
- `MADEIRA_IRCAP_RVA`/`MADEIRA_IRCAP_MODULE`: FEX IR capture on the translator's
  compile path. Cheap per block, but it is armed for every block compiled and
  its question is answered.
- `MADEIRA_SRCWATCH_ROWS`: only meaningful while `MADEIRA_SRCWATCH` is armed,
  which production never does.

All three now hang off **`MADEIRA_DIAGNOSTICS=1`** in the app's environment (Xcode
scheme, not persisted). `launchSteamTesting` *unsets* them in the off arm, so a
previous diagnostic launch cannot leak into a normal one.

`MADEIRA_QUIET=1` (set by `WineProcessBridge`, was already the production
default) now actually does what its comment claimed — "per-present log lines
(100+/s at RAW rates), winios poll heartbeat". The compositor and input bridge
had never consulted it, so these were live on every run:

- `winios_surface_present` computed a 4096-probe surface census **and did an
  `fprintf`+`fflush`** on every present of every window ≥400x400 for its first
  2000 presents (~33s at 60fps, per window). `[surf-alpha]`, `[surf-sentinel]`
  and the dump paths are gated with it.
- `winios_pProcessEvents` drained **every** queued input event through
  `fprintf`+`fflush`. This is the input path: a mouse-look posts a relative move
  per tick, and the synchronous write sat between the sample and the game.
- `winios_post_touch_down/move/up` and `winios_post_key` logged per event.
- The desktop window-tree dump ran every 5s for the whole session.

The general lesson, and the reason this section exists: on this stack the
diagnostics are the workload. Before adding a knob, grep for probes still
writing to stderr on a hot path (`fprintf`/`dprintf` in `Winios.m`, `driver_ios.c`,
`message_ios.c`) and check whether the gate everyone assumes is on actually is.

The user-facing half of the same work lives in Settings → Performance:

- A **profile picker** (Balanced / Performance / Quality / Custom) that is
  *derived* from the four fields it owns (`MadeiraSettings.matchingProfile`)
  rather than stored beside them, so it can never claim a preset the fields do
  not spell. `Performance` = 960x540 (34% fewer pixels than the 1024x768
  default), `d3d11.mipClampBC=1`, `WINEDEBUG=-all`. Applying one writes only
  those four fields; pacing, pool, pad and advanced switches are untouched.
- **x87 fast math** (`FEX_X87REDUCEDPRECISION=1` via `madeira-fex.txt`) is an
  explicit switch and is off in every profile, because FEX's own description is
  "reduces emulation accuracy and may result in rendering bugs". The key name
  matters: `DeviceCapabilities.fexConfigEntries` uppercases and prefixes
  `FEX_`, and FEX matches the config key uppercased.
- **`madeira-winlog.txt`** is read directly by `WineProcessBridge` and becomes
  `WINEDEBUG` (its `-all` is the only way to switch Wine off entirely without a
  rebuild). `MADEIRA_DEBUG_VERBOSE=1` still outranks it.
- `MadeiraSettings` now decodes with `decodeIfPresent` for every field. The
  synthesized decoder requires all keys, and `SettingsStore` reads a throw as
  "no saved settings" — so adding a field used to silently reset the user's
  choices. Adding one is now a compatible change; keep it that way.

### The D3D11 (DXMT) lever surface

The renderer is a submodule (`research/dxmt` → `willfaust/dxmt`, `ios-port`), so
a change there needs a fork push, a rebuild of four PE DLLs and a device run.

**First, where the D3D11 code actually is, because it decides what can ship.**
The IPA workflow builds exactly one DXMT artifact, `libdxmt_combined.a`, and
`build/dxmt-ios/build.sh` shows its contents: the Metal unix layer
(`winemetal/unix/{winemetal_unix.c,cache.c}`), **airconv** (the DXBC→LLVM
translator, 15 files) and LLVM 15. The D3D11 API itself (`src/d3d11/`, ~19k
lines), the DXMT core (`src/dxmt/`), DXGI and NVAPI are Windows-side code
compiled into four PE DLLs — `d3d11.dll`, `dxgi.dll`, `winemetal.dll`,
`d3d10core.dll` — which are **committed binaries**, in two architecture sets
(`app/Madeira/aarch64-windows/` and `app/Madeira/arm64ec-windows/`; see "Two
architectures ship" below). The workflow only asserts they exist ("DXMT PE
module not shipped" in `.github/workflows/ipa.yml`), and `build-all.sh` rebuilds
them only if absent or if the patch stamp no longer matches, because rebuilding
"only replaces known-good binaries ... a needless way to break D3D". So:

- Editing `src/d3d11/*` or `src/dxmt/*` changes **nothing** in the IPA until the
  DLLs are cross-built by `build/dxmt-ios/build-pe.sh` and committed.
- `build-pe.sh` calls `xcrun` for exactly one thing: compiling `src/dxmt/*.metal`
  into `.air`/`.metallib` (`xcrun -sdk macosx metal`), which is embedded in the
  committed DLLs. That step is genuinely macOS-only. The rest of the PE build is
  portable, and this was verified rather than assumed: llvm-mingw publishes a
  Linux `aarch64-w64-mingw32` build of the pinned version, Wine 11.4 configures
  on Linux with `--enable-win64 --enable-archs=aarch64 --with-mingw=llvm-mingw`
  and yields `tools/winebuild/winebuild` plus the `aarch64-windows` import
  archives (`libwinecrt0.a`, `libntdll.a`, `libdbghelp.a`), and meson/ninja then
  cross-compile the DLLs. The Linux PE tree builds until `dxmt_command.air` and
  stops there -- the single macOS step.
- `build-llvm.sh` exits on non-Darwin by design, so airconv — the one
  D3D11-path component the workflow *does* compile — cannot be built here
  either, and it also needs `xcrun metal` for its three embedded `.metal`
  sources.
- Nothing in-tree validates a renderer change off-device: `tests/dx11/*.cpp` are
  rendering integration tests needing a real device, and the `wmt_api_census`
  counters are already `DXMT_API_CENSUS`-gated so they do not distort a run.
- `research/dxmt` stays pinned at its tested revision. Fixes go in
  `patches/dxmt-*.patch` and are applied by `build/dxmt-ios/build-all.sh`, so no
  fork and no `.gitmodules` repoint are needed.

What a Linux container can do, and what to use it for:

- **Compile-check a changed translation unit exactly as CI will.** `meson setup`
  writes `compile_commands.json` during configuration, before any compile, so
  the real command for a file is available even though the build later stops on
  the Metal step. Extract it, swap `-o` to a scratch path, and run it.
- **Check a patch applies, and that it still applies later.** `git apply
  --check` against the pinned revision, plus `--reverse --check` to tell
  "already applied" from "no longer applies".

A renderer change still cannot be run off-device: a wrong format or state
mapping does not fail a build, it produces a black screen that only a game
reveals. Compile-verification narrows the risk to semantics, not correctness, so
say which of the two a change has actually had.

### Two architectures ship, and games load the arm64ec one (ml805)

DXMT's four modules exist **twice**: `app/Madeira/aarch64-windows/` (PE machine
`0xAA64`, native ARM64) and `app/Madeira/arm64ec-windows/` (PE machine `0x8664`,
an x86_64-callable ARM64EC hybrid). Both are tracked, and `app/Madeira.xcodeproj`
copies both into the bundle as folder resources. A session picks between them in
`app/Madeira/WineProcessBridge.m`:

    BOOL use_arm64ec = (MADEIRA_USE_ARM64EC == 1) ||
                       (strstr(madeira_exe, "x64") != NULL) ||
                       (strchr(madeira_exe, '\\') != NULL);
    const char *bundle_subdir = use_arm64ec ? "arm64ec-windows" : "aarch64-windows";

A game is launched by full Win32 path, so **every real game takes
`arm64ec-windows/`**; only the in-bundle ARM64 tests (`cube.exe`) take
`aarch64-windows/`. Both directories are still symlinked into the prefix's
`system32` (non-colliding names from the other arch, plus the `sysx64`/`sysaa64`
per-arch farms), but a game's `d3d11.dll` resolves to the arm64ec copy.

That asymmetry was a silent shipping trap. `build-pe.sh` produced only
`aarch64-windows/`; `build-all.sh` only looked in that one directory for missing
files; and `.github/workflows/ipa.yml` only asserted `aarch64-windows/*.dll`
existed. A patch could therefore be applied, built, validated, committed and
reported green while `arm64ec-windows/` — the set games load — stayed at an older
revision. It did: that directory's DLLs were last built 2026-08-28 (`4c1e9f0`)
and carried none of the feature-level, no-abort or resource-residency fixes in
`patches/`. Anything verified by loading `aarch64-windows/` says nothing about
what a game runs.

Now:

- `build-pe.sh` writes two cross files (`aarch64-windows.ini`,
  `arm64ec-windows.ini`; llvm-mingw's `arm64ec-w64-mingw32-*` reports
  `cpu_family = 'aarch64'`, so only the tool prefix and install directory
  differ), builds each into its own Meson tree (`pe/`, `pe-arm64ec/`), asserts
  the machine word of every output, and installs into both app directories.
- `build-all.sh` treats a missing DLL in **either** directory as a rebuild
  reason.
- `tools/validate-ios-bundle.py` and the workflow gate check all four modules in
  both directories, each at its own expected machine word.
- `scripts/prepare-wine-ios.sh` configures Wine with
  `--enable-archs=aarch64,arm64ec` and builds the six import archives (three per
  architecture) that DXMT links against; without them the failure is a link
  error that reads like a DXMT bug rather than a missing Wine target. The
  `--enable-archs` list alone is not sufficient and never was: see "Fusing
  aarch64+arm64ec into ARM64X leaves arm64ec with no make rules" below.

To check a built bundle, test the machine word rather than mere existence:

    python3 - <<'PY'
    import struct, zipfile
    z = zipfile.ZipFile('Madeira-unsigned.ipa')
    for arch, want in (('aarch64-windows', 0xAA64), ('arm64ec-windows', 0x8664)):
        for name in ('d3d11', 'dxgi', 'winemetal', 'd3d10core'):
            b = z.read(f'Payload/Madeira.app/{arch}/{name}.dll')
            off = struct.unpack_from('<I', b, 0x3c)[0]
            mach = struct.unpack_from('<H', b, off + 4)[0]
            print(arch, name, hex(mach), 'OK' if mach == want else 'WRONG')
    PY

### Shipping a DXMT change

`build-all.sh` hashes the patch set into
`app/Madeira/aarch64-windows/.dxmt-pe-stamp`, committed beside the DLLs. A run
whose patches match the stamp skips the PE build; a new or edited patch forces
one. The stamp exists because the previous rule -- rebuild only when the DLLs
are missing -- silently shipped unpatched binaries the moment a patch was added
to an already-populated tree.

A patch that neither applies nor is already applied is fatal, not a warning.
Skipping it would compile the unpatched source and still report success, which
is the one failure mode that reaches a device undetected.

CI does not commit, so after a new patch the runner rebuilds the DLLs and the
committed copies stay stale until they are refreshed from the produced IPA.
Until that refresh lands, the repo's DLLs and its source disagree; the stamp is
what makes that state visible rather than assumed.

### The shipped DLLs were unoptimized

The four committed DLLs were not built by the `build-pe.sh` in this tree. That
script passes `--buildtype release`, which meson turns into `-O3 -DNDEBUG`, but
the DLLs it had been shipping contain uncompiled-looking machine code: every
argument stored to the stack and immediately reloaded, no register allocation,
`b` to the next instruction, 22,528 unwind entries against the rebuilt DLL's
5,120, 21,306 stack-frame setups against 1,742, and 2.74MB of `.text` against
1.71MB. Rebuilding them from the same pinned revision with the repo's own script
is the first time they have been optimized.

This matters more than any of the small levers above, and it is also the part to
be careful about: `-O0` to `-O3` changes which latent undefined behaviour
happens to work, so a rebuild is not a pure speed-up even when the source is
identical. When a rebuilt DLL misbehaves, establish whether the same build with
`--buildtype debug` (no `-O3`) also misbehaves before blaming the source change.

The `arm64ec-windows/` set is in exactly the same state, and it is the one games
load: its `d3d11.dll` is 5,398,528 bytes against the 4,792,320 the repo's own
script produces for aarch64, with the same unoptimized instruction patterns. The
first build after ml805 optimizes both sets, so the caveat above applies to a
device run of either.

### Unimplemented features abort; they should decline (ml804)

`IMPLEMENT_ME` and `UNIMPLEMENTED` in this tree expand to `Logger::err` followed
by `abort()`. In an API implementation that is almost always the wrong move: the
D3D11 and DXGI contracts let a driver refuse a feature with an HRESULT, and
titles are written to cope with exactly that, because real hardware refuses
things all the time. `abort()` converts "this optional feature is unavailable"
into "the whole emulator is gone", with nothing the user can act on.

The clearest proof that these were oversights rather than decisions: the same
feature was already handled gracefully in one place and fatally in another.
Debug annotation through the D3D11.0 `ID3DUserDefinedAnnotation` object returns
-1 from `BeginEvent`/`EndEvent`, does nothing in `SetMarker`, and answers FALSE to
`GetStatus` -- while the D3D11.2 methods on the device context for the same thing,
`IsAnnotationEnabled`, `SetMarkerInt`, `BeginEventInt` and `EndEvent`, aborted.
Wine's own `dxgi` swapchain functions, which the app also ships, return
`E_NOTIMPL` for the very methods DXMT aborted on.

`patches/dxmt-no-abort-on-optional-features.patch` removes eleven of them:
the four annotation methods plus seven `IDXGISwapChain1/2` methods
(`GetRestrictToOutput`, `SetBackgroundColor`, `GetBackgroundColor`, `SetRotation`,
`GetRotation`, `SetSourceSize`, `GetSourceSize`). Two rules to keep when
extending it:

- **Prefer the truthful answer to a failure.** `GetRestrictToOutput` returns NULL
  because the swap chain genuinely is not restricted to an output; `GetRotation`
  returns the stored value; `GetSourceSize` returns the back buffer size, which is
  the true source size when no scaling stage exists. Answering correctly is better
  than answering "unsupported".
- **When the semantics cannot be honoured, fail rather than accept.** `SetSourceSize`
  succeeds only when the requested size equals the back buffer, and returns
  `E_NOTIMPL` otherwise. Accepting a smaller source would tell a title to render
  into the top-left corner of a backdrop it believes is being scaled up -- a
  visibly broken frame that no log explains. Refusing keeps the title at native
  resolution, where its output is correct. `SetRotation` refuses the real rotations
  for the same reason: DXMT has no rotation stage, so success would mean a
  sideways frame.

A second batch, `patches/dxmt-resource-residency-and-reclaim.patch`, fixes the
two places where the memory-management API either aborted or lied:

- `QueryResourceResidency` aborted. It is on the base `IDXGIDevice`, not an
  obscure D3D11.2 interface, so any title that manages memory can reach it. It
  now reports every resource `DXGI_RESIDENCY_FULLY_RESIDENT`, which is simply
  true under unified memory -- and the answer needs no dereference of the
  resource pointers, only the count. Note the return type is `HRESULT`, not
  `void`; the aborting stub had already guessed right, so keep it.
- `ReclaimResources` returned `S_OK` without writing `pDiscarded`, an output
  array the caller reads one entry at a time. It now writes FALSE in every slot.
  `OfferResources` is a no-op, so nothing was ever discarded and FALSE is the
  honest answer; the old code handed back whatever was in the caller's buffer.

Roughly two dozen aborts remain, concentrated in the D3D11.1/1.2 tiled-resource
and D3D11.2 tile-mapping methods, `ReadFromSubresource`/`WriteToSubresource`,
`CreateQuery1`, `SwapDeviceContextState`, and `Flush1`. `Flush1` is not a
mechanical fix: it is `void` and takes an event to signal after the GPU work
completes, so a no-op would leave a waiting title hung rather than crash it.

Two capability facts worth having before promising "full game support":

- **Feature level is not uniform, and reporting it was broken.** `d3d11.cpp`
  only considers 11_1 where the GPU is `supportsFamily(Apple7)` (A14/M1 and
  later); everything older caps at 11_0. But the default probe list started at
  `11_0` and never contained `11_1`, so an application that passed NULL feature
  levels -- the common case -- was handed 11_0 *even on Apple7*, while every
  11_1 interface (`ID3D11Device1`, `ID3D11DeviceContext1/2`, `ClearView`,
  `DiscardView1`) and every 11_1 option in `D3D11_FEATURE_D3D11_OPTIONS`
  (`MapNoOverwriteOnDynamicConstantBuffer`, `MapNoOverwriteOnDynamicBufferSRV`)
  was already implemented. Nothing inside DXMT branches on the feature level, so
  this is a pure reporting fix: it changes what a title is told, which is what
  decides whether it takes its faster buffer-update path. Patch:
  `patches/dxmt-11-1-default-feature-level.patch`.
- **BC decode is a gap only on older devices.** Hardware BC arrives with Apple9
  (A17 Pro and later); "some" Apple7/Apple8 iPads have it and Apple6-and-older
  do not. The fork's unfinished "tier-3 CPU decompression" — `remap_unsupported_bc`
  maps BC to RGBA8 but uploads the BC blob raw, so the texture reads as
  black/noise — therefore does not affect the Apple9+ devices this work targets.
  Check `[gpu-caps] ml709 BC=` in the startup log to know which side a device is
  on. The reachable config surface is exactly nine options
  (`d3d11.ignoreMapFlagNoWait`, `d3d11.metalSpatialUpscaleFactor`,
  `d3d11.mipClampBC`, `d3d11.noMeshShaders`, `d3d11.preferredMaxFrameRate`,
  `dxgi.customDeviceId`, `dxgi.customVendorId`, `dxgi.forceSDR`,
  `dxmt.shaderMetalVersion`) plus `DXMT_METALFX_SPATIAL_SWAPCHAIN` and
  `DXMT_LOG_LEVEL`; anything else is not read by the shipped DLLs.

The levers reachable from the app are these, and the traps in each:

- **`madeira-dxmt.txt` is NOT line-based, whatever the file looks like.** DXMT
  reads `DXMT_CONFIG` as inline `key=value` chunks split on `;`, and its parser
  takes one option per chunk with the value ending at the first whitespace.
  Handed a multi-line file verbatim it applies the *first* line and drops the
  rest in silence. `DeviceCapabilities.dxmtConfigInline` folds the file into the
  inline form before it becomes the environment; keep the file one-option-per-
  line (that is what a person reading it in the Files app needs) and let the
  normalizer do the translation. This was invisible while the file held exactly
  one option and would have broken the moment it held two.
- **MetalFX upscaling needs two channels.** `d3d11.metalSpatialUpscaleFactor`
  alone does nothing: DXMT gates the spatial scaler on
  `DXMT_METALFX_SPATIAL_SWAPCHAIN`. The launch sequence derives that variable
  from the same text via `DeviceCapabilities.dxmtConfigArmsMetalFX` (true only
  above 1.0, because DXMT clamps to `max(factor, 1.0)` and arming at 1 buys a
  1:1 blit), which also makes a hand-edited file work. It is a GPU-side *trade*,
  not free speed: the title keeps rendering at the desktop size and MetalFX
  scales the finished image up, so it pays off next to a desktop the panel would
  otherwise stretch badly (`960x540` at 2× presents `1920x1080`). It is
  deliberately outside every profile, like the pool and the pad — the picker
  compares only the fields a preset owns.
- **The compiled-shader cache is now in Application Support, not
  `Library/Caches`.** DXMT caches every DXBC→AIR→metallib it builds, keyed by
  SHA-1, under `_CS_DARWIN_USER_CACHE_DIR` by default — which iOS may empty at
  will, so a title could recompile thousands of shaders every launch and hitch
  for seconds on each first appearance. `DXMTShaderCache.preparedPath` exports
  `DXMT_SHADER_CACHE_PATH` (DXMT only honours an absolute path, and appends
  `shaders_<metalVersion>.db` itself), excludes the directory from backup
  (unconditionally — corelibs-foundation *does* have `URLResourceValues`, so
  the off-device gate compiles that call rather than skipping it), and
  `discardUnreadableDatabases` deletes a database whose SQLite header is not
  intact. That last part is not optional: a location iOS cannot purge is also
  one nothing else clears, so a database truncated by a jetsam kill mid-write
  would cost a full recompile on every launch from then on. The trade named in
  the comment is real — every title now shares one database, which is safe
  because the keys are content hashes, but it does grow with the library.
- **`WINEDEBUG` does not reach DXMT's logger.** DXMT resolves
  `__wine_dbg_output` in ntdll and writes warn/info lines through it, bypassing
  Wine's channel filtering — a `WINEDEBUG=-all` run still formatted a string and
  took a mutex per warning, and those warnings are per-occurrence so they land
  mid-frame. `WineProcessBridge` now sets `DXMT_LOG_LEVEL=error` when
  `madeira-winlog.txt` says `-all` (levels: trace/debug/info/warn/error/none;
  default info). Errors stay, because they are what explains a black screen.
- **`d3d11.mipClampBC` is not gated on the GPU's BC support.** DXMT's clamp
  site (`d3d11_texture_device.cpp`) does not check
  `supportsBCTextureCompression`, so on a device that *can* sample BC the clamp
  only throws away texture detail. It is still in the Performance preset for the
  A15-class case it was written for — the preset is applied explicitly, never
  automatically — and the Settings copy sends a BC-capable user to the startup
  log line `[gpu-caps] ml709 BC=1`. If that ever costs a real device, the fix
  belongs in DXMT (add the capability test to the eligibility), not in a preset.

The FPS overlay also reports **whole-task CPU%** now (a delta over the same
250ms tick as the footprint). Every Windows "process" here is a thread of one
Mach task, so this is the emulator's total, and red CPU with low FPS is the
signature of a CPU-bound frame — the case no renderer setting can fix.

## iPad fullscreen: one size class is not enough (fixed ml790)

There is exactly one tooling layout and one fullscreen layout, chosen by
`LayoutPolicy.resolve` in `AppLayout.swift`. Do not go back to branching on
`verticalSizeClass == .compact`:

- That is true ONLY for an iPhone in landscape. An iPad reports `.regular`
  vertically in **both** orientations, so every iPad was permanently stuck in the
  tooling layout with a 240pt game strip and no way to enlarge it. An iPad has no
  rotation to discover fullscreen with. This was reported as "the iPad can't make
  the game fullscreen".
- The idiom is therefore an explicit input to the policy, not inferred from the
  size class. A compact size class on an iPad means a multitasking pane, and the
  tooling rows still fit there. `Info.plist` also sets `UIRequiresFullScreen` so
  that pane case cannot arise in a shipped build.
- Immersion is forced on an iPhone in landscape and chosen everywhere else, via
  the expand button in the nav bar. `LayoutPolicy.needsExitAffordance` decides
  whether the immersive layout draws its own way out — it must be false where
  immersion was forced, because there is nowhere to return to.

Two things about the fullscreen layout are load-bearing:

- The exit control is a 44pt strip **above** the surface, never a button over
  it. `MetalHostView.shared` is added as a subview of the *window*, so anything
  SwiftUI draws over the game rect is invisible. A 4:3 iPad leaves no pillarbox
  to hide in, so an overlaid button would be dead on the one device it is for.
- The touch-controls overlay lives in its own window and cannot read
  `ContentView`'s state. It learns whether the game is up from `GameChromeState`
  (`immersive`, `topInset`) — not from `w > h`, which is wrong in portrait
  fullscreen — and starts below the strip so it does not swallow taps meant for
  the exit button. `TouchControlsModel.hitsInteractive` must keep that offset.

`tools/test-app-ui.sh` asserts the whole table. It is cheap; extend it rather
than reasoning about this again.

## Controllers: XInput first, the keyboard/pointer bridge beside it (ml790, updated by ml808)

A controller now reaches the guest two ways, from one merged frame, and both are
on at once (`GamepadBridge`'s header argues the trade):

- As **XInput** — an Xbox 360 pad in player slot 0, served by the app itself,
  which is what a game that knows what a gamepad is reads. See "The virtual
  XInput pad" below for the path, the ABI and the wine patch. Before ml808 this
  did not exist at all.
- As **virtual keys and pointer motion** through `winios_post_key` /
  `winios_pointer` — the same two calls the key buttons, the on-screen stick and
  the S2 trackpad use — so a game that only understands a keyboard and a mouse
  still gets one, mouse-look included. `GamepadMap` and `GamepadBridge` are that
  half, and it is unchanged by ml808.

What is still true, and was the whole of ml790:

- There is no HID gamepad: no winebus.sys, no hidclass.sys, no winedevice host to
  enumerate one, and `build/wineios-drv/wineios.c` is only a PE stub for the
  audio driver. Autostarting winebus on iOS wedged the driver host behind the
  service startup lock (task #19), which is why the pad is a unix call into our
  own process instead.
- `ControlAction.pad(...)` and the mapping panel's "gamecontroller" tab stay
  inert (ml645), and the panel says so.
- No per-vendor mapping table is needed or wanted: `GCExtendedGamepad`
  normalizes Xbox, PlayStation and MFi pads into the same buttons and sticks and
  this side reads that profile. A pad without a full profile (Siri Remote, basic
  MFi) arrives as `microGamepad` and gets face buttons and a d-pad, no look axis.
- Defaults are on when a controller is connected (a plugged-in gamepad that is
  ignored is the more surprising behaviour) and rebindable in
  `Documents/madeira-gamepad.txt`: `ENABLED = 0`, `MOUSE_SPEED = 1.0`, and
  `<BUTTON> = 0xNN | VK0xNN | LMB | RMB | NONE`. Movement (left stick and d-pad →
  arrow keys) is deliberately not rebindable — it is the contract every Windows
  game already has, and the same eight-way snap `JoystickKeyView` uses.
- The look axis y is negated in exactly one place: `GamepadMap` is written in
  up-positive stick coordinates but posts mouse coordinates, which count upward
  as negative. Getting that wrong looks like a broken game, not a broken bridge.
- On disconnect the bridge releases everything it was holding. There is no other
  event that could lift a key the vanished controller was holding.

`tools/test-app-ui.sh` covers the mapping, including the sign. The
`GamepadBridge` glue itself cannot be compiled or run off-device; it still needs
one on-iPad confirmation.

## The virtual XInput pad: the app's own table, not a HID device (ml808)

`xinput1_4.dll` in the guest is upstream wine's `dlls/xinput1_3/main.c` plus one
patch, `patches/wine-xinput-virtual-pad.patch`, and every `xinput1_*` module is
built from that same source (`xinput1_1/1_2/1_4` set `PARENTSRC`). `xinput9_1_0`
is untouched on purpose: it does not link a unixlib and forwards to `xinput1_4`
at runtime.

The path, end to end:

    GamepadBridge.tick()         merged frame, main thread, 60fps while a source is live
      -> XInputBridge.publish    GamepadMap.xinputState(for:) converts to XINPUT_* units
      -> madeira_xinput_publish  stores it (app/Madeira/MadeiraXInput.c, under a lock)
      -> xinput1_4!XInputGetState
      -> WINE_UNIX_CALL(madeira_pad_get_state)
      -> madeira_pad_unix_get_state  copies the frame back

- The guest finds the table through `load_builtin_unixlib` in
  `build/ntdll-unix/virtual_ios.c`, which matches on the module name. There is no
  `unix_path` for xinput on iOS, so it falls back to the name in the PE export
  directory — the same fallback `dwrite` needs, and the reason this branch
  matches "xinput1_"/"xinput9_1_0" and not "xinput" (winexinput.sys contains the
  word). A new unixlib branch that matches only a `.so` path will never fire.
- The ABI is written twice — `app/Madeira/MadeiraXInput.h` (arm64, app) and
  `dlls/xinput1_3/madeira_xinput.h` (arm64ec, guest) — and
  `tools/test-xinput-pad.sh` compiles both and compares sizeof/offsetof, so a
  field added on one side only fails a gate instead of handing the guest garbage.
  Fixed-width fields only; append to the function table, never reorder it.
- The DLLs are committed binaries
  (`app/Madeira/{aarch64,arm64ec}-windows/xinput*.dll`) because no runner builds
  them: the workflow restores native archives from cache, exactly as with the
  DXMT modules (ml807). `scripts/build-wine-xinput-pe.sh` rebuilds them on an
  Apple Silicon Mac and writes `app/Madeira/.wine-pe-xinput-stamp`; a patch
  edited without a rebuild fails `tools/check-wine-pe-stamp.py`. Every shipped
  module's machine word and the pad's unix-call import are checked by
  `tools/validate-ios-bundle.py`.
- The pad exists exactly while a source is live — a paired controller or the
  on-screen pad. `GamepadBridge.stop()` calls `XInputBridge.clear()`, so the
  guest's slot 0 reports `DEVICE_NOT_CONNECTED` and a game falls back to the
  keyboard rather than reading a stick still held when the last thumb lifted.
- Opting out is `ENABLED = 0` for the physical controller or the pad's own
  Settings switch for the on-screen one; with both off the guest has no pad at
  all, which is the opt-out for a game that reacts badly to one being present.
- Rumble comes back the other way: `XInputSetState` -> the handler `XInputBridge`
  installs -> Core Haptics on the paired pad. It is quantised to 32 steps so a
  ramping motor does not rebuild a player every frame, drives one actuator from
  the stronger of the two motors, and fails quietly: a pad whose haptics we
  cannot reach must not cost the game its input.
- These values are an ABI a game reads: A/B/X/Y are 0x1000/0x2000/0x4000/0x8000,
  a trigger is travel 0...255 with no button bit, a stick is -32768...32767 with
  +y up. `GamepadMap.xinputBits` and `tools/test-app-ui.sh` pin them. A `.lt`/`.rt`
  that is only "pressed" (on-screen pad, keyboard binding) arrives as 255.
- Not verified on a device yet. The app's C layer, the ABI and the mapping have
  host tests; "a Windows game reads the pad" needs an iPad and a game. The first
  run should check stderr for `[unixlib] module ... ->
  madeira_xinput_unix_call_table`, which is the line that says the guest found
  the table at all, and then that a stick moves in-game.

## The on-screen pad goes through the same bridge, not around it (ml800)

`VirtualPadView` is a touch DualShock — d-pad, △○✕□, four shoulders,
Options/Share and two continuous sticks — that shows itself while a session is
running (`VirtualPadMode.automatic`, the default) because the system keyboard is
not a control scheme anyone can play with. It posts through `GamepadBridge`, the
same object the physical controller uses, and therefore through the same single
`post(from:to:)` differ. That is the whole design, and it is worth keeping:

- Two differs would each hold their own idea of what was down. Whichever ran
  last would win, so a key held on one source would flicker as the other
  released it. `GamepadInput.merged` combines the two frames first instead
  (buttons union; an axis takes whichever source is pushed further, so a thumb
  resting on the pad cannot cancel a controller stick).
- The bridge used to stop its tick when no controller was connected. It now also
  runs while the pad asks for input, and stops — releasing everything — when
  neither source does.
- `ENABLED = 0` in `madeira-gamepad.txt` turns off the *physical* controller
  only. The pad has its own switch in Settings; a user who turned off a gamepad
  they are not holding has not asked for the touch controls to go away. The
  bindings in that file still apply to the pad, so it is rebindable the same way.

Geometry lives in `VirtualPad.swift` and is deliberately pure, because the
failures here are invisible off-device and unrecoverable on it: a hit region
that disagrees with the drawn button, two controls claiming one point, or a
shoulder sitting on the immersive exit button all mean "unplayable".
`tools/test-app-ui.sh` asserts, for six real device sizes: every control fully on
screen, no two overlapping, nothing inside the top chrome inset, every control's
own centre resolving to itself, and the PlayStation positions of the four face
buttons. Extend that table rather than eyeballing a layout change.

Two rules that are easy to lose and expensive to relearn:

- The pad is drawn in its own `UIWindow` at `normal + 102` — above the touch
  controls (+101), which are above the joystick pad (+100) — because the game
  surface is a window-level `UIView` above the whole SwiftUI hierarchy. Unlike
  the other two, `VirtualPadWindow.hitTest` claims a point only when a control
  is there, so the gap between the buttons still belongs to the game.
- A stick's `y` is negated in exactly one place, `VirtualPadLayout.stickVector`:
  screen coordinates grow downward and `GamepadInput` is written up-positive.
  `ContentView`'s `ControlsWindow.hitTest` also stands down for any point the pad
  claims, or a tap on the pad would also fire whatever the user had placed on
  their own touch-controls layer.

`ls`/`rs` (stick clicks) are deliberately not on the pad: pressing a stick is a
different gesture from steering it, and a tap on the stick centre already means
"steer from here". The small ✕ in the middle of the deck — the one control
that is not a gamepad button — turns the pad off from inside the game.

### The pad's visibility is a session fact, and its touches are UIKit's (ml802)

Two things about the pad are load-bearing, and both were wrong:

- **When it shows.** `automatic` keys off `RunStatus.phase.isBusy` — the only
  honest "is a session running". It used to key off `GameChromeState.immersive`,
  which is a LAYOUT fact: an iPhone in landscape is immersive from launch, so
  the pad sat over the home screen before anything had started. `always` is
  still there for someone who wants it up over the tooling screens.
- **Where its touches go.** The input layer is `VirtualPadTouchView`, a plain
  `UIView` with `isMultipleTouchEnabled`, added as the pad window's topmost
  subview. The SwiftUI overlay draws only
  (`host.view.isUserInteractionEnabled = false`) and recognises no gesture at
  all. A per-control `DragGesture` looked equivalent and is not: the touch had
  to survive `UIWindow.hitTest` into a `UIHostingController`, then be recognised
  by a view whose `@State` flag was re-created whenever the overlay re-rendered
  — and the overlay re-renders on the first frame of every press, because the
  pad's held state is what it draws. That is a pad which lights up and posts
  nothing, which is how it was reported.
- Geometry therefore has ONE source. `VirtualPadState.bounds` is published by
  the touch view's `layoutSubviews`; the drawing reads it; `claims(_:in:)` uses
  it for both windows. Two sources can disagree by a safe-area inset, and that
  disagreement is invisible until a thumb is on the glass.
- What the pad draws is a readout of what the touch layer captured (`pad.held`,
  `pad.axes`), so a control cannot look pressed without having posted a press.
- `PadHit.canReassign` is the single rule for a finger that slides: buttons swap
  with buttons (a d-pad needs it), a stick is sticky within itself, and nothing
  crosses between the two classes. `tools/test-app-ui.sh` covers it.
- `VirtualPadState.push()` writes one `[pad] input ...` line to stderr per
  gesture, on the idle->held and held->idle edges only. `[pad] input` with no
  response in the game is a delivery problem, not a pad problem; silence is the
  pad. Check that line before suspecting `winios_post_key`.

## Build / test constraints in this environment

- The iOS app builds only with `xcodebuild` on macOS. There is no Linux build.
- Swift can be checked without a Mac in three grades, all wired into
  `tools/check-all.sh`:
  - `tools/test-device-capabilities.sh` type-checks `DeviceCapabilities.swift`
    (installs nothing, adapts the one Apple-only import, shims `sysctl`) and
    asserts its pool/override tables.
  - `tools/test-app-ui.sh` type-checks and asserts `AppLayout.swift`,
    `GamepadMap.swift`, `VirtualPad.swift` and `SettingsModel.swift`. All four
    are deliberately Foundation-only so this stays possible — keep framework
    imports out of them. `SettingsStore.swift` needs Combine and
    `SettingsView.swift` and `VirtualPadView.swift` need SwiftUI, so none of
    those three can be compiled off-device.
  - `tools/check-swift-syntax.sh` runs `swiftc -parse` over every app source.
    This catches syntax only, NOT types: a misspelled property still parses.
    `ContentView.swift` needs SwiftUI and cannot be type-checked off-device, so
    type errors there are still caught by nothing until a Mac or a build.
- Two failure modes have now reached the IPA runner from this blind spot, both
  found only on a Mac. Expect them and pre-empt them:
  - An API that does not exist on the type it is called on. `URL` has no
    `setResourceValue(_:forKey:)` (that is an `NSURL` selector; the Swift pair
    is `URLResourceValues` + `setResourceValues(_:)`), and it compiled nowhere
    because it sat behind `#if canImport(Darwin)` — the one branch the Linux
    gate is told to skip. That same guard was based on a wrong premise:
    corelibs-foundation *does* have URL resource values. Prefer an unconditional
    call; if a conditional is genuinely needed, it is unverifiable here, so read
    the Apple API surface twice.
  - "The compiler is unable to type-check this expression in reasonable time."
    A SwiftUI `Section` whose body has grown large, especially with `Text` built
    from a chain of `+` over interpolated literals and with optional `.tag`
    values, can exceed the solver's budget. Neither `swiftc -parse` nor any gate
    sees it. Keep each row a small `some View`, and keep long prose in `String`
    properties rather than inline concatenations — that also makes the text
    assertable from `tools/test-app-ui.sh`.
- All three Swift gates SKIP silently without a `swiftc` on PATH, which makes a
  green `check-all.sh` mean less than it looks. A Linux toolchain is enough to
  run them: the `swift-6.2-RELEASE-debian12` tarball from swift.org runs on
  Debian 13, needs the usual desktop deps (libcurl4, libedit, libicu,
  libncurses, libpython3, libsqlite3, libxml2, uuid), and installs outside the
  repository. Put its `usr/bin` on PATH and the gates run for real.
  - This container already has one at `/workspace/swift-6.2-RELEASE-debian12`,
    with the ncurses fix in `/workspace/swiftlibs` (Debian 13 ships only
    `libncursesw.so.6`; Swift links `libncurses.so.6`, so it holds a symlink).
    Do not download the 1 GB tarball again — run the gates with:
    `export LD_LIBRARY_PATH=/workspace/swiftlibs:/workspace/swift-6.2-RELEASE-debian12/usr/lib/swift/linux`
    `export PATH=/workspace/swift-6.2-RELEASE-debian12/usr/bin:$PATH`
  - Worth knowing when reading a green run: `-parse` only proves syntax, so the
    two `SettingsView`/`FPSOverlay` changes in ml803 are still unverified by a
    compiler until a Mac or a build touches them. Keep new logic in the
    Foundation-only files when a test can reach it instead.
- `tools/test-xinput-pad.sh` is the one gate that is not Swift: it compiles
  `app/Madeira/MadeiraXInput.c` against the guest-side structs lifted out of
  `patches/wine-xinput-virtual-pad.patch`, compares the ABI by sizeof/offsetof,
  and then drives the unix-call table the way ntdll does. It needs a C compiler
  (`cc`, `clang` or `gcc`) and skips without one.
  `tools/check-wine-pe-stamp.py` is pure Python: it pairs the committed XInput
  DLLs with the patch they were built from, so a patch edit without a rebuild
  fails the gate instead of shipping a binary that does something else.
- `tools/check-swift-c-symbols.py` is the gap between "the file parses" and
  "the compiler accepts it": Swift can only call a C function that a header the
  bridging header imports declares, and `swiftc -parse` does not resolve names.
  When it fires, add the header to `Madeira-Bridging-Header.h` — that is exactly
  what `MadeiraXInput.h` was missing from (ml808).
- `scripts/typecheck-app.sh` is the real check: `swiftc -typecheck` over every
  app Swift file with the project's bridging header, Swift version and
  deployment target, against the iOS SDK. It needs a Mac with Xcode 26, so it is
  its own job (`app-compile` in `ipa.yml`) that the build job lists in `needs`,
  and a mistake in the app then fails in ~2 minutes instead of after FEX + Wine
  + LLVM + DXMT have all been built. That is the ml808 lesson: three consecutive
  ~25-minute runs each ended on a Swift error this job reports at the top.
- `.github/workflows/gates.yml` runs `tools/check-all.sh` on every push and PR,
  on `macos-15` because three gates need `swiftc`.
- `scripts/make-ipa.sh` builds and packages the unsigned `Madeira-unsigned.ipa`
  on a Mac. It runs `tools/check-build-inputs.sh` first, because a clean clone
  cannot be linked. `--output`, `--configuration`, `--keep-build`.
- `.github/workflows/ipa.yml` builds the IPA on a hosted `macos-15` runner with
  no Mac and no pre-published binaries: FEX (`scripts/build-fex-ios.sh`), the
  GnuTLS stack, the Wine unix libs, LLVM 15 for iOS
  (`build/dxmt-ios/build-llvm.sh`) and DXMT (`build/dxmt-ios/build-all.sh`) are
  compiled from the pinned submodules, checked with
  `tools/validate-ios-bundle.py`, and cached between runs. It needs the iOS 26
  SDK because `ContentView.swift` calls `glassEffect()`. Triggered by pushes
  touching `app/`, `scripts/`, `build/`, `tools/`, `patches/` or the workflow
  itself, and by `workflow_dispatch`.
- `build/dxmt-ios/build-all.sh` rebuilds DXMT's four PE DLLs (`d3d11`, `dxgi`,
  `winemetal`, `d3d10core`) **only when they are missing**: they are committed,
  they were built against the same Wine revision the submodule pins, and a
  from-scratch rebuild would swap shipped binaries for a second opinion. The
  caches must never carry them either — a restore would write over the
  checked-in copies.
- `scripts/prepare-wine-ios.sh` downloads llvm-mingw, configures Wine for macOS
  (`wine/build-macos`, including the generated headers) and builds FreeType.
  `build/wineserver/build.sh`, `build/ntdll-unix/build.sh` and
  `build/win32u-unix/build.sh` then compile the Wine unix libs for iOS from
  source — `libwineserver.a` included, so there is no patch-an-existing-archive
  step any more and no base archive to supply.
- The two caches in `ipa.yml` have to agree about FreeType, and did not once.
  The native cache held `build/freetype-ios/build` — the cmake output — but not
  `research/freetype`, and it is `research/freetype/include` that
  `build/ntdll-unix/build.sh` and `build/win32u-unix/build.sh` put on their
  include path. With a native cache hit the FreeType step skipped its rebuild
  ("FreeType outputs valid"), so the headers were never fetched; with a Wine
  unix cache hit, nothing compiled them and the run passed. Only a *cold* Wine
  unix cache exposed it, as `ERROR: required Wine input missing:
  research/freetype/include/ft2build.h` — which reads like a broken checkout
  rather than a missing cache path. Both halves are now fixed: the skip test
  requires the headers, and `research/freetype` is in the native cache's path
  list. The general rule: a build input that CI can only obtain inside a
  "skip if already valid" branch must be named by the test that skips it.
- `scripts/publish-build-libs.sh` (ml792) tars archives for a `build-libs`
  release. It predates the self-building workflow and is now optional; the
  workflow does not consume it.
- The CI recipes above and `tools/validate-ios-bundle.py` /
  `tools/ar-macho-symbols.py` came from the sibling fork
  `LT-NP/uncrashed-ipad` (branch `fix/ci-ios-build`), which proved them on a
  hosted runner. They are GPL-licensed like the rest of the project; the
  `uncrashed` name survives only in two LLVM marker filenames.
- `node` and `python3` are available and are the way to sanity-check
  `madeira-jit.js` (syntax + unit-test the pure helpers).
- `build/*-tests` ship prebuilt `.exe`/binaries; they are not runnable on the
  build host.

## DX11 hardening pass (ml806) — what changed, and what was already there

A "harden DX11" request usually arrives phrased in DXVK terms. Three of its
premises do not hold on this stack, and the work that does is already done:

- **DXVK is not the layer.** DXMT translates D3D11 to Metal. `dxvk.enableAsync`
  and `dxvk.numCompilerThreads` are not read by the shipped DLLs (DXMT compiles
  DXBC → AIR → metallib on its own worker pool; async compile is the headline
  feature, not a toggle). Geometry shaders and stream output are already
  implemented as Metal compute (`DXMTGSDispatchMarshal`). The *reachable* config
  surface stays the nine options listed in "The D3D11 (DXMT) lever surface";
  anything else is silently not read. Do not ship a file that promises more.
- **The device-identity request maps to an existing control.** `GPUIdentity`
  now has a third preset, `nvidiaGeForceGTX1070` (`dxgi.customVendorId=10de`,
  `dxgi.customDeviceId=1b81`, `dxgi.customDeviceDesc="NVIDIA GeForce GTX 1070"`).
  The request quoted `1b80`, which is the GTX 1080's id; the preset uses `1b81`
  so the name and the id agree (a fingerprinting title can catch the mismatch).
  It flows through the existing `madeira-dxmt.txt` → `DXMT_CONFIG` channel, no
  new path.
- **A `dxvk.conf` pre-config file exists as an *optional* second channel.**
  `app/Madeira/dxvk.conf` is a committed template in DXVK's `key = value` syntax
  (which DXMT also accepts). Copy it into Documents as `dxvk.conf` and the
  launch sequence folds it into `DXMT_CONFIG` via
  `DeviceCapabilities.mergedDXMTConfig(settings:extra:)` — Settings wins on any
  key both name. The file is a template, not bundled, so nothing changes until
  the user opts in. `tools/test-device-capabilities.sh` pins the merge
  precedence.

What actually changed in this pass:

- `d3dcompiler_44/45/46.dll` are aliased onto `d3dcompiler_47.dll` in the
  prefix's `system32` at session start (`WineProcessBridge.m`, ml806). Wine
  ships 43 and 47; titles import 44/45/46 by name and a missing one is a failed
  start, not a degradation.
- `scripts/stage-nls.sh` copies every `c_*.nls` the Wine build produced into
  `app/Madeira/nls/` (a folder reference, so no project edit), closing the
  `STATUS_OBJECT_NAME_NOT_FOUND` gap for codepages beyond the four committed
  ones (c_437/1252/20127/28591). It is called from `scripts/make-ipa.sh` and is
  deliberately soft — a tree with no Wine `nls/` output keeps its committed set
  and still packages.
- `build-ipa.yml` (new) is the `main`/PR/dispatch wrapper around `ipa.yml`,
  which now also declares `workflow_call`. It publishes a GitHub Release with
  the unsigned IPA on push/dispatch only, never from a PR, and auths with
  `secrets.GH_TOKEN` falling back to `github.token` — no token is embedded.
  `ipa.yml`'s own `push` trigger (branch `jit-disconnect-hardening`) is
  unchanged.

## The arm64ec PE rebuild broke, and the stamp made it fatal (ml807)

A cold DXMT build (cache miss) fails at `Build the DXMT native library`, not in
any code this tree compiles on a Mac. Two pre-existing states combine:

- The committed `.dxmt-pe-stamp` is stale: `dxmt-no-abort-on-optional-features.patch`
  and `dxmt-resource-residency-and-reclaim.patch` landed after the DLLs were last
  built (`998f623`, `e5e201f`), so `build-all.sh` correctly decides to rebuild the
  PE DLLs. A warm cache hides this -- the previous green IPAs shipped the stale
  DLLs because the DXMT cache hit skipped `build-all.sh` entirely.
- That rebuild fails in Wine's `libs/winecrt0/arm64ec-windows/*.o`.

### Why arm64ec breaks on x86 asm that arm64 never touches

`arm64ec-w64-mingw32-clang` defines `__x86_64__` (ARM64EC keeps x86_64 type
layouts for x64 source compatibility), `__arm64ec__`, and `_M_ARM64EC` -- and
deliberately *not* `__aarch64__` (`clang/lib/Basic/Targets/AArch64.cpp`). Wine
guards most of its inline asm on architecture macros, so arm64ec compiles the
x86 branches. Two places did exactly that:

1. `include/winnt.h` `__fastfail()` -- `#if defined(__x86_64__) || defined(__i386__)`
   first, so arm64ec reached the x86 `int $0x29` whose `"c"` (ECX) constraint does
   not exist on AArch64: `error: invalid input constraint 'c' in asm`.
2. `InterlockedExchange` / `InterlockedExchangePointer` -- their fast path is
   `#if (__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 7))` with the x86
   `lock; xchgl` asm as `#elif`. clang reports `__GNUC__ == 4`/`__GNUC_MINOR__ == 2`,
   so it misses that test and lands on the asm: `unrecognized instruction mnemonic`.

### The fix is a backport, not a new workaround

`patches/wine-arm64ec-fastfail.patch` is a two-hunk backport of what upstream
Wine already does for both, so the fork can drop it on the next submodule bump
rather than carrying a private idiom:

- `__fastfail`: move the `__aarch64__ || __arm64ec__` branch *first* (upstream
  Wine master orders it that way).
- both `Interlocked*`: add `|| defined(__clang__)`, so clang takes the portable
  `__atomic_exchange_n` builtin on every architecture (upstream Wine master has
  the identical `|| defined(__clang__)` in four functions; the pinned revision
  only contains two of them).

`build/dxmt-ios/build-all.sh` applies it, together with
`wine-makedep-per-arch-pe.patch` below, right before
`prepare-wine-ios.sh --dxmt-pe`, and only when the PE rebuild is actually
needed, so a stamp-matching build never touches the wine tree.

### What was checked, and what was left alone

Every x86 inline-asm site reachable from the arm64ec PE build (all of `include/`,
`libs/`, `dlls/ntdll/`, `dlls/dbghelp/`) was audited. These are not reachable and
were left untouched: `NtCurrentTeb` (its aarch64/arm64ec `#elif` precedes the
x86_64 one), `InterlockedCompareExchange128` and `YieldProcessor` (already
`!__arm64ec__` / arm-first), the `__WINE_ATOMIC_*` macro block and
`include/msvcrt/crtdbg.h`'s `_CrtDbgBreak()` -- upstream leaves both as-is at
`__x86_64__`, and `_CrtDbgBreak` is a macro that only fails if a PE translation
unit expands it.

Consequence: after this fix the runner rebuilds the four PE DLLs (both archs)
with the two DXMT patches applied, so the produced IPA is correct; the committed
DLLs and stamp stay stale until someone runs `build-pe.sh` on a Mac and commits
the result. Do not "fix" the failure by bumping the stamp to the patch hash
without rebuilding -- that is exactly the "claim the patches are in when they
are not" state the stamp exists to prevent.

### Fusing aarch64+arm64ec into ARM64X leaves arm64ec with no make rules

With the fastfail backport in, every arm64ec `.o` compiles and the build dies at
the archive step instead:

    make: *** No rule to make target 'libs/winecrt0/arm64ec-windows/libwinecrt0.a'.  Stop.

Not a missing file. Wine will not generate that rule while `aarch64` *and*
`arm64ec` are both PE architectures. `tools/makedep.c` reads `HOST_ARCH` plus
`PE_ARCHS` into an `archs` array -- `aarch64` (host), `aarch64`, `arm64ec` here
-- and then pairs the two PE architectures into one ARM64X image:

    if ((ec_arch = find_pe_arch( "arm64ec" )) && (arch = find_pe_arch( "aarch64" )))
    {
        native_archs[ec_arch] = arch;
        hybrid_archs[arch] = ec_arch;
        strarray_add( &hybrid_target_flags[ec_arch], "-marm64x" );
    }

`output_static_lib()` and `output_import_lib()` each begin with
`if (native_archs[arch]) return;`, and `native_archs[arm64ec]` is now set, so
the arm64ec import archives are never emitted. The arm64ec objects are still
compiled -- as prerequisites of the *aarch64* archive, which is linked with
`-b arm64ec-w64-mingw32 -marm64x`. So `arm64ec-windows/` gets `.o` files and
nothing else, `aarch64-windows/` gets a hybrid archive, and five of the six
targets in `prepare-wine-ios.sh --dxmt-pe` cannot exist, whatever
`--enable-archs` says.

An ARM64X image is not what this tree ships: the committed DLLs are genuinely
per-architecture (`aarch64-windows/ntdll.dll` is machine `0xAA64`,
`arm64ec-windows/ntdll.dll` is `0x8664`, no ARM64X `0xA641` anywhere), and
`WineProcessBridge.m` picks the directory per session. ARM64X is how one image
serves both; Madeira ships two.

`patches/wine-makedep-per-arch-pe.patch` removes the pairing block, which makes
makedep treat the two as independent PE architectures -- the `i386` + `x86_64`
case -- and emit every output twice, each from its own objects and with its own
`-b <target>`: `libs/winecrt0/`, `dlls/ntdll/` and `dlls/dbghelp/` each get both
`aarch64-windows/libX.a` and `arm64ec-windows/libX.a`.

That was measured, not reasoned about: makedep from the pinned `7817e22` was
built and run over `libs/winecrt0`, `dlls/ntdll` and `dlls/dbghelp` with
`HOST_ARCH=aarch64` and `PE_ARCHS="aarch64 arm64ec"`, twice. Before the change,
all three directories offered only `aarch64-windows/` -- the rule set behind the
CI failure. After it, all six archives exist, `-marm64x` is gone, and the
arm64ec archives list only `arm64ec-windows/*.o` and `-b arm64ec-w64-mingw32`.
`configure` compiles `tools/makedep.c` itself (`AC_CONFIG_COMMANDS([tools/makedep])`),
so applying the patch before the `--dxmt-pe` configure is enough; the regenerated
Makefiles carry it.

`prepare-wine-ios.sh` now greps the generated `Makefile` for each of the six
targets before running make, so a configure that did not see the patch fails
with that sentence instead of "No rule to make target".

## The launch-path audit (ml809–ml811): three silent failures, one launch

The request was a DX11 "compatibility" pass. The DX11 layer (DXMT) was already
hardened by ml806 and nothing in this pass changed it; what actually broke last
launches was the path *to* DX11, in three places that each fail quietly and
produce a symptom that points somewhere else. All three are fixed; none of them
needed a new subsystem.

### ml810 — Wine was asking the loader for display drivers this build does not contain

`explorer`'s and `win32u`'s compiled default for the display driver is the
string `"mac,x11,wayland"` (`wine/programs/explorer/desktop.c`). With no
`GraphicsDriver` value in the prefix -- which is the state of every prefix this
app has ever seeded -- `load_display_driver` walked all three names and called
`LoadLibraryW("wine<name>.drv")` for each. All three fail on iOS: `configure`
ran `--without-x`, `winemac` is macOS-only, and no display `.drv` ships at all.
So every launch logged three `LoadLibrary` misses (`0xc0000034`) before landing
on the winios driver it was always going to use anyway -- noise that reads like
a missing-file bug, and three syscalls of loader work per launch.

Two independent fixes, because they cover different prefixes:

- `build/win32u-unix/driver_ios.c`: `load_desktop_driver()` now returns `FALSE`
  immediately `#ifdef WINE_IOS`, before the registry is consulted. There is
  nothing to look for on this platform, so there is nothing to probe. The
  `#else` branch is Wine's code, untouched. The consequence in
  `load_display_driver()` is that `winios_user_driver` is installed
  *unconditionally* rather than as the fallback of a doomed probe -- which also
  removes a real (if unreachable today) hazard: a prefix whose stale GUID
  happened to name a loadable module would have taken the winemac path.
- `WineProcessBridge.m`: `ios_ensure_graphics_driver()` pins
  `HKCU\Software\Wine\Drivers\Graphics="null"` in `user.reg` at seed time. It
  must run *before* the wineserver boots, because the wineserver parses and
  rewrites `user.reg`; after that the on-disk value is no longer authoritative.
  `"null"` is not a placeholder -- it is the one value that tells Wine's driver
  loop to stop without attempting a load.

`scripts/build-prefix-snapshot.sh` writes the same value when it generates a
template, so a regenerated `prefix-template.tar.gz` ships correct and the app's
repair is a no-op on it. `tools/test-graphics-driver-pin.sh` asserts the edit
does the right thing to fixtures *and to the shipped template*, and -- the half
that matters more for a file holding every application setting the prefix has --
that it is byte-for-byte inert when there is nothing to do.

### ml809 — the codepage tables never reached the bundle (STATUS_OBJECT_NAME_NOT_FOUND)

Reported as `NtCreateFile`/NLS errors. Root cause was mundane: `wine/nls`
contains 76 tables (68 codepages) in the source tree, and only four of them
(`c_437`, `c_1252`, `c_20127`, `c_28591`) were ever committed into
`app/Madeira/nls/`. A title asking for CP932, CP1251 or anything else got a
failed lookup in locale setup. Three parts to the fix:

- `scripts/stage-nls.sh` now stages the full set from `wine/nls` and **fails**
  (exit 1) if fewer than 68 codepages are present, instead of silently shipping
  a subset. It stays soft when there is no Wine tree at all, which is what makes
  a source-only checkout still packageable.
- `WineProcessBridge.m` links every bundled `nls/*.nls` into
  `drive_c/windows/system32` at seed time. That is the *second* path ntdll tries
  (`env_ios.c: open_nls_data_file` resolves `<data_dir>/nls/<name>.nls` first,
  then `C:\windows\system32\<name>.nls`), and the template strips every `.nls`
  out of system32 -- so before this the fallback could not succeed for any
  codepage. Symlinks, not copies: the bundle owns the files, and a copy goes
  stale on the next Wine bump. It also logs loudly when the bundle carries fewer
  than 60 tables, because that failure is otherwise invisible on the device.
- `tools/check-nls-set.py` compares source against bundle (component-wise, by
  name) and is wired into `tools/check-all.sh`, so a stale or partial bundle
  fails the gate rather than the game.

### ml811 — "JIT pool allocation failed" was usually "no debugger attached"

`StikJITHelper.pollForJIT` completed on `jit_check_debugged()`, which is
`CS_DEBUGGED` -- a flag that is **sticky across detach**. On the first launch
that is a fine signal (`CS_DEBUGGED` is clear until StikDebug arrives). On every
launch after it, the flag is already set before anything is opened, so the poll
returned `true` on its first tick, StikDebug was still launching, and the caller
went straight to allocating the JIT pool. The pool is allocated through a
`BRK #0xf00d` that only the debugger answers; with nobody attached it is not
answered at all, the returned pointer is zero, and every placement wave fails.
The user then saw an *address-space* error ("BAD POOL: no placement", "Debugger
failed to allocate RX memory") for a problem that was never about memory, and
pressing launch again "fixed" it whenever the race went the other way.

- The wait is now on `P_TRACED` (`isDebuggerAttached()`), which answers "is
  StikDebug on this process right now".
- `ensureAttached(attempts:)` retries the URL open up to three times, spaced by
  1.5s, and re-checks `P_TRACED` itself rather than trusting the completion --
  a caller can no longer run on a success that is only a sticky flag. The launch
  sequence uses it; the single-shot re-attach is gone.
- The pool failure now names the cause when `P_TRACED` is clear, so the next
  reader of that log does not go looking in the allocator.

`isDebuggerAttached()` and `jit_check_debugged()` are the two questions
`JITState` keeps apart in the UI chip ("JIT ready · detached"); ml811 makes the
launch path keep them apart too.

### Verified, not changed

- `d3dcompiler_43.dll` ships real; `44/45/46` are aliased onto `47` at session
  start (ml806, `WineProcessBridge.m`), and the cross-arch link pass covers them
  for the non-session arch. All five names resolve in either arch.
- The `dxvk.conf` request: `dxvk.*` keys remain documented-but-inert (DXMT is
  the layer; see the ml806 section). The device identity stays
  `dxgi.customDeviceId=1b81` (`nvidiaGeForceGTX1070`), *not* the `1b80` asked
  for -- `1b80` is the GTX 1080's id and would contradict the descriptor string
  next to it.

## The Winlator audit (ml812): compatibility is a missing-module problem

Studied `brunodev85/winlator` at `bae41e0` (11.2) for reusable compatibility
work. The full audit, mechanism by mechanism, is `research/winlator-compat-audit.md`;
the short version is that its value is the *shape* of its layer, not its code --
it is Android, this is iOS, and its graphics stack is Vulkan where this one is
Metal. Three things came out of it that are true regardless:

**The number.** Winlator's `common_dlls.json` expects 580 DLLs in system32.
This bundle ships 120 for the x64 guest and 115 for the ARM64 one, and 124 of
them overlap. The set was never a compatibility decision -- it is the
transitive closure of the imports of Wine's own test executables (twelve test
`.exe` files ship in `arm64ec-windows/`) plus the four DXMT modules. That is
why `xaudio2_7.dll` is absent while its helper `X3DAudio1_7.dll` is present,
and why the previous round's DXMT tuning did not move the "this game does not
start" number: a title whose `LoadLibrary` fails is not a title that can be
tuned. `tools/pe-module-manifest.txt` is the missing list (121 modules Wine
builds, 12 Microsoft-only), `tools/check-pe-module-set.py` prints it on every
gate run, and `--strict` fails until it is closed.

**The alias table (`app/Madeira/DLLAliases.h`).** ml812 made 32 names resolve to
a module the bundle ships (`d3dx9_24..42 -> d3dx9_43`, `d3dcompiler_33..42 ->
d3dcompiler_43`, `d3dcompiler_44..46 -> d3dcompiler_47`) on the argument that
the families export unversioned symbols (`D3DXMatrixMultiply`, `D3DCompile`) and
Wine models every generation as a forwarder to one implementation. **The first
two ladders are wrong and ml813 removed them.** `d3dx9_24` exports nine names
`d3dx9_43` does not (`D3DXCreateFragmentLinker`, `D3DXGatherFragments{,FromFileA,
FromFileW,FromResourceA,FromResourceW}`, `D3DXCpuOptimizations`,
`D3DXGetTargetDescByName/ByVersion`), `d3dx9_33` six, `d3dx9_36/39` seven, and
`d3dcompiler_33` is missing six from `d3dcompiler_43`
(`D3DCompileFromMemory`, `D3DDisassembleCode`, `D3DDisassembleEffect`,
`D3DGetCodeDebugInfo`, `D3DPreprocessFromMemory`, `D3DReflectCode`). A title that
imports one of those does not load the module, which is the same symptom as the
missing DLL the alias was meant to cure -- but now with a module present, so the
next reader has further to look. Only `d3dx9_42` (nothing missing) and
`d3dcompiler_43 -> d3dcompiler_47` (its 17 exports are all in 47's 29) are sound,
and the table keeps the latter, plus `d3dcompiler_44/45 -> 47` for the two
generations Microsoft only ever shipped in the SDK.
The runtime loop in `WineProcessBridge.m` links into the session's `system32`,
skips names that already exist (a real copy, a native component, or a user's
drop-in always wins) and clears stale links, so it is idempotent.
`tools/check-dll-aliases.py` gates the table: a target must be shipped for the
x64 session, an alias may never shadow a shipped module, no chains, and (ml813)
every alias must carry a reference set -- the aliased name's real exports -- so
that the exporting half of the claim is checked rather than assumed. It found a
real asymmetry on its first run: `d3dx9_43.dll` and `d3dcompiler_43/47.dll` exist
only in `arm64ec-windows/`, so the ARM64 session has no DirectX 9 shader compiler
at all. Still true, and still not on the critical path: see ml813.

**The build (`scripts/build-wine-pe-modules.sh` +
`.github/workflows/wine-pe-modules.yml`).** Builds the manifest's modules from
the pinned tree for both guest architectures with the per-module recipe
`scripts/build-wine-xinput-pe.sh` already proves
(`make dlls/<module>/<arch>/<module>.dll`), refusing to touch the modules DXMT
and the XInput patch own, validating every installed module's machine word, and
additive unless `--force`. It was a separate workflow from `ipa.yml` on purpose:
the DLLs are committed and the IPA job restores its Wine tree from cache, so
shipping ~240 new modules into the release build before one run has exercised
them is risk with no upside. Run it, inspect the artifact, commit the DLLs with
the Wine revision in its summary, then the same two steps (prepare-wine-ios.sh,
build-wine-pe-modules.sh) move into `ipa.yml` unchanged. **ml813 took the second
option and did not commit the DLLs** -- 161 MB of binaries that are only valid
for one Wine revision is a worse trade than 35 minutes of runner time, and the
IPA workflow builds and caches them (see that section).

What was deliberately *not* taken: gladio/vortek (Android GL/Vulkan process
shims; DXMT links Metal directly), DXVK/VKD3D/Zink (Vulkan-only), Box64's
presets (different emulator; only its stability-ladder shape transfers, and
that is backlog item 4), and its rootfs/pulseaudio packaging.

Items 1-3 are done in ml813 (the module build ran to completion for both
architectures, the DirectX component exists, and the ARM64-side modules are the
same 143). Still open: per-title settings profiles (item 4) and wine-mono for the
managed titles (item 5).

## The compat set is built, shipped and gated (ml813)

ml812 ended with a manifest, a gate and a build script that had never run to
completion. This pass ran it, closed the Microsoft half, and fixed one thing
ml812 got wrong (the alias ladder, above). The order matters: the manifest is
what a DirectX-era title imports, so "DX11 support" here is a packaging property
before it is a rendering one -- a title whose `LoadLibrary` fails is not a title
any layer can translate for.

**The Wine half: 143 modules, both architectures, 0 errors.** The build is per
module (`make dlls/<module>/<arch>/<module>.dll`), it refuses to touch anything
DXMT or the XInput patch owns, and it is additive. What changed to make it
finish and stay finished:

- `--strip-debug` with the arch-specific `llvm-mingw` strip, which is what made
  the set shippable. Measured, not assumed: across the 143 modules, arm64ec
  168 MB -> 50 MB and aarch64 267 MB -> 111 MB, and the only PE modules left with
  `.debug_info` are the DXMT ones `build-all.sh` produces. Debug info in a
  shipped DLL is pure payload; not `--strip-all`, which is 16% smaller again but
  drops the COFF symbol table for no reward. The export table is untouched
  either way (336 exports in, 336 out, on a d3dx9_41 arm64ec module).
- **The patch order is load-bearing, and the script now enforces it.** Wine
  generates one flat `Makefile` with `tools/makedep`, so a patch to `makedep.c`
  only changes the rules once makedep has been rebuilt *and* re-run. A build
  directory configured before `wine-makedep-per-arch-pe.patch` -- which is every
  caller that runs `prepare-wine-ios.sh` first, the IPA workflow included -- holds
  a Makefile from the unpatched makedep, where the arm64ec half of every module
  does not exist as a separate target: it is folded into `aarch64-windows/` as one
  ARM64X hybrid (0 arm64ec DLL rules, 880 `-marm64x` flags, measured). The
  `has_rule` loop then reports all ~150 arm64ec targets as unbuildable and stops,
  which reads like a tree that cannot build the set. `build-wine-pe-modules.sh`
  now runs `make Makefile` straight after applying the patches: that is Wine's own
  regeneration path (the Makefile depends on `config.status` and `tools/makedep`),
  it rebuilds makedep from the patched source and re-runs it, and it is a no-op
  when the file is current. CI failed once on exactly this before the line was
  added.
- `PE_MODULES_OUT`, so the install directory is not hardcoded: the IPA workflow
  builds into `build/wine-pe-modules` and caches *that*, then copies from it.

The script already refused to overwrite a module the bundle has, refused to
touch what DXMT and the XInput patch own, aborted when the generated Makefile had
no rule for a requested target, and verified every installed module's machine
word. Those four are why the first full run came back 143/143 with 0 errors
instead of compiling Wine's own test dependencies into the bundle.

**The Microsoft half: a `x86_64-directx` component, built like the vcruntime
one.** Wine's DirectX helper libraries are reimplementations with holes exactly
where a title calls them: `d3dx11_43` declares 19 of its 44 exports `@ stub`
including `D3DX11CreateShaderResourceViewFromFile{A,W}` -- the call a DX11 game
makes to load a texture -- and all four async processors and all six
`D3DX11PreprocessShader*`; `d3dx10_43` stubs 26 of 176, keeping both
`D3DX10CreateShaderResourceViewFromFile{A,W}`; `d3dcompiler_47` stubs 10 of 29
(`D3DCompile2`, the `d3dcompiler_47`-era entry points). A stub is worse than a
missing module: it answers `E_NOTIMPL`, so the module loads, the title starts,
and it dies at the first texture -- the "missing DLL" symptom one level down.

`tools/extract-directx.py` + `tools/fetch-directx.sh` produce the real ones from
the June 2010 DirectX redistributable, and `tools/directx-component.txt` is the
list (16 modules, 15.9 MB). Details that are load-bearing:

- The redistributable is self-extracting and its cabinets nest, so the extractor
  descends into them and proves the machine word of every file it installs -- a
  wrong arch here is a load failure inside the guest, not an error at build time.
- x64 only. The component is overlaid onto the x64 session's `system32`; an ARM64
  copy would be a module the ARM64 guest loads *instead of* the ARM64EC one,
  which is the class of mistake the vcruntime exemptions exist for.
- Names are lowercased on install. The cabinet spells some of them
  `D3DCompiler_43.dll`, the rest of the bundle is lower case, and the packaged
  bundle is read by name out of a zip (`tools/validate-ios-bundle.py`), where
  the difference is a hit or a miss.
- Nothing in the list replaces a module DXMT owns (`d3d11`, `dxgi`,
  `d3d10core`, `winemetal`), and `tools/check-pe-module-set.py` fails if one is
  ever added. The overlay runs *after* the bundle symlinks and unlinks first, so
  Microsoft's `d3dx11_43` is what the guest gets, not Wine's stub.

The risk to watch, and the fix if it shows: this is an x86_64 Microsoft DLL
running under FEX, which is the arrangement that killed `msvcp140` (its C++
throw path corrupted guest RSP; see the exemption notes in
`WineProcessBridge.m`). `d3dx11_43` is C with no exception path, so it is
expected to be fine -- but if DX11 titles start failing at the first texture
*after* this component ships, the first thing to try is exempting that module in
the overlay table and keeping the stub, which is a one-line change.

**The gates now say what "complete" means.** `check-pe-module-set.py` accounts
for every manifest entry in one of three ways: a Wine module the bundle should
have built, a native module the DirectX component provides, or a native module
with no source anywhere in the tree (`dpmodemx`, `wmcodecdspuuid` -- reported in
the summary and never failed, because there is nothing to fail *on*).
`--strict` fails when either of the first two is incomplete, and it is now run by
`make-ipa.sh` before packaging as well as by both workflows, so a local release
build and the release are the same build. `check-dll-aliases.py` verifies alias
exports against a reference set generated from Wine's `.spec`, and
`validate-ios-bundle.py` reads `tools/directx-component.txt` and requires all 16
modules, x64, in the *packaged* app -- so "the runtime is in the IPA" is a
property the gate checks rather than a property of the build script.

**CI builds the thing it ships.** `ipa.yml` restores a
`build/wine-pe-modules` cache keyed on the Wine revision, the manifest and the
build script; builds and installs the 143 modules; runs the two fetchers; runs
`--strict`; then builds and packages. Nothing about the release depends on
someone having committed the right binaries. `.github/workflows/
wine-pe-modules.yml` stays as the standalone inspector (`--force` rebuild, one
artifact of both directories), and `build-ipa.yml` is unchanged: it still wraps
`ipa.yml`, and still publishes an unsigned IPA with a Release on push/dispatch.

What was deliberately not done: committing the module set (161 MB, valid for one
Wine revision); a `dxvk.conf` full of `dxvk.*` keys (ml806 -- DXMT does not read
them, and a config file that promises more than the layer implements is worse
than no file); and closing the ARM64 DirectX 9 asymmetry (`d3dx9_43`,
`d3dcompiler_43/47` still exist only for x64). That last one is real but not
reachable: DX11 requires DXMT's x64 path, and an ARM64-native DX9 title is not a
thing that exists in the wild.

## Handing an unsigned IPA to the user

The build lives in CI, so "give me an IPA" is three steps, and the third is the
one that is easy to forget — the artifact is invisible until something serves
it:

1. Push the branch to the fork (`nwaf92641/Madeira`). `.github/workflows/*ipa*`
   builds it on a hosted macOS runner; gates run first, so a red gate means no
   IPA at all.
2. Download the run's `Madeira-unsigned-ipa` artifact through the API and unzip
   it (`curl -L .../actions/artifacts/<id>/zip`).
3. Put the `.ipa` in `/workspace/project/ipa-dist/` and serve that directory on
   port 12000 (`python3 -m http.server 12000 --bind 0.0.0.0`). The runtime maps
   that port to `https://work-1-rwoqahycgtqbslzr.prod-runtime.all-hands.dev/`,
   which is the link the user can actually open on the device. Restart the
   server after any conversation restart; it does not survive one.

`index.html` in that directory is the download page. Keep its build sha and
SHA-256 in step with the file, and verify the served copy with `sha256sum` and a
`Content-Length` check — a stale page over a new IPA is worse than no page.

Pushing needs a credential that can write. The environment's own
`GITHUB_TOKEN` is a GitHub App token that authenticates and can read
(including CI logs and artifacts) but is denied writes with "Resource not
accessible by integration", so a push with it fails. When the user supplies a
PAT, use it for the push only; do not record it in this file or anywhere else
in the repo.

The artifact is unsigned: sideload it (AltStore, Sideloadly, TrollStore) or
re-sign it. Nothing in the repo signs it.

## Release readiness

- Bundle id must stay `com.madeira.emulator` in the Xcode project, matching
  `app/source.json` and `scripts/deploy-thumper.sh`. A mismatch silently points
  sideload tooling at the wrong app.
- `Madeira.entitlements` deliberately carries `get-task-allow` (needed for the
  debugger to attach), `increased-memory-limit` and `allow-jit`.
  `source.json` also requests `extended-virtual-addressing`, which free Apple
  IDs cannot provision — it is injected post-install (see GetMoreRam notes in
  `ARCHITECTURE_ANALYSIS.md`), so do not add it to the entitlements file or a
  free-account signature will fail.
- `app/source.json` (`downloadURL`, `iconURL`, `size`) is still placeholder
  data and must be filled in before an official release.
