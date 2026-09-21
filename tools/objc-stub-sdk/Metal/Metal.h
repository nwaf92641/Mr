/* Stand-in for Metal/Metal.h. The members declared here are the ones this
 * project uses, spelled as Apple's documentation spells them: a check against a
 * stub only means something if the stub is not more forgiving than the real
 * header. See ../README.md. */
#ifndef MR_STUB_METAL_H
#define MR_STUB_METAL_H

#include <Foundation/Foundation.h>
#include <dispatch/dispatch.h>

typedef NSUInteger MTLPixelFormat;
typedef NSUInteger MTLTextureUsage;
typedef NSUInteger MTLTextureType;
typedef NSUInteger MTLStorageMode;
typedef NSUInteger MTLResourceOptions;
typedef NSUInteger MTLResourceUsage;
typedef NSUInteger MTLCommandBufferStatus;
typedef NSUInteger MTLLoadAction;
typedef NSUInteger MTLStoreAction;
typedef NSUInteger MTLPrimitiveType;
typedef NSUInteger MTLIndexType;
typedef NSUInteger MTLGPUFamily;
typedef NSUInteger MTLArgumentBuffersTier;

typedef struct {
  NSUInteger width, height, depth;
} MTLSize;

typedef struct {
  NSUInteger x, y, z;
} MTLOrigin;

typedef struct {
  MTLOrigin origin;
  MTLSize size;
} MTLRegion;

typedef struct {
  double red, green, blue, alpha;
} MTLClearColor;

static inline MTLRegion MTLRegionMake2D(NSUInteger x, NSUInteger y, NSUInteger w,
                                        NSUInteger h) {
  MTLRegion r;
  r.origin.x = x;
  r.origin.y = y;
  r.origin.z = 0;
  r.size.width = w;
  r.size.height = h;
  r.size.depth = 1;
  return r;
}

static inline MTLClearColor MTLClearColorMake(double r, double g, double b,
                                              double a) {
  MTLClearColor c;
  c.red = r;
  c.green = g;
  c.blue = b;
  c.alpha = a;
  return c;
}

enum {
  MTLPixelFormatInvalid = 0,
  MTLPixelFormatR8Unorm = 10,
  MTLPixelFormatRG8Unorm = 30,
  MTLPixelFormatRGBA8Unorm = 70,
  MTLPixelFormatRGBA8Unorm_sRGB = 71,
  MTLPixelFormatBGRA8Unorm = 80,
  MTLPixelFormatBGRA8Unorm_sRGB = 81,
  MTLPixelFormatR16Float = 63,
  MTLPixelFormatRG16Float = 65,
  MTLPixelFormatRGBA16Float = 115,
  MTLPixelFormatR32Float = 10,
  MTLPixelFormatRG32Float = 105,
  MTLPixelFormatRGBA32Float = 125,
  MTLPixelFormatR32Uint = 53,
  MTLPixelFormatR32Sint = 54,
  MTLPixelFormatDepth16Unorm = 250,
  MTLPixelFormatDepth32Float = 252,
  MTLPixelFormatDepth24Unorm_Stencil8 = 255,
  MTLPixelFormatDepth32Float_Stencil8 = 260,
  MTLPixelFormatBC1_RGBA = 130,
  MTLPixelFormatBC2_RGBA = 132,
  MTLPixelFormatBC3_RGBA = 134,
  MTLPixelFormatBC4_RUnorm = 140,
  MTLPixelFormatBC5_RGUnorm = 142,
  MTLPixelFormatBC6H_RGBUfloat = 150,
  MTLPixelFormatBC7_RGBAUnorm = 152,
};

enum {
  MTLTextureUsageUnknown = 0x0,
  MTLTextureUsageShaderRead = 0x1,
  MTLTextureUsageShaderWrite = 0x2,
  MTLTextureUsageRenderTarget = 0x4,
  MTLTextureUsagePixelFormatView = 0x10,
};

enum {
  MTLTextureType2D = 2,
  MTLTextureType2DArray = 3,
  MTLTextureType2DMultisample = 4,
  MTLTextureType3D = 7,
  MTLTextureType2DMultisampleArray = 8,
};

enum {
  MTLStorageModeShared = 0,
  MTLStorageModeManaged = 1,
  MTLStorageModePrivate = 2,
  MTLStorageModeMemoryless = 3,
};

enum {
  MTLResourceStorageModeShared = 0 << 4,
  MTLResourceStorageModePrivate = 2 << 4,
};

enum {
  MTLResourceUsageRead = 1 << 0,
  MTLResourceUsageWrite = 1 << 1,
  MTLResourceUsageSample = 1 << 2,
};

typedef NSUInteger MTLRenderStages;

