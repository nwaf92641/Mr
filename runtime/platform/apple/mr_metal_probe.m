#import "mr_metal_probe.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

#include <string.h>

/*
 * Every capability below sits behind the SDK version macro for the release that
 * introduced that symbol, so building against an older SDK yields a smaller
 * answer instead of a compile error. The availability is then checked again at
 * run time, because the SDK is a build-time fact and the OS version is not: an
 * SDK that knows MTLGPUFamilyMetal4 still compiles an app that runs on iPadOS
 * 18, where asking the device about Metal 4 would be asking the wrong question.
 */
static void mr_metal_copy(char *dst, size_t cap, NSString *src) {
  if (dst == NULL || cap == 0) return;
  dst[0] = '\0';
  if (src == nil) return;
  const char *utf8 = [src UTF8String];
  if (utf8 == NULL) return;
  size_t n = strlen(utf8);
  if (n >= cap) n = cap - 1;
  memcpy(dst, utf8, n);
  dst[n] = '\0';
}

void mr_metal_probe(mr_metal_probe_result *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) return;

  out->device_present = true;
  mr_metal_copy(out->device_name, sizeof(out->device_name), device.name);

  /* Limits that have been on MTLDevice since its first release. */
  out->max_buffer_length = (uint64_t)device.maxBufferLength;
  out->max_threads_per_threadgroup =
      (size_t)device.maxThreadsPerThreadgroup.width;
  out->argument_buffers_tier2 =
      device.argumentBuffersSupport == MTLArgumentBuffersTier2;

#if defined(__IPHONE_12_0) || defined(__MAC_10_14)
  if (@available(iOS 12.0, macOS 10.14, *)) {
    out->max_threadgroup_memory = (uint64_t)device.maxThreadgroupMemoryLength;
  }
#endif

#if defined(__IPHONE_13_0) || defined(__MAC_10_15)
  if (@available(iOS 13.0, macOS 10.15, *)) {
    out->unified_memory = device.hasUnifiedMemory != NO;
  }
#endif

  /*
   * The Apple family is recorded as the highest one the device reports, so the
   * number means here what it means in Apple's tables. Each step is guarded by
   * the SDK that introduced it -- Apple9 arrived with the iOS 17 headers, Apple8
   * with iOS 16, Apple7 with iOS 14 -- because asking about a family the headers
   * predate does not compile.
   */
#if defined(__IPHONE_14_0) || defined(__MAC_11_0)
  if (@available(iOS 14.0, macOS 11.0, *)) {
    if ([device supportsFamily:MTLGPUFamilyApple7]) {
      out->gpu_family = (int)MTLGPUFamilyApple7;
    }
  }
#endif
#if defined(__IPHONE_16_0) || defined(__MAC_13_0)
  if (@available(iOS 16.0, macOS 13.0, *)) {
    if ([device supportsFamily:MTLGPUFamilyApple8]) {
      out->gpu_family = (int)MTLGPUFamilyApple8;
    }
    out->metal3_available = [device supportsFamily:MTLGPUFamilyMetal3];
  }
#endif
#if defined(__IPHONE_17_0) || defined(__MAC_14_0)
  if (@available(iOS 17.0, macOS 14.0, *)) {
    if ([device supportsFamily:MTLGPUFamilyApple9]) {
      out->gpu_family = (int)MTLGPUFamilyApple9;
    }
  }
#endif

#if defined(__IPHONE_16_4) || defined(__MAC_11_0)
  if (@available(iOS 16.4, macOS 11.0, *)) {
    out->supports_bc_textures = device.supportsBCTextureCompression != NO;
  }
#endif

#if defined(__IPHONE_14_0) || defined(__MAC_11_0)
  if (@available(iOS 14.0, macOS 11.0, *)) {
    /*
     * Whether the tracing is hardware-accelerated is a separate question that
     * Apple answers in its GPU family tables rather than through a device query,
     * so it is deliberately not folded into this value.
     */
    out->supports_hardware_raytracing = device.supportsRaytracing != NO;
  }
#endif

#if defined(__IPHONE_26_0) || defined(__MAC_26_0)
  if (@available(iOS 26.0, macOS 26.0, *)) {
    /*
     * Two independent answers, and both are required.
     *
     * `supportsFamily:MTLGPUFamilyMetal4` is the OS and the GPU reporting the
     * feature set, and responding to `newCommandAllocator` is the device being
     * able to create the object the whole ABI is built around. Apple's support
     * list puts Metal 4 on A14 and later, and a device can satisfy one of these
     * without the other; trusting either alone turns a missing capability into a
     * nil object deep inside the backend, far from the cause.
     */
    bool family = [device supportsFamily:MTLGPUFamilyMetal4];
    bool allocator =
        [device respondsToSelector:@selector(newCommandAllocator)];
    out->metal4_available = family && allocator;
  }
#endif

  /*
   * Metal 4 implies Metal 3. The MTL4 objects are built on the device and queue
   * model Metal 3 introduced, and a device claiming the newer family but not the
   * older one would make the backend's fallback chain unreachable.
   */
  if (out->metal4_available) out->metal3_available = true;

  /*
   * Mesh shaders are reported false on purpose.
   *
   * Metal has no MTLDevice member that answers this -- the answer lives in
   * Apple's feature-set tables, which are not a runtime query. The portable core
   * reads a false here as "do not take the mesh path", so being wrong in this
   * direction costs a translation, while being wrong in the other direction asks
   * the GPU for something it may not have. The value belongs here the day the
   * mesh path exists and can be checked against a device.
   */
  out->supports_mesh_shaders = false;
}
