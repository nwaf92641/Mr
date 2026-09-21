/* Stand-in for TargetConditionals.h.
 *
 * Defaults to macOS. tools/check-objc.sh also compiles with TARGET_OS_OSX forced
 * to 0, so the iOS-only branches in the backend are type-checked too rather than
 * being dead code that only a device would ever compile. */
#ifndef MR_STUB_TARGETCONDITIONALS_H
#define MR_STUB_TARGETCONDITIONALS_H

#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 1
#endif

#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif

#endif /* MR_STUB_TARGETCONDITIONALS_H */
