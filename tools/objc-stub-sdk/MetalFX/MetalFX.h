/*
 * MetalFX, stubbed for the Objective-C syntax check that runs without a Mac.
 *
 * Only what the project calls is declared, and the declarations match Apple's
 * documented API: each descriptor answers `supportsDevice:` as a class method.
 * The availability floors (iOS/iPadOS 16, macOS 13) are in the header's comment
 * in mr_metalfx_probe.m rather than encoded here, because the probe asks whether
 * the class responds instead of testing a version.
 *
 * See tools/objc-stub-sdk/README.md: this proves the code parses and the
 * selectors exist, and proves nothing about whether the runtime works.
 */
#ifndef MR_STUB_METALFX_H
#define MR_STUB_METALFX_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@interface MTLFXSpatialScalerDescriptor : NSObject
+ (BOOL)supportsDevice:(id<MTLDevice>)device;
@end

@interface MTLFXTemporalScalerDescriptor : NSObject
+ (BOOL)supportsDevice:(id<MTLDevice>)device;
@end

@interface MTLFXTemporalDenoisedScalerDescriptor : NSObject
+ (BOOL)supportsDevice:(id<MTLDevice>)device;
@end

#endif /* MR_STUB_METALFX_H */
