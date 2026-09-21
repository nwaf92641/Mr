/* Stand-in for QuartzCore/CAMetalLayer.h. */
#ifndef MR_STUB_CAMETALLAYER_H
#define MR_STUB_CAMETALLAYER_H

#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

/* Apple declares CAMetalDrawable as a protocol, not a class, so a caller holds
 * id<CAMetalDrawable>. Declaring it as a class here would accept code on this
 * host that a Mac rejects. */
@protocol CAMetalDrawable <MTLDrawable>
- (id<MTLTexture>)texture;
@end

@interface CAMetalLayer : NSObject
@property(nonatomic, retain) id<MTLDevice> device;
@property(nonatomic) MTLPixelFormat pixelFormat;
@property(nonatomic) CGSize drawableSize;
@property(nonatomic) BOOL framebufferOnly;
+ (instancetype)layer;
- (id<CAMetalDrawable>)nextDrawable;
@end

#endif /* MR_STUB_CAMETALLAYER_H */
