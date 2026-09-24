/* Mr — Metal 4 layer, Objective-C++ implementation.
 *
 * Compiled with ARC, unlike DXMT's winemetal_unix.c. That is deliberate: the
 * objects here are handed out as opaque handles and stored in long-lived
 * structs, and ARC is the least error-prone way to get that right. The seam is
 * the C header, so the two conventions never meet.
 *
 * Every MTL4 call is inside a function marked API_AVAILABLE(ios(26.0),
 * macos(26.0)), reached only through a public wrapper that checks
 * @available first. That keeps the 26+ symbols weak-linked, so the app still
 * launches on an iOS 18 device and mr_mtl4_available() reports 0 there.
 *
 * API reference: apple/game-porting-toolkit,
 * game-porting-skills/skills/translating-to-metal4-api. Selectors and property
 * names come from that skill and from the MTL4*.h headers in the iOS 26 SDK;
 * names that could not be checked against either are not used.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mr_metal4.h"

/* The enum values in the header are bridged straight into Metal. If any of
 * them disagrees with Metal, every call site would silently pass the wrong
 * value -- so the disagreement is a build failure instead. */
static_assert((int)MR_MTL4_PRIMITIVE_POINT == (int)MTLPrimitiveTypePoint, "primitive point");
static_assert((int)MR_MTL4_PRIMITIVE_TRIANGLE == (int)MTLPrimitiveTypeTriangle, "primitive triangle");
static_assert((int)MR_MTL4_PRIMITIVE_TRIANGLE_STRIP == (int)MTLPrimitiveTypeTriangleStrip,
              "primitive triangle strip");
static_assert((int)MR_MTL4_INDEX_UINT16 == (int)MTLIndexTypeUInt16, "index uint16");
static_assert((int)MR_MTL4_INDEX_UINT32 == (int)MTLIndexTypeUInt32, "index uint32");
static_assert((int)MR_MTL4_LOAD_DONT_CARE == (int)MTLLoadActionDontCare, "load dontcare");
static_assert((int)MR_MTL4_LOAD_LOAD == (int)MTLLoadActionLoad, "load load");
static_assert((int)MR_MTL4_LOAD_CLEAR == (int)MTLLoadActionClear, "load clear");
static_assert((int)MR_MTL4_STORE_DONT_CARE == (int)MTLStoreActionDontCare, "store dontcare");
static_assert((int)MR_MTL4_STORE_STORE == (int)MTLStoreActionStore, "store store");
static_assert((int)MR_MTL4_STORE_MULTISAMPLE_RESOLVE == (int)MTLStoreActionMultisampleResolve,
              "store resolve");
static_assert((unsigned)MR_MTL4_STAGE_VERTEX == (unsigned)MTLRenderStageVertex, "stage vertex");
static_assert((unsigned)MR_MTL4_STAGE_FRAGMENT == (unsigned)MTLRenderStageFragment, "stage fragment");

#define MR_MTL4_AVAIL API_AVAILABLE(ios(26.0), macos(26.0))

/* --------------------------------------------------------------- diagnostics */

static char g_error[256] = {0};

static void mr_mtl4_fail(const char *message) {
  size_t n = 0;
  if (message != nullptr) {
    for (; message[n] != '\0' && n + 1 < sizeof(g_error); ++n) {
      g_error[n] = message[n];
    }
  }
  g_error[n] = '\0';
  fprintf(stderr, "[mr-mtl4] %s\n", g_error);
}

const char *mr_mtl4_last_error(void) { return g_error; }

/* ------------------------------------------------------------------- objects */

struct mr_mtl4_device {
  id mtl;   /* id<MTLDevice> */
  id queue; /* id<MTL4CommandQueue>, one per device, created once */
  char name[256];
};

struct mr_mtl4_table {
  id table; /* id<MTL4ArgumentTable> */
  unsigned stages;
};

struct mr_mtl4_frame {
  mr_mtl4_device *device; /* not owned */
  id allocator;           /* id<MTL4CommandAllocator> */
  id buffer;              /* id<MTL4CommandBuffer> */
  id render;              /* id<MTL4RenderCommandEncoder>, nil unless open */
  id compute;            /* id<MTL4ComputeCommandEncoder>, nil unless open */
  id completion;          /* id<MTLSharedEvent> */
  uint64_t completion_value;
  bool begun;
  bool ended;
  bool committed;
  bool in_flight;
  bool encoder_open;
  char label[128];
};

struct mr_mtl4_transient {
  id buffer; /* id<MTLBuffer> */
  uint64_t capacity;
  uint64_t offset;
};

/* -------------------------------------------------------------------- probe */

MR_MTL4_AVAIL static int available_impl(void) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return 0;
  }
  return [device supportsFamily:MTLGPUFamilyMetal4] ? 1 : 0;
}

int mr_mtl4_available(void) {
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return available_impl();
  }
  return 0;
}

/* ------------------------------------------------------------------- device */