enum {
  MTLRenderStageVertex = 1 << 0,
  MTLRenderStageFragment = 1 << 1,
  MTLRenderStageTile = 1 << 2,
  MTLRenderStageObject = 1 << 3,
  MTLRenderStageMesh = 1 << 4,
};

enum {
  MTLCommandBufferStatusNotEnqueued = 0,
  MTLCommandBufferStatusEnqueued = 1,
  MTLCommandBufferStatusCommitted = 2,
  MTLCommandBufferStatusScheduled = 3,
  MTLCommandBufferStatusCompleted = 4,
  MTLCommandBufferStatusError = 5,
};

enum {
  MTLLoadActionDontCare = 0,
  MTLLoadActionLoad = 1,
  MTLLoadActionClear = 2,
};

enum {
  MTLStoreActionDontCare = 0,
  MTLStoreActionStore = 1,
};

enum {
  MTLPrimitiveTypePoint = 0,
  MTLPrimitiveTypeLine = 1,
  MTLPrimitiveTypeTriangle = 3,
};

enum {
  MTLIndexTypeUInt16 = 0,
  MTLIndexTypeUInt32 = 1,
};

enum {
  MTLGPUFamilyApple1 = 1001,
  MTLGPUFamilyApple9 = 1009,
  MTLGPUFamilyMac2 = 2002,
  MTLGPUFamilyCommon3 = 3003,
};

enum {
  MTLArgumentBuffersTier1 = 0,
  MTLArgumentBuffersTier2 = 1,
};

@class MTLRenderPassDescriptor;

@protocol MTLResource;
@protocol MTLTexture;
@protocol MTLBuffer;
@protocol MTLFunction;
@protocol MTLLibrary;
@protocol MTLCommandQueue;
@protocol MTLCommandBuffer;
@protocol MTLRenderCommandEncoder;
@protocol MTLBlitCommandEncoder;
@protocol MTLComputeCommandEncoder;
@protocol MTLRenderPipelineState;
@protocol MTLComputePipelineState;
@protocol MTLSamplerState;
@protocol MTLHeap;
@protocol MTLFence;
@protocol MTLEvent;
@protocol MTLSharedEvent;
@protocol MTLDrawable;
@protocol MTLBinaryArchive;
@protocol MTLCommandAllocator;
@protocol MTLRenderPipelineState;

@protocol MTLResource <NSObject>
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (MTLStorageMode)storageMode;
- (NSUInteger)length;
@end

@protocol MTLBuffer <MTLResource>
- (void *)contents;
@end

@protocol MTLTexture <MTLResource>
- (MTLPixelFormat)pixelFormat;
- (NSUInteger)width;
- (NSUInteger)height;
- (NSUInteger)depth;
- (NSUInteger)mipmapLevelCount;
- (NSUInteger)sampleCount;
- (MTLTextureType)textureType;
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)pixelBytes
          bytesPerRow:(NSUInteger)bytesPerRow;
- (void)getBytes:(void *)pixelBytes
     bytesPerRow:(NSUInteger)bytesPerRow
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level;
@end

@protocol MTLFunction <NSObject>
- (NSString *)name;
@end

@protocol MTLLibrary <NSObject>
- (id<MTLFunction>)newFunctionWithName:(NSString *)functionName;
@end

@protocol MTLSamplerState <NSObject>
@end

@protocol MTLHeap <NSObject>
@end

@protocol MTLRenderPipelineState <NSObject>
@end

@protocol MTLComputePipelineState <NSObject>
@end

/* The label is here, not on the encoders and not on the pass descriptor. That
 * distinction is the one the real SDK enforced: this stub previously put a label
 * on MTLRenderPassDescriptor, the backend believed it, and the first Apple build
 * of metal/mr_backend_metal3.m failed with "property 'label' not found on object
 * of type 'MTLRenderPassDescriptor *'". */
@protocol MTLCommandEncoder <NSObject>
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (void)endEncoding;
@end

@protocol MTLRenderCommandEncoder <MTLCommandEncoder>
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState;
- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index;
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler
                      atIndex:(NSUInteger)index;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler
                        atIndex:(NSUInteger)index;
- (void)useResource:(id<MTLResource>)resource
              usage:(MTLResourceUsage)usage
             stages:(MTLRenderStages)stages;
- (void)useResource:(id<MTLResource>)resource
              usage:(MTLResourceUsage)usage
    __attribute__((deprecated("Use useResource:usage:stages: instead")));
- (void)useHeap:(id<MTLHeap>)heap;
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
         instanceCount:(NSUInteger)instanceCount;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset;
- (void)endEncoding;
@end

@protocol MTLBlitCommandEncoder <MTLCommandEncoder>
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture
              toTexture:(id<MTLTexture>)destinationTexture;
- (void)endEncoding;
@end

