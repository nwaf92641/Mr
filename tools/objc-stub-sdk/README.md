# A stub Apple SDK, so the Objective-C can be type-checked off a Mac

This is a set of header stubs, not a port of anything. It exists for exactly one
purpose: to let a Linux host run `clang -fsyntax-only` over this project's
Objective-C, which is otherwise compiled nowhere. The development host has no
Apple SDK, and GitHub does not register workflows from a non-default branch, so
a pull request adding an Apple CI job cannot use it.

Run it with:

```sh
tools/check-objc.sh
```

## What a green run proves

- The Objective-C parses: syntax, message sends, property access,
  `@autoreleasepool`.
- The C inside those files type-checks under the project's own warning set
  (`-Wall -Wextra -Wconversion -Wsign-conversion -Werror` and the rest).
- Every vtable assignment in `metal/mr_backend_metal3.m` matches the function
  pointer type declared in `runtime/include/mr/mr_backend.h`. That header is real
  and is included unchanged, so this check is worth something: a mismatched
  signature there is a compile error on a Mac, and it is found here instead.

## What a green run does not prove

- **That these declarations match Apple's.** A method spelled the way Apple
  spells it but declared here with a different arity would pass here and fail on
  a Mac. The stubs were written from Apple's published documentation for the
  members this project actually uses, and they are worth exactly that much.
- **Anything about availability.** Nothing here knows which OS version
  introduced a symbol.
- **Anything about behaviour.** No memory management, no reference counting
  semantics, no runtime. A stub `-release` does the same as any other no-op.

The honest summary is "well-formed", not "works". The Mac is still the only
place that can say the second thing.

## What the stubs learned from the first real Apple build

These stubs are not just declarations any more. The first arm64 macOS build of
the Metal backend failed on things this directory had wrong or missing, and each
one was folded back in so the next occurrence is caught here instead:

- `MTLTextureDescriptor` and `MTLRenderPassDescriptor` have no `label`.
  `MTLCommandEncoder` does, and both encoder protocols refine it.
- `useResource:usage:` is deprecated from macOS 13 / iOS 16 and the project
  builds with `-Werror`, so the stub marks it deprecated exactly as Apple does.
  `check-objc.sh` now passes `-Werror` too, because without it that deprecation
  was a warning that scrolled past and a failed CI run.
- `MTLRenderStages` and its `MTLRenderStageVertex` / `MTLRenderStageFragment`
  values exist.

The rule that follows: when a real Apple SDK rejects something, fix the stub in
the same commit. A stub that is friendlier than the SDK is worse than no stub,
because it reports success for code that cannot build.