MR_MTL4_AVAIL static mr_mtl4_device *device_create_impl(void) {
  id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
  if (mtl == nil) {
    mr_mtl4_fail("MTLCreateSystemDefaultDevice returned nil");
    return nullptr;
  }
  if (![mtl supportsFamily:MTLGPUFamilyMetal4]) {
    mr_mtl4_fail("device does not report MTLGPUFamilyMetal4");
    return nullptr;
  }
  id<MTL4CommandQueue> queue = [mtl newMTL4CommandQueue];
  if (queue == nil) {
    mr_mtl4_fail("[MTLDevice newMTL4CommandQueue] returned nil");
    return nullptr;
  }
  auto *device = new mr_mtl4_device{};
  device->mtl = mtl;
  device->queue = queue;
  const char *name = [[mtl name] UTF8String];
  if (name != nullptr) {
    strncpy(device->name, name, sizeof(device->name) - 1);
  }
  return device;
}

mr_mtl4_device *mr_mtl4_device_create(void) {
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return device_create_impl();
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_device_destroy(mr_mtl4_device *device) {
  if (device == nullptr) {
    return;
  }
  device->queue = nil;
  device->mtl = nil;
  delete device;
}

const char *mr_mtl4_device_name(mr_mtl4_device *device) {
  return device != nullptr ? device->name : "";
}

void *mr_mtl4_device_metal_device(mr_mtl4_device *device) {
  if (device == nullptr) {
    return nullptr;
  }
  return (__bridge void *)device->mtl;
}

void *mr_mtl4_device_queue(mr_mtl4_device *device) {
  if (device == nullptr) {
    return nullptr;
  }
  return (__bridge void *)device->queue;
}

bool mr_mtl4_device_add_residency_set(mr_mtl4_device *device, void *mtl_residency_set) {
  if (device == nullptr || mtl_residency_set == nullptr) {
    mr_mtl4_fail("mr_mtl4_device_add_residency_set: null argument");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    [(id<MTL4CommandQueue>)device->queue
        addResidencySet:(__bridge id<MTLResidencySet>)mtl_residency_set];
    return true;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

MR_MTL4_AVAIL static mr_mtl4_table *table_create_impl(mr_mtl4_device *device, uint32_t max_buffers,
                                                     uint32_t max_textures, uint32_t max_samplers,
                                                     unsigned stages, bool attribute_strides) {
  if (max_buffers == 0 || max_textures == 0) {
    mr_mtl4_fail("argument table needs a non-zero buffer and texture bind count");
    return nullptr;
  }
  MTL4ArgumentTableDescriptor *desc = [[MTL4ArgumentTableDescriptor alloc] init];
  desc.maxBufferBindCount = (NSUInteger)max_buffers;
  desc.maxTextureBindCount = (NSUInteger)max_textures;
  desc.maxSamplerStateBindCount = (NSUInteger)max_samplers;
  desc.supportAttributeStrides = attribute_strides ? YES : NO;
  /* Unbound slots read as null rather than whatever was in the table before. */
  desc.initializeBindings = YES;

  NSError *error = nil;
  id<MTL4ArgumentTable> table = [(id<MTLDevice>)device->mtl newArgumentTableWithDescriptor:desc
                                                                                    error:&error];
  if (table == nil) {
    mr_mtl4_fail(error != nil ? [[error localizedDescription] UTF8String]
                              : "[MTLDevice newArgumentTableWithDescriptor:error:] returned nil");
    return nullptr;
  }
  auto *wrapper = new mr_mtl4_table{};
  wrapper->table = table;
  wrapper->stages = stages;
  return wrapper;
}

mr_mtl4_table *mr_mtl4_device_new_table(mr_mtl4_device *device, uint32_t max_buffers,
                                        uint32_t max_textures, uint32_t max_samplers,
                                        unsigned stages, bool attribute_strides) {
  if (device == nullptr) {
    mr_mtl4_fail("mr_mtl4_device_new_table: null device");
    return nullptr;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return table_create_impl(device, max_buffers, max_textures, max_samplers, stages,
                             attribute_strides);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_table_destroy(mr_mtl4_table *table) {
  if (table == nullptr) {
    return;
  }
  table->table = nil;
  delete table;
}

void *mr_mtl4_table_metal_table(mr_mtl4_table *table) {
  if (table == nullptr) {
    return nullptr;
  }
  return (__bridge void *)table->table;
}

MR_MTL4_AVAIL static void table_set_buffer_impl(mr_mtl4_table *table, uint64_t gpu_address,
                                               uint32_t stride, uint32_t index, bool strided) {
  (void)stride; /* only the unimplemented strided path would use it */
  if (table == nullptr || table->table == nil) {
    return;
  }
  if (gpu_address == 0) {
    /* Metal 4 rejects a null buffer address at draw time; saying so here names
     * the binding instead of the draw. */
    mr_mtl4_fail("mr_mtl4_table_set_buffer: null gpu address");
    return;
  }
  if (strided) {
    /* MTL4ArgumentTable.h declares two setAddress: variants. The SDK rejected
     * 'setAddress:stride:atIndex:', so this one is left unimplemented rather
     * than guessed a second time -- a wrong selector here would bind a wrong
     * stride and look like geometry corruption, not like a bug in binding.
     * check-metal4.sh prints the real signatures; implement from those. */
    mr_mtl4_fail("mr_mtl4_table_set_buffer_strided: strided binding is not implemented yet; "
                 "the SDK's strided setAddress: signature has not been read");
    return;
  }
  [(id<MTL4ArgumentTable>)table->table setAddress:(MTLGPUAddress)gpu_address
                                          atIndex:(NSUInteger)index];
}

void mr_mtl4_table_set_buffer(mr_mtl4_table *table, uint64_t gpu_address, uint32_t index) {
  if (@available(iOS 26.0, macOS 26.0, *)) {
    table_set_buffer_impl(table, gpu_address, 0, index, false);
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_table_set_buffer_strided(mr_mtl4_table *table, uint64_t gpu_address, uint32_t stride,
                                      uint32_t index) {
  if (@available(iOS 26.0, macOS 26.0, *)) {
    table_set_buffer_impl(table, gpu_address, stride, index, true);
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_table_set_texture(mr_mtl4_table *table, void *mtl_texture, uint32_t index) {
  if (table == nullptr || mtl_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_table_set_texture: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (table->table == nil) {
      return;
    }
    [(id<MTL4ArgumentTable>)table->table setTexture:[(__bridge id<MTLTexture>)mtl_texture
                                                       gpuResourceID]
                                            atIndex:(NSUInteger)index];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_table_set_sampler(mr_mtl4_table *table, void *mtl_sampler, uint32_t index) {
  if (table == nullptr || mtl_sampler == nullptr) {
    mr_mtl4_fail("mr_mtl4_table_set_sampler: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (table->table == nil) {
      return;
    }
    [(id<MTL4ArgumentTable>)table->table
        setSamplerState:[(__bridge id<MTLSamplerState>)mtl_sampler gpuResourceID]
                atIndex:(NSUInteger)index];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* ------------------------------------------------------------------- frames */

MR_MTL4_AVAIL static mr_mtl4_frame *frame_create_impl(mr_mtl4_device *device, const char *label) {
  id<MTL4CommandAllocator> allocator = [(id<MTLDevice>)device->mtl newCommandAllocator];
  if (allocator == nil) {
    mr_mtl4_fail("[MTLDevice newCommandAllocator] returned nil");
    return nullptr;
  }
  id<MTL4CommandBuffer> buffer = [(id<MTLDevice>)device->mtl newCommandBuffer];
  if (buffer == nil) {
    mr_mtl4_fail("[MTLDevice newCommandBuffer] returned nil");
    return nullptr;
  }
  auto *frame = new mr_mtl4_frame{};
  frame->device = device;
  frame->allocator = allocator;
  frame->buffer = buffer;
  /* Completion is reported through a shared event the queue signals at commit,
   * which is Metal 4's replacement for addCompletedHandler. */
  frame->completion = [(id<MTLDevice>)device->mtl newSharedEvent];
  if (frame->completion == nil) {
    mr_mtl4_fail("[MTLDevice newSharedEvent] returned nil");
    return nullptr;
  }
  if (label != nullptr) {
    strncpy(frame->label, label, sizeof(frame->label) - 1);
  }
  return frame;
}

mr_mtl4_frame *mr_mtl4_device_new_frame(mr_mtl4_device *device, const char *label) {
  if (device == nullptr) {
    mr_mtl4_fail("mr_mtl4_device_new_frame: null device");
    return nullptr;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_create_impl(device, label);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_frame_destroy(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    return;
  }
  frame->render = nil;
  frame->compute = nil;
  frame->completion = nil;
  frame->buffer = nil;
  frame->allocator = nil;
  delete frame;
}

MR_MTL4_AVAIL static bool frame_begin_impl(mr_mtl4_frame *frame) {
  if (frame->in_flight) {
    /* Resetting an allocator whose work has not retired is an error in Metal 4,
     * not a race: the GPU would read command memory out from under itself. */
    mr_mtl4_fail("frame still in flight; wait for it or use another frame");
    return false;
  }
  if (frame->begun) {
    mr_mtl4_fail("mr_mtl4_frame_begin called on an open frame");
    return false;
  }
  [(id<MTL4CommandAllocator>)frame->allocator reset];
  [(id<MTL4CommandBuffer>)frame->buffer
      beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)frame->allocator];
  frame->begun = true;
  frame->ended = false;
  frame->committed = false;
  frame->encoder_open = false;
  return true;
}

bool mr_mtl4_frame_begin(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_begin: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_begin_impl(frame);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_is_in_flight(const mr_mtl4_frame *frame) {
  return frame != nullptr && frame->in_flight;
}

/* ----------------------------------------------------------------- encoders */

MR_MTL4_AVAIL static void frame_close_encoder_impl(mr_mtl4_frame *frame) {
  if (!frame->encoder_open) {
    return;
  }
  if (frame->render != nil) {
    [(id<MTL4RenderCommandEncoder>)frame->render endEncoding];
    frame->render = nil;
  } else if (frame->compute != nil) {
    [(id<MTL4ComputeCommandEncoder>)frame->compute endEncoding];
    frame->compute = nil;
  }
  frame->encoder_open = false;
}

MR_MTL4_AVAIL static bool frame_begin_render_impl(mr_mtl4_frame *frame, void *rp_desc,
                                                 MTL4RenderEncoderOptions options) {
  if (!frame->begun || frame->ended) {
    mr_mtl4_fail("render pass opened outside an open command buffer");
    return false;
  }
  if (frame->encoder_open) {
    mr_mtl4_fail("an encoder is already open; Metal 4 allows one at a time");
    return false;
  }
  MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
  if (desc == nil) {
    mr_mtl4_fail("render pass descriptor is nil");
    return false;
  }
  id<MTL4RenderCommandEncoder> encoder =
      options == 0 ? [(id<MTL4CommandBuffer>)frame->buffer renderCommandEncoderWithDescriptor:desc]
                    : [(id<MTL4CommandBuffer>)frame->buffer renderCommandEncoderWithDescriptor:desc
                                                                                     options:options];
  if (encoder == nil) {
    mr_mtl4_fail("[MTL4CommandBuffer renderCommandEncoderWithDescriptor:] returned nil");
    return false;
  }
  frame->render = encoder;
  frame->encoder_open = true;
  return true;
}

MR_MTL4_AVAIL static bool frame_begin_compute_impl(mr_mtl4_frame *frame) {
  if (!frame->begun || frame->ended) {
    mr_mtl4_fail("compute pass opened outside an open command buffer");
    return false;
  }
  if (frame->encoder_open) {
    mr_mtl4_fail("an encoder is already open; Metal 4 allows one at a time");
    return false;
  }
  id<MTL4ComputeCommandEncoder> encoder = [(id<MTL4CommandBuffer>)frame->buffer
      computeCommandEncoder];
  if (encoder == nil) {
    mr_mtl4_fail("[MTL4CommandBuffer computeCommandEncoder] returned nil");
    return false;
  }
  frame->compute = encoder;
  frame->encoder_open = true;
  return true;
}

bool mr_mtl4_frame_begin_render_pass(mr_mtl4_frame *frame, void *rp_desc) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_begin_render_pass: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_begin_render_impl(frame, rp_desc, (MTL4RenderEncoderOptions)0);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_begin_render_pass_suspended(mr_mtl4_frame *frame, void *rp_desc) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_begin_render_pass_suspended: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    /* Replaces MTLParallelRenderCommandEncoder: a suspended pass must be
     * resumed by the next command buffer in the same commit batch. */
    return frame_begin_render_impl(frame, rp_desc, MTL4RenderEncoderOptionSuspending);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_begin_render_pass_resumed(mr_mtl4_frame *frame, void *rp_desc) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_begin_render_pass_resumed: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_begin_render_impl(frame, rp_desc, MTL4RenderEncoderOptionResuming);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_begin_compute_pass(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_begin_compute_pass: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_begin_compute_impl(frame);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_end_encoder(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_end_encoder: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    frame_close_encoder_impl(frame);
    return true;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_encoder_is_open(const mr_mtl4_frame *frame) {
  return frame != nullptr && frame->encoder_open;
}

void *mr_mtl4_frame_metal_encoder(mr_mtl4_frame *frame) {
  if (frame == nullptr || frame->render == nil) {
    return nullptr;
  }
  return (__bridge void *)frame->render;
}

void mr_mtl4_frame_bind_table(mr_mtl4_frame *frame, mr_mtl4_table *table) {
  if (frame == nullptr || table == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_bind_table: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (!frame->encoder_open) {
      mr_mtl4_fail("mr_mtl4_frame_bind_table: no encoder open");
      return;
    }
    if (frame->render != nil) {
      [(id<MTL4RenderCommandEncoder>)frame->render
          setArgumentTable:(id<MTL4ArgumentTable>)table->table
                  atStages:(MTLRenderStages)table->stages];
    } else if (frame->compute != nil) {
      [(id<MTL4ComputeCommandEncoder>)frame->compute
          setArgumentTable:(id<MTL4ArgumentTable>)table->table];
    }
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* -------------------------------------------------------------- render state */

void mr_mtl4_frame_set_render_pipeline(mr_mtl4_frame *frame, void *mtl_pso) {
  if (frame == nullptr || mtl_pso == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_render_pipeline: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_render_pipeline: no render encoder open");
      return;
    }
    [(id<MTL4RenderCommandEncoder>)frame->render
        setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)mtl_pso];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_set_compute_pipeline(mr_mtl4_frame *frame, void *mtl_pso) {
  if (frame == nullptr || mtl_pso == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_compute_pipeline: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->compute == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_compute_pipeline: no compute encoder open");
      return;
    }
    [(id<MTL4ComputeCommandEncoder>)frame->compute
        setComputePipelineState:(__bridge id<MTLComputePipelineState>)mtl_pso];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_set_viewport(mr_mtl4_frame *frame, double x, double y, double width,
                                double height, double znear, double zfar) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_viewport: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_viewport: no render encoder open");
      return;
    }
    MTLViewport viewport = {x, y, width, height, znear, zfar};
    [(id<MTL4RenderCommandEncoder>)frame->render setViewports:&viewport count:1];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_set_scissor(mr_mtl4_frame *frame, uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_scissor: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_scissor: no render encoder open");
      return;
    }
    MTLScissorRect rect = {x, y, width, height};
    [(id<MTL4RenderCommandEncoder>)frame->render setScissorRects:&rect count:1];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_set_blend_color(mr_mtl4_frame *frame, float r, float g, float b, float a) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_blend_color: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_blend_color: no render encoder open");
      return;
    }
    [(id<MTL4RenderCommandEncoder>)frame->render setBlendColorRed:r green:g blue:b alpha:a];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_set_stencil_reference(mr_mtl4_frame *frame, uint32_t value) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_set_stencil_reference: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_set_stencil_reference: no render encoder open");
      return;
    }
    [(id<MTL4RenderCommandEncoder>)frame->render setStencilReferenceValue:value];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* ---------------------------------------------------------------- encoding */

void mr_mtl4_frame_draw(mr_mtl4_frame *frame, mr_mtl4_primitive_type type, uint32_t vertex_start,
                        uint32_t vertex_count, uint32_t instance_count, uint32_t base_instance) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_draw: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_draw: no render encoder open");
      return;
    }
    [(id<MTL4RenderCommandEncoder>)frame->render drawPrimitives:(MTLPrimitiveType)type
                                                    vertexStart:(NSUInteger)vertex_start
                                                    vertexCount:(NSUInteger)vertex_count
                                                  instanceCount:(NSUInteger)instance_count
                                                   baseInstance:(NSUInteger)base_instance];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_draw_indexed(mr_mtl4_frame *frame, mr_mtl4_primitive_type type,
                                uint32_t index_count, mr_mtl4_index_type index_type,
                                uint64_t index_address, uint64_t index_buffer_length,
                                uint32_t instance_count, int32_t base_vertex,
                                uint32_t base_instance) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_draw_indexed: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->render == nil) {
      mr_mtl4_fail("mr_mtl4_frame_draw_indexed: no render encoder open");
      return;
    }
    if (index_address == 0) {
      mr_mtl4_fail("mr_mtl4_frame_draw_indexed: null index buffer address");
      return;
    }
    [(id<MTL4RenderCommandEncoder>)frame->render
        drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)index_count
                    indexType:(MTLIndexType)index_type
                  indexBuffer:(MTLGPUAddress)index_address
            indexBufferLength:(NSUInteger)index_buffer_length
                instanceCount:(NSUInteger)instance_count
                   baseVertex:(NSInteger)base_vertex
                 baseInstance:(NSUInteger)base_instance];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_dispatch(mr_mtl4_frame *frame, uint32_t groups_x, uint32_t groups_y,
                            uint32_t groups_z, uint32_t threads_x, uint32_t threads_y,
                            uint32_t threads_z) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_dispatch: null frame");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->compute == nil) {
      mr_mtl4_fail("mr_mtl4_frame_dispatch: no compute encoder open");
      return;
    }
    [(id<MTL4ComputeCommandEncoder>)frame->compute
        dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, groups_z)
        threadsPerThreadgroup:MTLSizeMake(threads_x, threads_y, threads_z)];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_copy_buffer(mr_mtl4_frame *frame, void *src_buffer, uint64_t src_offset,
                               void *dst_buffer, uint64_t dst_offset, uint64_t size) {
  if (frame == nullptr || src_buffer == nullptr || dst_buffer == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_copy_buffer: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    /* Metal 4 has no blit encoder; copies are compute-encoder operations. */
    if (frame->compute == nil) {
      mr_mtl4_fail("mr_mtl4_frame_copy_buffer: no compute encoder open");
      return;
    }
    [(id<MTL4ComputeCommandEncoder>)frame->compute
        copyFromBuffer:(__bridge id<MTLBuffer>)src_buffer
          sourceOffset:(NSUInteger)src_offset
              toBuffer:(__bridge id<MTLBuffer>)dst_buffer
     destinationOffset:(NSUInteger)dst_offset
                  size:(NSUInteger)size];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_fill_buffer(mr_mtl4_frame *frame, void *buffer, uint64_t offset,
                               uint64_t length, uint8_t value) {
  if (frame == nullptr || buffer == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_fill_buffer: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->compute == nil) {
      mr_mtl4_fail("mr_mtl4_frame_fill_buffer: no compute encoder open");
      return;
    }
    [(id<MTL4ComputeCommandEncoder>)frame->compute
        fillBuffer:(__bridge id<MTLBuffer>)buffer
             range:NSMakeRange((NSUInteger)offset, (NSUInteger)length)
             value:value];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_generate_mipmaps(mr_mtl4_frame *frame, void *mtl_texture) {
  if (frame == nullptr || mtl_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_generate_mipmaps: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (frame->compute == nil) {
      mr_mtl4_fail("mr_mtl4_frame_generate_mipmaps: no compute encoder open");
      return;
    }
    [(id<MTL4ComputeCommandEncoder>)frame->compute
        generateMipmapsForTexture:(__bridge id<MTLTexture>)mtl_texture];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* ---------------------------------------------------------- submit / present */

MR_MTL4_AVAIL static bool frame_submit_impl(mr_mtl4_frame *frame, void *mtl_drawable) {
  if (frame->committed) {
    mr_mtl4_fail("frame already committed");
    return false;
  }
  if (!frame->begun) {
    mr_mtl4_fail("frame was not begun");
    return false;
  }
  frame_close_encoder_impl(frame);
  if (!frame->ended) {
    [(id<MTL4CommandBuffer>)frame->buffer endCommandBuffer];
    frame->ended = true;
  }

  id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)frame->device->queue;
  id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)mtl_drawable;

  /* Order matters and is fixed by Metal 4: the GPU waits for the drawable
   * before the batch, and the queue signals it after. A command buffer cannot
   * wait or signal a drawable itself. */
  if (drawable != nil) {
    [queue waitForDrawable:drawable];
  }
  id<MTL4CommandBuffer> buffers[1] = {(id<MTL4CommandBuffer>)frame->buffer};
  [queue commit:buffers count:1];
  if (frame->completion != nil) {
    frame->completion_value += 1;
    [queue signalEvent:(id<MTLSharedEvent>)frame->completion value:frame->completion_value];
  }
  if (drawable != nil) {
    [queue signalDrawable:drawable];
    [drawable present];
  }

  frame->committed = true;
  frame->begun = false;
  frame->in_flight = true;
  return true;
}

bool mr_mtl4_frame_commit(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_commit: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_submit_impl(frame, nullptr);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_present(mr_mtl4_frame *frame, void *mtl_drawable) {
  if (frame == nullptr || mtl_drawable == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_present: null argument");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return frame_submit_impl(frame, mtl_drawable);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

bool mr_mtl4_frame_wait(mr_mtl4_frame *frame, uint64_t timeout_ms) {
  if (frame == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_wait: null frame");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (!frame->in_flight) {
      return true;
    }
    id<MTLSharedEvent> event = (id<MTLSharedEvent>)frame->completion;
    if (event == nil) {
      mr_mtl4_fail("no completion event on this frame");
      return false;
    }
    const uint64_t timeout = timeout_ms == 0 ? UINT64_MAX : timeout_ms;
    if (![event waitUntilSignaledValue:frame->completion_value timeoutMS:timeout]) {
      mr_mtl4_fail("timed out waiting for the GPU");
      return false;
    }
    frame->in_flight = false;
    return true;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

void mr_mtl4_frame_wait_for_event(mr_mtl4_frame *frame, void *mtl_shared_event, uint64_t value) {
  if (frame == nullptr || mtl_shared_event == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_wait_for_event: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    [(id<MTL4CommandQueue>)frame->device->queue
        waitForEvent:(__bridge id<MTLSharedEvent>)mtl_shared_event
               value:value];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_frame_signal_event(mr_mtl4_frame *frame, void *mtl_shared_event, uint64_t value) {
  if (frame == nullptr || mtl_shared_event == nullptr) {
    mr_mtl4_fail("mr_mtl4_frame_signal_event: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    [(id<MTL4CommandQueue>)frame->device->queue
        signalEvent:(__bridge id<MTLSharedEvent>)mtl_shared_event
               value:value];
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* --------------------------------------------------- render pass descriptor */

MR_MTL4_AVAIL static void *render_pass_create_impl(void) {
  MTL4RenderPassDescriptor *desc = [[MTL4RenderPassDescriptor alloc] init];
  return (__bridge_retained void *)desc;
}

void *mr_mtl4_render_pass_create(void) {
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return render_pass_create_impl();
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_render_pass_destroy(void *rp_desc) {
  if (rp_desc == nullptr) {
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    MTL4RenderPassDescriptor *desc =
        (__bridge_transfer MTL4RenderPassDescriptor *)rp_desc; /* ARC releases it */
    (void)desc;
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_render_pass_set_size(void *rp_desc, uint32_t width, uint32_t height) {
  if (rp_desc == nullptr) {
    mr_mtl4_fail("mr_mtl4_render_pass_set_size: null descriptor");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    /* Only needed when no attachment supplies the size; harmless otherwise. */
    MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
    desc.renderTargetWidth = (NSUInteger)width;
    desc.renderTargetHeight = (NSUInteger)height;
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_render_pass_set_color(void *rp_desc, uint32_t index, void *mtl_texture,
                                   mr_mtl4_load_action load, mr_mtl4_store_action store,
                                   double clear_r, double clear_g, double clear_b,
                                   double clear_a) {
  if (rp_desc == nullptr || mtl_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_render_pass_set_color: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
    MTLRenderPassColorAttachmentDescriptor *attachment = desc.colorAttachments[index];
    if (attachment == nil) {
      mr_mtl4_fail("mr_mtl4_render_pass_set_color: attachment index out of range");
      return;
    }
    attachment.texture = (__bridge id<MTLTexture>)mtl_texture;
    attachment.loadAction = (MTLLoadAction)load;
    attachment.storeAction = (MTLStoreAction)store;
    attachment.clearColor = MTLClearColorMake(clear_r, clear_g, clear_b, clear_a);
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_render_pass_set_color_resolve(void *rp_desc, uint32_t index,
                                           void *mtl_resolve_texture) {
  if (rp_desc == nullptr || mtl_resolve_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_render_pass_set_color_resolve: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
    MTLRenderPassColorAttachmentDescriptor *attachment = desc.colorAttachments[index];
    if (attachment == nil) {
      mr_mtl4_fail("mr_mtl4_render_pass_set_color_resolve: attachment index out of range");
      return;
    }
    attachment.resolveTexture = (__bridge id<MTLTexture>)mtl_resolve_texture;
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_render_pass_set_depth(void *rp_desc, void *mtl_texture, mr_mtl4_load_action load,
                                   mr_mtl4_store_action store, double clear_depth) {
  if (rp_desc == nullptr || mtl_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_render_pass_set_depth: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
    MTLRenderPassDepthAttachmentDescriptor *attachment = desc.depthAttachment;
    attachment.texture = (__bridge id<MTLTexture>)mtl_texture;
    attachment.loadAction = (MTLLoadAction)load;
    attachment.storeAction = (MTLStoreAction)store;
    attachment.clearDepth = clear_depth;
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

void mr_mtl4_render_pass_set_stencil(void *rp_desc, void *mtl_texture, mr_mtl4_load_action load,
                                     mr_mtl4_store_action store, uint32_t clear_stencil) {
  if (rp_desc == nullptr || mtl_texture == nullptr) {
    mr_mtl4_fail("mr_mtl4_render_pass_set_stencil: null argument");
    return;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    MTL4RenderPassDescriptor *desc = (__bridge MTL4RenderPassDescriptor *)rp_desc;
    MTLRenderPassStencilAttachmentDescriptor *attachment = desc.stencilAttachment;
    attachment.texture = (__bridge id<MTLTexture>)mtl_texture;
    attachment.loadAction = (MTLLoadAction)load;
    attachment.storeAction = (MTLStoreAction)store;
    attachment.clearStencil = clear_stencil;
    return;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
}

/* ------------------------------------------------------ transient allocator */

MR_MTL4_AVAIL static mr_mtl4_transient *transient_create_impl(void *mtl_device,
                                                             uint64_t capacity) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
  if (device == nil) {
    mr_mtl4_fail("mr_mtl4_transient_create: no Metal device");
    return nullptr;
  }
  if (capacity == 0) {
    mr_mtl4_fail("mr_mtl4_transient_create: zero capacity");
    return nullptr;
  }
  id<MTLBuffer> buffer = [device newBufferWithLength:(NSUInteger)capacity
                                             options:MTLResourceStorageModeShared];
  if (buffer == nil) {
    mr_mtl4_fail("[MTLDevice newBufferWithLength:options:] returned nil");
    return nullptr;
  }
  auto *transient = new mr_mtl4_transient{};
  transient->buffer = buffer;
  transient->capacity = capacity;
  transient->offset = 0;
  return transient;
}

mr_mtl4_transient *mr_mtl4_transient_create(void *mtl_device, uint64_t capacity) {
  if (mtl_device == nullptr) {
    mr_mtl4_fail("mr_mtl4_transient_create: null device");
    return nullptr;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return transient_create_impl(mtl_device, capacity);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_transient_destroy(mr_mtl4_transient *transient) {
  if (transient == nullptr) {
    return;
  }
  transient->buffer = nil;
  delete transient;
}

uint64_t mr_mtl4_transient_write(mr_mtl4_transient *transient, const void *data, uint64_t size) {
  if (transient == nullptr) {
    mr_mtl4_fail("mr_mtl4_transient_write: null allocator");
    return 0;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (data == nullptr || size == 0) {
      mr_mtl4_fail("mr_mtl4_transient_write: nothing to write");
      return 0;
    }
    /* 256 is the D3D11 constant-buffer alignment, the strictest consumer here;
     * float4 constant data only needs 16. */
    const uint64_t alignment = 256;
    uint64_t offset = (transient->offset + alignment - 1) & ~(alignment - 1);
    if (offset + size > transient->capacity) {
      mr_mtl4_fail("transient buffer exhausted; raise the capacity for this frame");
      return 0;
    }
    id<MTLBuffer> buffer = (id<MTLBuffer>)transient->buffer;
    uint8_t *base = (uint8_t *)[buffer contents];
    if (base == nullptr) {
      mr_mtl4_fail("transient buffer has no CPU-visible contents");
      return 0;
    }
    memcpy(base + offset, data, (size_t)size);
    transient->offset = offset + size;
    return (uint64_t)[buffer gpuAddress] + offset;
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return 0;
}

void mr_mtl4_transient_reset(mr_mtl4_transient *transient) {
  if (transient != nullptr) {
    transient->offset = 0;
  }
}

uint64_t mr_mtl4_transient_used(const mr_mtl4_transient *transient) {
  return transient != nullptr ? transient->offset : 0;
}

uint64_t mr_mtl4_transient_capacity(const mr_mtl4_transient *transient) {
  return transient != nullptr ? transient->capacity : 0;
}

void *mr_mtl4_transient_buffer(mr_mtl4_transient *transient) {
  if (transient == nullptr) {
    return nullptr;
  }
  return (__bridge void *)transient->buffer;
}

/* ----------------------------------------------------------------- residency */

struct mr_mtl4_residency {
  id set; /* id<MTLResidencySet> */
  uint32_t added;
  uint32_t capacity;
  uint32_t rejected;
  bool dirty;
};

MR_MTL4_AVAIL static mr_mtl4_residency *residency_create_impl(mr_mtl4_device *device,
                                                             uint32_t capacity) {
  if (capacity == 0) {
    mr_mtl4_fail("mr_mtl4_residency_create: zero capacity");
    return nullptr;
  }
  MTLResidencySetDescriptor *desc = [[MTLResidencySetDescriptor alloc] init];
  desc.initialCapacity = (NSInteger)capacity;
  NSError *error = nil;
  id<MTLResidencySet> set = [(id<MTLDevice>)device->mtl newResidencySetWithDescriptor:desc
                                                                                error:&error];
  if (set == nil) {
    mr_mtl4_fail(error != nil ? [[error localizedDescription] UTF8String]
                              : "[MTLDevice newResidencySetWithDescriptor:error:] returned nil");
    return nullptr;
  }
  auto *residency = new mr_mtl4_residency{};
  residency->set = set;
  residency->capacity = capacity;
  return residency;
}

mr_mtl4_residency *mr_mtl4_residency_create(mr_mtl4_device *device, uint32_t capacity) {
  if (device == nullptr) {
    mr_mtl4_fail("mr_mtl4_residency_create: null device");
    return nullptr;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return residency_create_impl(device, capacity);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return nullptr;
}

void mr_mtl4_residency_destroy(mr_mtl4_residency *residency) {
  if (residency == nullptr) {
    return;
  }
  residency->set = nil;
  delete residency;
}

MR_MTL4_AVAIL static bool residency_add_impl(mr_mtl4_residency *residency, void *allocation) {
  if (allocation == nullptr) {
    return false;
  }
  if (residency->added >= residency->capacity) {
    /* A resource that does not fit is a hard failure rather than something to
     * ignore: the draw that reads it is invalid, and Metal would report that
     * against the draw rather than against the binding that overflowed. */
    residency->rejected += 1;
    mr_mtl4_fail("mr_mtl4_residency_add: set is full; raise the capacity for this frame");
    return false;
  }
  /* Adding the same allocation twice is not an error, so a caller does not have
   * to deduplicate resources the binding path meets many times per frame. */
  [(id<MTLResidencySet>)residency->set addAllocation:(__bridge id<MTLAllocation>)allocation];
  residency->added += 1;
  residency->dirty = true;
  return true;
}

bool mr_mtl4_residency_add(mr_mtl4_residency *residency, void *mtl_allocation) {
  if (residency == nullptr || mtl_allocation == nullptr) {
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return residency_add_impl(residency, mtl_allocation);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

MR_MTL4_AVAIL static bool residency_commit_impl(mr_mtl4_residency *residency) {
  [(id<MTLResidencySet>)residency->set commit];
  residency->dirty = false;
  return true;
}

bool mr_mtl4_residency_commit(mr_mtl4_residency *residency) {
  if (residency == nullptr) {
    mr_mtl4_fail("mr_mtl4_residency_commit: null residency set");
    return false;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    return residency_commit_impl(residency);
  }
  mr_mtl4_fail("Metal 4 requires iOS 26 or macOS 26");
  return false;
}

void *mr_mtl4_residency_metal_set(mr_mtl4_residency *residency) {
  if (residency == nullptr) {
    return nullptr;
  }
  return (__bridge void *)residency->set;
}

bool mr_mtl4_residency_dirty(const mr_mtl4_residency *residency) {
  return residency != nullptr && residency->dirty;
}

uint32_t mr_mtl4_residency_added(const mr_mtl4_residency *residency) {
  return residency != nullptr ? residency->added : 0;
}

uint32_t mr_mtl4_residency_rejected(const mr_mtl4_residency *residency) {
  return residency != nullptr ? residency->rejected : 0;
}
