/*
 * MetalFX availability. See mr_metalfx_probe.h for why this is its own file.
 *
 * Every answer here comes from asking the device, through the availability
 * query Apple documents for each effect:
 *
 *   +[MTLFXSpatialScalerDescriptor supportsDevice:]
 *   +[MTLFXTemporalScalerDescriptor supportsDevice:]
 *   +[MTLFXTemporalDenoisedScalerDescriptor supportsDevice:]
 *
 * All three are in iOS/iPadOS 16 and macOS 13, which is below the project's
 * deployment floor, but the class is looked up by name and asked whether it
 * responds rather than called directly. That keeps a weak-linked or absent
 * framework out of the crash path: an unloaded class resolves to Nil.
 *
 * Frame interpolation is deliberately not queried. MTLFXFrameInterpolatorDescriptor
 * exists, but its availability query was not confirmed against Apple's
 * documentation, and a capability flag guessed from a class name is worse than a
 * flag that is absent.
 */

#import "mr_metalfx_probe.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

static bool descriptor_supports(id<MTLDevice> device, NSString *name) {
  if (device == nil) {
    return false;
  }
  Class descriptor = NSClassFromString(name);
  if (descriptor == Nil) {
    return false;
  }
  if (![descriptor respondsToSelector:@selector(supportsDevice:)]) {
    return false;
  }
  return (bool)[descriptor supportsDevice:device];
}

void mr_metalfx_probe(mr_metalfx_caps *out) {
  if (out == NULL) {
    return;
  }
  out->spatial = false;
  out->temporal = false;
  out->denoise = false;

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return;
  }

  out->spatial =
      descriptor_supports(device, @"MTLFXSpatialScalerDescriptor");
  out->temporal =
      descriptor_supports(device, @"MTLFXTemporalScalerDescriptor");
  out->denoise =
      descriptor_supports(device, @"MTLFXTemporalDenoisedScalerDescriptor");

#if !__has_feature(objc_arc)
  [device release];
#endif
}
