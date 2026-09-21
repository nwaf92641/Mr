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