@protocol MTLCommandBuffer <NSObject>
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor *)renderPassDescriptor;
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (void)commit;
- (void)waitUntilCompleted;
- (MTLCommandBufferStatus)status;
- (NSError *)error;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)setLabel:(NSString *)label;
@end

@protocol MTLCommandQueue <NSObject>
- (id<MTLCommandBuffer>)commandBuffer;
- (void)setLabel:(NSString *)label;
@end

@protocol MTLDrawable <NSObject>
- (id<MTLTexture>)texture;
@end

@interface MTLTextureDescriptor : NSObject
@property(nonatomic) MTLPixelFormat pixelFormat;
@property(nonatomic) NSUInteger width;
@property(nonatomic) NSUInteger height;
@property(nonatomic) NSUInteger depth;
@property(nonatomic) NSUInteger mipmapLevelCount;
@property(nonatomic) NSUInteger arrayLength;
@property(nonatomic) NSUInteger sampleCount;
@property(nonatomic) MTLTextureUsage usage;
@property(nonatomic) MTLStorageMode storageMode;
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic, copy) NSString *label;
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:
                                  (MTLPixelFormat)pixelFormat
                                                       width:(NSUInteger)width
                                                      height:(NSUInteger)height
                                                   mipmapped:(BOOL)mipmapped;
@end

@interface MTLRenderPassColorAttachmentDescriptor : NSObject
@property(nonatomic, retain) id<MTLTexture> texture;
@property(nonatomic) MTLLoadAction loadAction;
@property(nonatomic) MTLStoreAction storeAction;
@property(nonatomic) MTLClearColor clearColor;
@end

@interface MTLRenderPassDepthAttachmentDescriptor : NSObject
@property(nonatomic, retain) id<MTLTexture> texture;
@property(nonatomic) MTLLoadAction loadAction;
@property(nonatomic) MTLStoreAction storeAction;
@property(nonatomic) double clearDepth;
@end

@interface MTLRenderPassStencilAttachmentDescriptor : NSObject
@property(nonatomic, retain) id<MTLTexture> texture;
@property(nonatomic) MTLLoadAction loadAction;
@property(nonatomic) MTLStoreAction storeAction;
@property(nonatomic) uint32_t clearStencil;
@end

@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor *)objectAtIndexedSubscript:
    (NSUInteger)attachmentIndex;
- (void)setObject:(MTLRenderPassColorAttachmentDescriptor *)attachment
    atIndexedSubscript:(NSUInteger)attachmentIndex;
@end

@interface MTLRenderPassDescriptor : NSObject
@property(readonly) MTLRenderPassColorAttachmentDescriptorArray *colorAttachments;
@property(readonly) MTLRenderPassDepthAttachmentDescriptor *depthAttachment;
@property(readonly) MTLRenderPassStencilAttachmentDescriptor *stencilAttachment;
@property(nonatomic, copy) NSString *label;
+ (MTLRenderPassDescriptor *)renderPassDescriptor;
@end

@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
@property(nonatomic) MTLPixelFormat pixelFormat;
@end

@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject
- (MTLRenderPipelineColorAttachmentDescriptor *)objectAtIndexedSubscript:
    (NSUInteger)attachmentIndex;
@end

@interface MTLRenderPipelineDescriptor : NSObject
@property(nonatomic, copy) NSString *label;
@property(nonatomic, retain) id<MTLFunction> vertexFunction;
@property(nonatomic, retain) id<MTLFunction> fragmentFunction;
@property(readonly) MTLRenderPipelineColorAttachmentDescriptorArray *colorAttachments;
@property(nonatomic) MTLPixelFormat depthAttachmentPixelFormat;
@property(nonatomic) MTLPixelFormat stencilAttachmentPixelFormat;
@property(nonatomic) NSUInteger rasterSampleCount;
@property(nonatomic) BOOL alphaToCoverageEnabled;
@end

@protocol MTLDevice <NSObject>
- (NSString *)name;
- (BOOL)hasUnifiedMemory;
- (BOOL)supportsBCTextureCompression;
- (BOOL)supportsRaytracing;
- (BOOL)supportsFamily:(MTLGPUFamily)family;
- (NSUInteger)maxBufferLength;
- (NSUInteger)maxThreadgroupMemoryLength;
- (MTLSize)maxThreadsPerThreadgroup;
- (MTLArgumentBuffersTier)argumentBuffersSupport;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length
                             options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytes:(const void *)pointer
                             length:(NSUInteger)length
                            options:(MTLResourceOptions)options;
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data
                               error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                               options:(id)options
                                 error:(NSError **)error;
- (id<MTLRenderPipelineState>)
    newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                   error:(NSError **)error;
- (id<MTLHeap>)newHeapWithDescriptor:(id)descriptor;
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);

#endif /* MR_STUB_METAL_H */
