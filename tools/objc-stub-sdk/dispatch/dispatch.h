/* Stand-in for dispatch/dispatch.h: only what this project passes to Metal. */
#ifndef MR_STUB_DISPATCH_H
#define MR_STUB_DISPATCH_H

#include <stddef.h>
#include <Foundation/Foundation.h>

#ifndef OS_OBJECT_USE_OBJC
/* Left at 1, which is what a modern Apple SDK sets for an Objective-C
 * translation unit. The backend has an #if on this, and both branches have to be
 * reachable, so tools/check-objc.sh compiles the file twice with it forced each
 * way. */
#define OS_OBJECT_USE_OBJC 1
#endif

typedef id dispatch_data_t;
typedef id dispatch_queue_t;

#define DISPATCH_DATA_DESTRUCTOR_DEFAULT ((void *)0)

dispatch_data_t dispatch_data_create(const void *buffer, size_t size,
                                     dispatch_queue_t queue, void *destructor);

#if !OS_OBJECT_USE_OBJC
void dispatch_release(dispatch_data_t object);
#endif

#endif /* MR_STUB_DISPATCH_H */
