/*
 * What Metal 4 on this machine actually is.
 *
 * This probe exists to answer one question with evidence instead of memory: which
 * Metal 4 names exist in the SDK this was compiled against, and which of them this
 * device and OS answer for at runtime. Nothing here is written from recollection
 * of the API. Names that might not exist are looked up as strings and reported
 * present or absent; headers that might not exist are tested with __has_include.
 *
 * The distinction it keeps: a name being present in the SDK is a build-time fact,
 * and it means the API can be named in Mr's code. A name being answered at runtime
 * is a device and OS fact, and it means the API can be used on this machine. A
 * backend needs both, and the reason a Metal 4 path is unavailable is usually one
 * of the two rather than the other.
 *
 * Output is line-oriented key=value so the workflow can tee it into the evidence
 * and diff it between machines.
 *
 * Exit status:
 *   0   ran on a real Apple GPU that answers for Metal 4
 *   77  ran, but this machine cannot verify Metal 4 (the CI runner's paravirtual
 *       device is exactly this case): REQUIRES REAL APPLE GPU VALIDATION
 *   1   the probe could not run
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void kv(const char *key, const char *value) {
  printf("%s=%s\n", key, value);
}

static void kv_bool(const char *key, bool value) {
  kv(key, value ? "yes" : "no");
}

/* A name this code cannot name at compile time, because it might not be in the SDK
 * this is built against. Looking it up as a string answers the question directly
 * and cannot be wrong about a signature. */
static void report_class(const char *name) {
  Class c = NSClassFromString([NSString stringWithUTF8String:name]);
  printf("sdk_class.%s=%s\n", name, c != nil ? "present" : "absent");
}

static void report_selector(id object, const char *name) {
  SEL sel = NSSelectorFromString([NSString stringWithUTF8String:name]);
  if (sel != NULL && [object respondsToSelector:sel]) {
    printf("device_selector.%s=yes\n", name);
  } else {
    printf("device_selector.%s=no\n", name);
  }
}

int main(void) {
  @autoreleasepool {
    printf("## what the SDK was compiled with\n");
#if defined(__MAC_26_0)
    kv_bool("sdk.macos_26", true);
#else
    kv_bool("sdk.macos_26", false);
#endif
#if defined(__IPHONE_26_0)
    kv_bool("sdk.ios_26", true);
#else
    kv_bool("sdk.ios_26", false);
#endif
    kv("sdk.macos_version_compiled_against", __MAC_OS_X_VERSION_MAX_ALLOWED
                                               ? "numeric value below"
                                               : "unknown");
    printf("sdk.macos_version_max_allowed=%d\n",
           __MAC_OS_X_VERSION_MAX_ALLOWED);

    /*
     * Candidates, tested rather than assumed. Which of these the SDK has is the
     * build-time half of the answer, and a wrong guess shows up here as absent
     * rather than as a compile error three files away.
     */
    /*
     * __has_include is a preprocessing operator, not a function. The first version
     * of this file passed it to a function and the compiler rejected it outright --
     * "must be used within a preprocessing directive" -- so the probe never ran and
     * the job failed on the compile. The lesson is the project's own rule: a probe
     * that cannot be built cannot measure anything, and the failure looks like a
     * verdict about Metal 4 when it is a syntax error.
     *
     * One candidate is asked here rather than the whole list. The workflow's own
     * step tests all 31 MTL4 headers individually and reports each one, which is
     * the better measurement, and duplicating it here only added a way to fail.
     */
#if __has_include(<Metal/MTL4CommandQueue.h>)
    kv_bool("compile_time.mtl4_command_queue_header", true);
#else
    kv_bool("compile_time.mtl4_command_queue_header", false);
#endif

    printf("\n## candidate class names at runtime\n");
    report_class("MTL4CommandQueue");
    report_class("MTL4CommandQueueDescriptor");
    report_class("MTL4CommandBuffer");
    report_class("MTL4CommandAllocator");
    report_class("MTL4RenderCommandEncoder");
    report_class("MTL4ComputeCommandEncoder");
    report_class("MTL4ArgumentTable");
    report_class("MTL4ArgumentTableDescriptor");
    report_class("MTL4Compiler");
    report_class("MTL4LibraryFunctionDescriptor");
    report_class("MTL4RenderPipelineDescriptor");
    report_class("MTL4ComputePipelineDescriptor");

    printf("\n## devices\n");
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    if (devices == nil || devices.count == 0) {
      id<MTLDevice> only = MTLCreateSystemDefaultDevice();
      devices = only != nil ? @[ only ] : @[];
    }
    printf("device_count=%lu\n", (unsigned long)devices.count);
    if (devices.count == 0) {
      kv("result", "no Metal device is available to this process");
      return 77;
    }

    bool real_apple_gpu = false;
    bool metal4 = false;
    for (id<MTLDevice> device in devices) {
      NSString *name = device.name ?: @"unknown";
      printf("device=%s\n", name.UTF8String);
      bool paravirtual = [name containsString:@"Paravirtual"];
      printf("device.%s.paravirtual=%s\n", name.UTF8String,
             paravirtual ? "yes" : "no");
      if (!paravirtual) real_apple_gpu = true;

      /* The same two answers mr_metal_probe.m requires, asked again here so this
       * probe stands on its own. A GPU family is the feature set the OS and GPU
       * report; responding to newCommandAllocator is this device being able to
       * create the object the whole Metal 4 command model is built on. */
      bool family = false;
      if (@available(macOS 26.0, iOS 26.0, *)) {
        family = [device supportsFamily:MTLGPUFamilyMetal4];
      }
      printf("device.%s.metal4_family=%s\n", name.UTF8String,
             family ? "yes" : "no");
      report_selector(device, "newCommandAllocator");
      report_selector(device, "newCommandQueue");
      if (family && [device respondsToSelector:NSSelectorFromString(
                             @"newCommandAllocator")]) {
        metal4 = true;
      }
    }

    kv_bool("real_apple_gpu", real_apple_gpu);
    kv_bool("metal4_available", metal4);

    printf("\n## verdict\n");
    if (!real_apple_gpu) {
      kv("result", "REQUIRES REAL APPLE GPU VALIDATION: this machine can build the "
                   "Metal 4 path and cannot verify it");
      return 77;
    }
    if (!metal4) {
      kv("result", "UNSUPPORTED TARGET: a real Apple GPU without Metal 4");
      return 77;
    }
    kv("result", "a real Apple GPU that answers for Metal 4");
    return 0;
  }
}
