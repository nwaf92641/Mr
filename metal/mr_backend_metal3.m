/*
 * The Metal 3 backend: the ABI in mr_backend.h implemented on MTLDevice.
 *
 * This is the compatibility path, and the name is not a claim of inferiority. It
 * implements the Metal 4 shaped ABI -- allocators, argument tables, residency --
 * by emulation, exactly as the header's mapping table describes, so a caller
 * written for Metal 4 is correct here too. What it does not do is offer what
 * Metal 3 does not have: there are no explicit barriers, so hazard tracking stays
 * implicit; there are no native argument tables, so bindings are applied through
 * the encoder's own calls; there is no allocator object, so allocators are a
 * count of outstanding command buffers.
 *
 * Scope, stated plainly. The frame path is real: device, queue, allocators,
 * command buffers, render encoders, argument tables, textures, buffers, shader
 * libraries, render pipelines, draws, back-buffer reads and drawable present all
 * do the thing they say. Everything else returns MR_ERR_UNSUPPORTED with a
 * reason that names what is missing, and mr_metal_last_reason() hands that reason
 * to a test or a log. There is no path here that returns MR_OK without having
 * done the work, because a backend that lies about one call makes every later
 * failure read as a bug somewhere else.
 *
 * Why it is this small: this file has to be provable before DXMT or Wine exist in
 * the picture. A frame that is created, drawn, read back and checked is proof
 * that the device, the queue, the command model, the pipeline and the pixel
 * formats all work, and it can be produced with no guest code at all.
 *
 * Manual reference counting: this is an interop layer between a C ABI and the
 * Objective-C runtime, and the objects are owned by a C table whose lifetime the
 * compiler cannot see. ARC would be guessing about that table, so retain and
 * release are written out.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include <stdlib.h>
#include <string.h>

#include "mr_metal_backend.h"
#include "mr/mr_backend.h"

/* ------------------------------------------------------------- object table */

/*
 * Handles are indices with a generation tag, not pointers, because they cross
 * into the PE side of DXMT where a host address is meaningless, and because a
 * stale handle has to be detected rather than followed. Reusing a slot bumps the
 * generation, so a handle that outlives its object resolves to nothing instead of
 * to whatever was allocated next.
 */
#define MR_HANDLE_INDEX_BITS 24u
#define MR_HANDLE_INDEX_MASK ((1ull << MR_HANDLE_INDEX_BITS) - 1ull)

typedef enum {
  MR_OBJ_FREE = 0,
  MR_OBJ_ALLOCATOR,
  MR_OBJ_CMDBUF,
  MR_OBJ_ENCODER,
  MR_OBJ_TEXTURE,
  MR_OBJ_BUFFER,
  MR_OBJ_LIBRARY,
  MR_OBJ_PIPELINE,
  MR_OBJ_ARG_TABLE,
  MR_OBJ_RESIDENCY,
  MR_OBJ_HEAP,
} mr_obj_kind;

typedef struct {
  mr_obj_kind kind;
  uint32_t generation;
  id obj;   /* the Metal object, retained; nil until a command buffer is begun */
  id aux;   /* second object where one call needs two (blit source) */
  void *data; /* malloc'd side struct for kinds that need one */
  uint64_t value;
} mr_obj_slot;

/* An allocator in Metal 3 owns no memory: it counts the command buffers it has
 * handed out, which is what makes resetting it while they are in flight the same
 * error it is in Metal 4. */
typedef struct {
  mr_gfx_handle handed[16];
  uint32_t handed_count;
  uint32_t capacity;
} mr_allocator;

typedef struct {
  uint32_t max_buffers;
  uint32_t max_textures;
  uint32_t max_samplers;
  mr_gfx_handle *buffers;
  uint64_t *offsets;
  mr_gfx_handle *textures;
  mr_gfx_handle *samplers;
} mr_arg_table;

typedef struct {
  mr_gfx_handle *items;
  uint32_t count;
  uint32_t capacity;
} mr_residency;

/*
 * The public vtable (mr_backend.h) is the first member, so the pointer the ABI
 * hands around and the pointer to this structure are the same address. The
 * cast in mr_ctx() depends on that and nothing else.
 */
typedef struct {
  mr_gfx_backend pub;

  id<MTLDevice> device;
  id<MTLCommandQueue> queue;

  mr_obj_slot *slots;
  uint32_t slot_count;

  /* The encoder a bind or barrier is currently aimed at. Metal 3 needs this
   * because residency has to be pushed onto the encoder that will use the
   * resource, and the ABI's call carries the encoder handle already. It is kept
   * for the encoder-lifetime checks in draw() and end. */
  mr_gfx_handle current_encoder;

  /* Borrowed from the process, retained here: the layer present() draws into. */
  id<CAMetalLayer> layer;

  char reason[256];
  char commit_status[256];
} mr_metal_ctx;

/* ---------------------------------------------------------------- helpers */

static mr_metal_ctx *mr_ctx(mr_gfx_backend *self) {
  return self != NULL ? (mr_metal_ctx *)self : NULL;
}

static void mr_reason(mr_metal_ctx *c, const char *text) {
  if (c == NULL) return;
  if (text == NULL) text = "";
  size_t n = strlen(text);
  if (n >= sizeof(c->reason)) n = sizeof(c->reason) - 1;
  memcpy(c->reason, text, n);
  c->reason[n] = '\0';
}

static mr_status mr_unsupported(mr_metal_ctx *c, const char *why) {
  mr_reason(c, why);
  return MR_ERR_UNSUPPORTED;
}

static mr_status mr_state(mr_metal_ctx *c, const char *why) {
  mr_reason(c, why);
  return MR_ERR_STATE;
}

static mr_obj_slot *mr_slot(mr_metal_ctx *c, mr_gfx_handle h) {
  if (c == NULL || h == MR_GFX_HANDLE_NONE) return NULL;
  uint64_t index = (h & MR_HANDLE_INDEX_MASK);
  uint64_t generation = h >> MR_HANDLE_INDEX_BITS;
  if (index == 0 || index > c->slot_count) return NULL;
  mr_obj_slot *s = &c->slots[index - 1];
  if (s->kind == MR_OBJ_FREE || (uint64_t)s->generation != generation) {
    return NULL;
  }
  return s;
}

static mr_obj_slot *mr_slot_as(mr_metal_ctx *c, mr_gfx_handle h,
                               mr_obj_kind kind) {
  mr_obj_slot *s = mr_slot(c, h);
  if (s == NULL || s->kind != kind) return NULL;
  return s;
}

static mr_gfx_handle mr_intern(mr_metal_ctx *c, mr_obj_kind kind, id obj) {
  if (c == NULL) return MR_GFX_HANDLE_NONE;
  for (uint32_t i = 0; i < c->slot_count; i++) {
    mr_obj_slot *s = &c->slots[i];
    if (s->kind != MR_OBJ_FREE) continue;
    s->kind = kind;
    s->generation++;
    if (s->generation == 0) s->generation = 1;
    s->obj = obj; /* ownership passes to the table */
    s->aux = nil;
    s->data = NULL;
    s->value = 0;
    return ((uint64_t)s->generation << MR_HANDLE_INDEX_BITS) | (uint64_t)(i + 1);
  }
  /* Out of slots. Reported rather than grown, because a growing table would
   * hide a leak: handles are only freed through resource_destroy and the
   * destroy entries, so hitting this ceiling means one of those stopped being
   * called. */
  mr_reason(c, "the backend's handle table is full (4096 objects); a destroy "
               "call is missing");
  return MR_GFX_HANDLE_NONE;
}

static void mr_release_obj(id *obj) {
  if (obj != NULL && *obj != nil) {
    [*obj release];
    *obj = nil;
  }
}

static void mr_slot_free(mr_metal_ctx *c, mr_obj_slot *s) {
  (void)c;
  if (s == NULL) return;
  mr_release_obj(&s->obj);
  mr_release_obj(&s->aux);
  free(s->data);
  s->data = NULL;
  s->kind = MR_OBJ_FREE;
  s->value = 0;
}

static const char *mr_status_word(MTLCommandBufferStatus st) {
  switch (st) {
    case MTLCommandBufferStatusNotEnqueued: return "not enqueued";
    case MTLCommandBufferStatusEnqueued: return "enqueued";
    case MTLCommandBufferStatusCommitted: return "committed";
    case MTLCommandBufferStatusScheduled: return "scheduled";
    case MTLCommandBufferStatusCompleted: return "completed";
    case MTLCommandBufferStatusError: return "error";
  }
  return "unknown";
}

/* ---------------------------------------------------------------- formats */

static bool mr_to_pixel_format(mr_format f, MTLPixelFormat *out) {
  switch (f) {
    case MR_FMT_R8_UNORM: *out = MTLPixelFormatR8Unorm; return true;
    case MR_FMT_R8G8_UNORM: *out = MTLPixelFormatRG8Unorm; return true;
    case MR_FMT_R8G8B8A8_UNORM:
      *out = MTLPixelFormatRGBA8Unorm;
      return true;
    case MR_FMT_R8G8B8A8_UNORM_SRGB:
      *out = MTLPixelFormatRGBA8Unorm_sRGB;
      return true;
    case MR_FMT_B8G8R8A8_UNORM:
      *out = MTLPixelFormatBGRA8Unorm;
      return true;
    case MR_FMT_B8G8R8A8_UNORM_SRGB:
      *out = MTLPixelFormatBGRA8Unorm_sRGB;
      return true;
    case MR_FMT_R16_FLOAT: *out = MTLPixelFormatR16Float; return true;
    case MR_FMT_R16G16_FLOAT: *out = MTLPixelFormatRG16Float; return true;
    case MR_FMT_R16G16B16A16_FLOAT:
      *out = MTLPixelFormatRGBA16Float;
      return true;
    case MR_FMT_R32_FLOAT: *out = MTLPixelFormatR32Float; return true;
    case MR_FMT_R32G32_FLOAT: *out = MTLPixelFormatRG32Float; return true;
    case MR_FMT_R32G32B32A32_FLOAT:
      *out = MTLPixelFormatRGBA32Float;
      return true;
    case MR_FMT_R32_UINT: *out = MTLPixelFormatR32Uint; return true;
    case MR_FMT_R32_SINT: *out = MTLPixelFormatR32Sint; return true;
    case MR_FMT_D16_UNORM: *out = MTLPixelFormatDepth16Unorm; return true;
    case MR_FMT_D32_FLOAT: *out = MTLPixelFormatDepth32Float; return true;
    case MR_FMT_D24_UNORM_S8_UINT:
      *out = MTLPixelFormatDepth24Unorm_Stencil8;
      return true;
    case MR_FMT_BC1_RGBA_UNORM: *out = MTLPixelFormatBC1_RGBA; return true;
    case MR_FMT_BC2_RGBA_UNORM: *out = MTLPixelFormatBC2_RGBA; return true;
    case MR_FMT_BC3_RGBA_UNORM: *out = MTLPixelFormatBC3_RGBA; return true;
    case MR_FMT_BC4_R_UNORM: *out = MTLPixelFormatBC4_RUnorm; return true;
    case MR_FMT_BC5_RG_UNORM: *out = MTLPixelFormatBC5_RGUnorm; return true;
    case MR_FMT_BC6H_RGB_UFLOAT: *out = MTLPixelFormatBC6H_RGBUfloat; return true;
    case MR_FMT_BC7_RGBA_UNORM: *out = MTLPixelFormatBC7_RGBAUnorm; return true;
    case MR_FMT_UNKNOWN:
    case MR_FMT_COUNT:
      break;
  }
  return false;
}

static MTLTextureUsage mr_to_texture_usage(mr_usage u) {
  MTLTextureUsage out = MTLTextureUsageUnknown;
  if (u & MR_USAGE_SHADER_READ) out |= MTLTextureUsageShaderRead;
  if (u & MR_USAGE_SHADER_WRITE) out |= MTLTextureUsageShaderWrite;
  if (u & MR_USAGE_RENDER_TARGET) out |= MTLTextureUsageRenderTarget;
  if (u & (MR_USAGE_TRANSFER_SRC | MR_USAGE_TRANSFER_DST)) {
    out |= MTLTextureUsagePixelFormatView;
  }
  return out;
}

/* ------------------------------------------------------------- device caps */

static mr_status mr_metal_caps(mr_gfx_backend *self, mr_gfx_caps *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || out == NULL) return MR_ERR_INVALID;
  memset(out, 0, sizeof(*out));

  out->backend = MR_BACKEND_METAL3;
  memcpy(out->name, "metal3", sizeof("metal3"));
  out->gpu_family = 0;
  out->device_name[0] = '\0';

  const char *name = c->device.name.UTF8String;
  if (name != NULL) {
    size_t n = strlen(name);
    if (n >= sizeof(out->device_name)) n = sizeof(out->device_name) - 1;
    memcpy(out->device_name, name, n);
    out->device_name[n] = '\0';
  }

  /* What Metal 3 genuinely has. These are the flags a caller uses to decide
   * whether to take a fast path, so a wrong true here is a crash later. */
  out->unified_memory = c->device.hasUnifiedMemory != NO;
  out->explicit_residency = false;  /* useResource: instead */
  out->explicit_barriers = false;   /* implicit hazard tracking */
  out->argument_tables = false;     /* encoder binding calls instead */
  out->async_pipeline_compilation = true; /* completion-handler compilation */
  out->pipeline_archives = true;    /* MTLBinaryArchive exists in Metal 3 */
  out->flexible_pipelines = false;  /* Metal 4 only */
  out->mesh_shaders = false;        /* not offered on this path */
  out->bc_textures = c->device.supportsBCTextureCompression != NO;

  out->max_buffers_per_stage = 31; /* the Metal 3 argument-table tier-2 limit */
  out->max_textures_per_stage = 128;
  out->max_argument_table_entries = 0;
  out->max_buffer_length = (uint64_t)c->device.maxBufferLength;
  out->max_threadgroup_memory = (uint64_t)c->device.maxThreadgroupMemoryLength;
  out->max_threads_per_threadgroup =
      (size_t)c->device.maxThreadsPerThreadgroup.width;
  out->max_residency_sets_per_queue = 0;
  out->max_frames_in_flight = 3;
  return MR_OK;
}

static void mr_metal_destroy_backend(mr_gfx_backend *self) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return;
  @autoreleasepool {
    for (uint32_t i = 0; i < c->slot_count; i++) {
      mr_slot_free(c, &c->slots[i]);
    }
    free(c->slots);
    c->slots = NULL;
    /* Released directly rather than through mr_release_obj: that helper takes
     * id*, and id<MTLDevice> is a different pointer type to the compiler. */
    if (c->layer != nil) { [c->layer release]; c->layer = nil; }
    if (c->queue != nil) { [c->queue release]; c->queue = nil; }
    if (c->device != nil) { [c->device release]; c->device = nil; }
    free(c);
  }
}

/* ------------------------------------------------------------- allocators */

static mr_status mr_allocator_create(mr_gfx_backend *self, const char *label,
                                     mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || out == NULL) return MR_ERR_INVALID;
  (void)label;

  mr_allocator *a = (mr_allocator *)calloc(1, sizeof(*a));
  if (a == NULL) return MR_ERR_NOMEM;
  a->capacity = 16;

  mr_gfx_handle h = mr_intern(c, MR_OBJ_ALLOCATOR, nil);
  if (h == MR_GFX_HANDLE_NONE) {
    free(a);
    return MR_ERR_NOMEM;
  }
  mr_slot(c, h)->data = a;
  *out = h;
  return MR_OK;
}

static mr_status mr_allocator_reset(mr_gfx_backend *self, mr_gfx_handle alloc) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, alloc, MR_OBJ_ALLOCATOR);
  if (s == NULL) return MR_ERR_INVALID;
  mr_allocator *a = (mr_allocator *)s->data;
  if (a == NULL) return MR_ERR_INVALID;

  /*
   * The Metal 4 rule, enforced here because a caller written against this ABI
   * will rely on it: an allocator cannot be reset while the commands it produced
   * are still in flight. In Metal 3 the consequence of getting this wrong is
   * subtler than in Metal 4 -- the command buffers still exist, so nothing
   * crashes -- but the caller would then be ring-reusing a slot the GPU is
   * reading, and the frame would be wrong instead of late.
   */
  for (uint32_t i = 0; i < a->handed_count; i++) {
    mr_obj_slot *cb = mr_slot_as(c, a->handed[i], MR_OBJ_CMDBUF);
    if (cb == NULL) continue;
    id<MTLCommandBuffer> buffer = (id<MTLCommandBuffer>)cb->obj;
    if (buffer == nil) continue;
    MTLCommandBufferStatus st = buffer.status;
    if (st != MTLCommandBufferStatusCompleted &&
        st != MTLCommandBufferStatusError) {
      return mr_state(c, "allocator_reset called while a command buffer it "
                         "handed out is still in flight");
    }
  }
  a->handed_count = 0;
  return MR_OK;
}

static void mr_allocator_destroy(mr_gfx_backend *self, mr_gfx_handle alloc) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, alloc, MR_OBJ_ALLOCATOR);
  if (s == NULL) return;
  mr_slot_free(c, s);
}

/* ---------------------------------------------------------- command buffers */

static mr_status mr_command_buffer_create(mr_gfx_backend *self,
                                          const char *label,
                                          mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || out == NULL) return MR_ERR_INVALID;
  (void)label;
  /* Only a handle is made here. The MTLCommandBuffer itself is created by
   * begin_command_buffer, because that is where the ABI says the allocator
   * supplies the command memory. */
  mr_gfx_handle h = mr_intern(c, MR_OBJ_CMDBUF, nil);
  if (h == MR_GFX_HANDLE_NONE) return MR_ERR_NOMEM;
  *out = h;
  return MR_OK;
}

static void mr_command_buffer_destroy(mr_gfx_backend *self, mr_gfx_handle cmd) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, cmd, MR_OBJ_CMDBUF);
  if (s == NULL) return;
  mr_slot_free(c, s);
}

static mr_status mr_begin_command_buffer(mr_gfx_backend *self, mr_gfx_handle cmd,
                                         mr_gfx_handle allocator) {
  mr_metal_ctx *c = mr_ctx(self);
  @autoreleasepool {
    mr_obj_slot *s = mr_slot_as(c, cmd, MR_OBJ_CMDBUF);
    mr_obj_slot *as = mr_slot_as(c, allocator, MR_OBJ_ALLOCATOR);
    if (s == NULL || as == NULL) return MR_ERR_INVALID;
    if (s->obj != nil) {
      return mr_state(c, "begin_command_buffer called on a command buffer that "
                         "has already been begun");
    }
    mr_allocator *a = (mr_allocator *)as->data;
    if (a == NULL) return MR_ERR_INVALID;
    if (a->handed_count >= a->capacity || a->handed_count >= 16) {
      return mr_state(c, "the allocator is at its frames-in-flight limit; wait "
                         "for the GPU or reset it before encoding another frame");
    }

    id<MTLCommandBuffer> buffer = [c->queue commandBuffer];
    if (buffer == nil) return mr_state(c, "the command queue refused to make a "
                                           "command buffer");
    [buffer retain];
    s->obj = buffer;
    s->value = allocator;
    a->handed[a->handed_count++] = cmd;
    return MR_OK;
  }
}

static mr_status mr_end_command_buffer(mr_gfx_backend *self, mr_gfx_handle cmd) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, cmd, MR_OBJ_CMDBUF);
  if (s == NULL) return MR_ERR_INVALID;
  if (s->obj == nil) {
    return mr_state(c, "end_command_buffer called before begin_command_buffer");
  }
  /* An encoder that was never ended would make the commit fail deep inside
   * Metal with a message about metal_validate_end_encoding. Saying so here is
   * the difference between a usable error and a puzzle. */
  if (c->current_encoder != MR_GFX_HANDLE_NONE) {
    mr_obj_slot *enc = mr_slot(c, c->current_encoder);
    if (enc != NULL && enc->value == cmd) {
      return mr_state(c, "end_command_buffer called with an encoder still open "
                         "on this command buffer");
    }
  }
  return MR_OK;
}

static mr_status mr_commit(mr_gfx_backend *self, const mr_gfx_handle *cmds,
                           size_t count) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return MR_ERR_INVALID;
  if (count != 0 && cmds == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    for (size_t i = 0; i < count; i++) {
      mr_obj_slot *s = mr_slot_as(c, cmds[i], MR_OBJ_CMDBUF);
      if (s == NULL) return MR_ERR_INVALID;
      id<MTLCommandBuffer> buffer = (id<MTLCommandBuffer>)s->obj;
      if (buffer == nil) {
        return mr_state(c, "commit called on a command buffer that was never "
                           "begun");
      }
      [buffer commit];
    }
  }
  return MR_OK;
}

static mr_status mr_metal_wait_idle(mr_gfx_backend *self) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return MR_ERR_INVALID;

  mr_status result = MR_OK;
  @autoreleasepool {
    for (uint32_t i = 0; i < c->slot_count; i++) {
      mr_obj_slot *s = &c->slots[i];
      if (s->kind != MR_OBJ_CMDBUF || s->obj == nil) continue;
      id<MTLCommandBuffer> buffer = (id<MTLCommandBuffer>)s->obj;
      MTLCommandBufferStatus st = buffer.status;
      if (st == MTLCommandBufferStatusCommitted ||
          st == MTLCommandBufferStatusScheduled ||
          st == MTLCommandBufferStatusEnqueued) {
        [buffer waitUntilCompleted];
        st = buffer.status;
      }
      NSError *error = buffer.error;
      const char *err = error != nil ? error.localizedDescription.UTF8String : "";
      snprintf(c->commit_status, sizeof(c->commit_status), "%s%s%s",
               mr_status_word(st), err != NULL && err[0] != '\0' ? ": " : "",
               err != NULL ? err : "");
      if (st == MTLCommandBufferStatusError) result = MR_ERR_STATE;
    }
  }
  return result;
}

/* ---------------------------------------------------------------- resources */

static mr_status mr_texture_create(mr_gfx_backend *self,
                                   const mr_texture_desc *desc,
                                   mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || desc == NULL || out == NULL) return MR_ERR_INVALID;
  if (desc->width == 0 || desc->height == 0) {
    return MR_ERR_INVALID;
  }

  @autoreleasepool {
    MTLPixelFormat pf = MTLPixelFormatInvalid;
    if (!mr_to_pixel_format(desc->format, &pf)) {
      return mr_unsupported(c, "texture format is not mapped to a Metal pixel "
                               "format by this backend");
    }
    if (mr_format_is_block_compressed(desc->format) &&
        c->device.supportsBCTextureCompression == NO) {
      /* Metal's own error for this is a generic validation failure. Naming the
       * device limitation is more use than relaying it. */
      return mr_unsupported(c, "this device does not support BC-compressed "
                               "textures");
    }

    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    d.pixelFormat = pf;
    d.width = (NSUInteger)desc->width;
    d.height = (NSUInteger)desc->height;
    d.depth = desc->depth != 0 ? (NSUInteger)desc->depth : 1;
    d.mipmapLevelCount = desc->mip_levels != 0 ? (NSUInteger)desc->mip_levels : 1;
    d.arrayLength = desc->array_length != 0 ? (NSUInteger)desc->array_length : 1;
    d.sampleCount = desc->sample_count != 0 ? (NSUInteger)desc->sample_count : 1;
    d.usage = mr_to_texture_usage(desc->usage);

    if (d.depth > 1) {
      d.textureType = MTLTextureType3D;
    } else if (d.arrayLength > 1) {
      d.textureType = d.sampleCount > 1 ? MTLTextureType2DMultisampleArray
                                        : MTLTextureType2DArray;
    } else {
      d.textureType = d.sampleCount > 1 ? MTLTextureType2DMultisample
                                        : MTLTextureType2D;
    }

    if ((desc->usage & MR_USAGE_TILE_MEMORY_ONLY) != 0) {
#if TARGET_OS_OSX
      [d release];
      return mr_unsupported(c, "memoryless (tile-only) textures exist on iOS, "
                               "not on macOS");
#else
      d.storageMode = MTLStorageModeMemoryless;
#endif
    } else {
      d.storageMode = desc->shared ? MTLStorageModeShared : MTLStorageModePrivate;
    }

    if (desc->label != NULL) {
      d.label = [NSString stringWithUTF8String:desc->label];
    }

    id<MTLTexture> texture = [c->device newTextureWithDescriptor:d];
    [d release];
    if (texture == nil) {
      return mr_state(c, "Metal refused to allocate the texture (see the device "
                         "log; usually a size or sample-count limit)");
    }

    mr_gfx_handle h = mr_intern(c, MR_OBJ_TEXTURE, texture);
    if (h == MR_GFX_HANDLE_NONE) {
      [texture release];
      return MR_ERR_NOMEM;
    }
    *out = h;
    return MR_OK;
  }
}

static mr_status mr_buffer_create(mr_gfx_backend *self,
                                  const mr_buffer_desc *desc,
                                  mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || desc == NULL || out == NULL) return MR_ERR_INVALID;
  if (desc->length == 0) return MR_ERR_INVALID;

  @autoreleasepool {
    MTLResourceOptions opts = desc->shared ? MTLResourceOptionsStorageModeShared
                                           : MTLResourceOptionsStorageModePrivate;
    if (desc->length > (uint64_t)c->device.maxBufferLength) {
      return mr_state(c, "buffer exceeds the device's maximum buffer length");
    }
    id<MTLBuffer> buffer = [c->device newBufferWithLength:(NSUInteger)desc->length
                                                  options:opts];
    if (buffer == nil) {
      return mr_state(c, "Metal refused to allocate the buffer");
    }
    if (desc->label != NULL) {
      buffer.label = [NSString stringWithUTF8String:desc->label];
    }

    mr_gfx_handle h = mr_intern(c, MR_OBJ_BUFFER, buffer);
    if (h == MR_GFX_HANDLE_NONE) {
      [buffer release];
      return MR_ERR_NOMEM;
    }
    *out = h;
    return MR_OK;
  }
}

static void mr_resource_destroy(mr_gfx_backend *self, mr_gfx_handle resource) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return;
  mr_obj_slot *s = mr_slot(c, resource);
  if (s == NULL) return;
  /* Encoders and the table itself are not resources; freeing them through this
   * call would leave the caller holding a handle it still expects to draw
   * with. */
  switch (s->kind) {
    case MR_OBJ_TEXTURE:
    case MR_OBJ_BUFFER:
    case MR_OBJ_HEAP:
      mr_slot_free(c, s);
      break;
    default:
      break;
  }
}

static void *mr_buffer_contents(mr_gfx_backend *self, mr_gfx_handle buffer) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, buffer, MR_OBJ_BUFFER);
  if (s == NULL) return NULL;
  id<MTLBuffer> b = (id<MTLBuffer>)s->obj;
  if (b == nil) return NULL;
  if (b.storageMode != MTLStorageModeShared) {
    /* A private buffer has no CPU address at all; returning nil is the honest
     * answer. Metal returns NULL for the call, so the check is only here to
     * record why in the reason string. */
    mr_reason(c, "buffer_contents called on a private (GPU-only) buffer");
    return NULL;
  }
  return b.contents;
}

static mr_status mr_copy_into_texture(mr_gfx_backend *self, mr_gfx_handle cmd,
                                      const void *bytes, size_t len,
                                      uint32_t bytes_per_row,
                                      uint32_t bytes_per_image,
                                      mr_gfx_handle texture) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || texture == MR_GFX_HANDLE_NONE) return MR_ERR_INVALID;
  (void)cmd;

  @autoreleasepool {
    mr_obj_slot *s = mr_slot_as(c, texture, MR_OBJ_TEXTURE);
    if (s == NULL) return MR_ERR_INVALID;
    if (bytes_per_image != 0) {
      return mr_unsupported(c, "copy_into_texture handles 2D images only; a "
                               "non-zero bytes_per_image means an array or a "
                               "volume");
    }
    id<MTLTexture> t = (id<MTLTexture>)s->obj;
    if (t == nil) return MR_ERR_INVALID;
    if (t.storageMode == MTLStorageModePrivate) {
      return mr_unsupported(c, "copy_into_texture needs a CPU-visible texture; "
                               "a private one has no host address to copy from");
    }
    if (bytes == NULL || len < (size_t)bytes_per_row * t.height) {
      return MR_ERR_RANGE;
    }

    /*
     * This is an upload, not a GPU copy, and the command buffer argument is
     * deliberately unused.
     *
     * replaceRegion is what a CPU-side write into a texture is in Metal 3, and
     * D3D11's UpdateSubresource -- the call this maps to -- is documented as
     * exactly that an immediate CPU-side update. Metal orders it against
     * subsequent GPU work on the same queue, so not routing it through the
     * caller's command buffer does not lose the ordering the caller needs.
     */
    MTLRegion region = MTLRegionMake2D(0, 0, t.width, t.height);
    [t replaceRegion:region
         mipmapLevel:0
           withBytes:bytes
         bytesPerRow:(NSUInteger)bytes_per_row];
    return MR_OK;
  }
}

/* ----------------------------------------------------------------- encoders */

/*
 * Builds the Metal render pass descriptor the ABI's mr_render_pass_desc
 * describes. Kept apart from the encoder setup so the depth-stencil pairing rule
 * has one place to live.
 */
static void mr_fill_render_pass(mr_metal_ctx *c, MTLRenderPassDescriptor *rp,
                                const mr_render_pass_desc *desc) {
  for (uint32_t i = 0; i < desc->color_count && i < 8; i++) {
    if (desc->color[i] == MR_GFX_HANDLE_NONE) continue;
    mr_obj_slot *ts = mr_slot_as(c, desc->color[i], MR_OBJ_TEXTURE);
    if (ts == NULL) continue;
    MTLRenderPassColorAttachmentDescriptor *att = rp.colorAttachments[i];
    att.texture = (id<MTLTexture>)ts->obj;
    att.loadAction = desc->do_clear_color[i] ? MTLLoadActionClear
                                             : MTLLoadActionLoad;
    att.storeAction = desc->store_color[i] ? MTLStoreActionStore
                                           : MTLStoreActionDontCare;
    att.clearColor = MTLClearColorMake((double)desc->clear_color[0],
                                       (double)desc->clear_color[1],
                                       (double)desc->clear_color[2],
                                       (double)desc->clear_color[3]);
  }

  if (desc->depth != MR_GFX_HANDLE_NONE) {
    mr_obj_slot *ds = mr_slot_as(c, desc->depth, MR_OBJ_TEXTURE);
    if (ds != NULL) {
      id<MTLTexture> depth = (id<MTLTexture>)ds->obj;
      rp.depthAttachment.texture = depth;
      rp.depthAttachment.loadAction =
          desc->do_clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
      rp.depthAttachment.storeAction =
          desc->store_depth ? MTLStoreActionStore : MTLStoreActionDontCare;
      rp.depthAttachment.clearDepth = (double)desc->clear_depth_value;

      /*
       * A depth-stencil texture has to appear in both attachment slots. Metal
       * validates the pair: setting only depthAttachment on a format that has a
       * stencil component is a validation failure, and the message talks about
       * the stencil attachment rather than about the missing assignment.
       */
      MTLPixelFormat dsf = depth.pixelFormat;
      if (dsf == MTLPixelFormatDepth24Unorm_Stencil8 ||
          dsf == MTLPixelFormatDepth32Float_Stencil8) {
        rp.stencilAttachment.texture = depth;
        rp.stencilAttachment.loadAction = rp.depthAttachment.loadAction;
        rp.stencilAttachment.storeAction = rp.depthAttachment.storeAction;
        rp.stencilAttachment.clearStencil = (uint32_t)desc->clear_stencil_value;
      }
    }
  }
}

static mr_status mr_render_encoder_begin(mr_gfx_backend *self, mr_gfx_handle cmd,
                                         const mr_render_pass_desc *desc,
                                         mr_gfx_handle *out_encoder) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || desc == NULL || out_encoder == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    mr_obj_slot *cs = mr_slot_as(c, cmd, MR_OBJ_CMDBUF);
    if (cs == NULL) return MR_ERR_INVALID;
    id<MTLCommandBuffer> buffer = (id<MTLCommandBuffer>)cs->obj;
    if (buffer == nil) {
      return mr_state(c, "render_encoder_begin before begin_command_buffer");
    }
    if (c->current_encoder != MR_GFX_HANDLE_NONE) {
      return mr_state(c, "an encoder is already open; end it before beginning "
                         "another");
    }
    if (desc->color_count == 0 && desc->depth == MR_GFX_HANDLE_NONE) {
      return MR_ERR_INVALID;
    }

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    mr_fill_render_pass(c, rp, desc);
    if (desc->label != NULL) {
      rp.label = [NSString stringWithUTF8String:desc->label];
    }

    id<MTLRenderCommandEncoder> enc =
        [buffer renderCommandEncoderWithDescriptor:rp];
    if (enc == nil) {
      return mr_state(c, "Metal refused to make a render encoder (the pass had "
                         "no usable attachment)");
    }
    [enc retain];

    mr_gfx_handle h = mr_intern(c, MR_OBJ_ENCODER, enc);
    if (h == MR_GFX_HANDLE_NONE) {
      [enc release];
      return MR_ERR_NOMEM;
    }
    mr_slot(c, h)->value = cmd;
    c->current_encoder = h;
    *out_encoder = h;
    return MR_OK;
  }
}

static mr_status mr_render_encoder_end(mr_gfx_backend *self,
                                       mr_gfx_handle encoder) {
  mr_metal_ctx *c = mr_ctx(self);

  @autoreleasepool {
    mr_obj_slot *s = mr_slot_as(c, encoder, MR_OBJ_ENCODER);
    if (s == NULL) return MR_ERR_INVALID;
    id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)s->obj;
    if (enc == nil) {
      return mr_state(c, "this encoder has already been ended");
    }
    [enc endEncoding];

    /*
     * The Metal object is released here rather than left for teardown, and the
     * slot stays behind holding the handle.
     *
     * A command encoder is one-shot: once ended it cannot be reopened, and its
     * only remaining purpose is to hold the command it encoded. A game loop
     * makes one per frame per pass, so keeping them alive until the backend
     * dies would retain millions of objects and exhaust the address space long
     * before the game ended. Handing the memory back now while keeping the
     * handle valid means a stale draw returns a state error instead of
     * dereferencing a freed encoder.
     */
    [enc release];
    s->obj = nil;
    if (c != NULL && c->current_encoder == encoder) {
      c->current_encoder = MR_GFX_HANDLE_NONE;
    }
    return MR_OK;
  }
}

/* --------------------------------------------------------- draws and bindings */

static mr_status mr_draw(mr_gfx_backend *self, mr_gfx_handle encoder,
                         const mr_draw_args *args) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || args == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    mr_obj_slot *es = mr_slot_as(c, encoder, MR_OBJ_ENCODER);
    if (es == NULL) return MR_ERR_INVALID;
    id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)es->obj;
    if (enc == nil) return mr_state(c, "draw on an encoder that has been ended");
    if (args->vertex_count == 0 || args->instance_count == 0) return MR_OK;

    mr_obj_slot *ps = mr_slot_as(c, args->pipeline, MR_OBJ_PIPELINE);
    if (ps == NULL) return MR_ERR_INVALID;
    id<MTLRenderPipelineState> pso = (id<MTLRenderPipelineState>)ps->obj;
    if (pso == nil) return MR_ERR_INVALID;

    [enc setRenderPipelineState:pso];

    if (args->first_instance != 0) {
      /* The base-instance draw variant is the one whose availability differs
       * between Metal releases, and a wrong availability guard here is a
       * runtime crash rather than a compile error. Refusing says so. */
      return mr_unsupported(c, "non-zero first_instance needs the base-instance "
                               "draw call, which this backend does not use");
    }

    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:(NSUInteger)args->first_vertex
            vertexCount:(NSUInteger)args->vertex_count
          instanceCount:(NSUInteger)args->instance_count];
    return MR_OK;
  }
}

static mr_status mr_draw_indexed(mr_gfx_backend *self, mr_gfx_handle encoder,
                                 mr_gfx_handle index_buffer,
                                 uint32_t index_count, uint32_t index_offset) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    mr_obj_slot *es = mr_slot_as(c, encoder, MR_OBJ_ENCODER);
    if (es == NULL) return MR_ERR_INVALID;
    id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)es->obj;
    if (enc == nil) return mr_state(c, "draw on an encoder that has been ended");
    if (index_count == 0) return MR_OK;

    mr_obj_slot *is = mr_slot_as(c, index_buffer, MR_OBJ_BUFFER);
    if (is == NULL) return MR_ERR_INVALID;
    id<MTLBuffer> indices = (id<MTLBuffer>)is->obj;
    if (indices == nil) return MR_ERR_INVALID;

    /*
     * The index type is assumed to be 32-bit.
     *
     * mr_backend.h passes the index buffer, a count and an offset, and nothing
     * that says whether the indices are 16- or 32-bit, so there is no way to
     * read it off the ABI. D3D11 knows -- it is in the DXGI_FORMAT of the bound
     * index buffer -- and DXMT will have to pass it. Until the ABI grows that
     * argument this is the assumption, stated rather than hidden, and a 16-bit
     * index buffer will draw wrong rather than fail.
     */
    uint32_t stride = 4;
    if (index_offset % stride != 0) {
      return mr_unsupported(c, "a 16-bit index buffer needs an index type the "
                               "ABI does not carry; this backend reads 32-bit "
                               "indices");
    }

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:(NSUInteger)index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:indices
             indexBufferOffset:(NSUInteger)index_offset];
    return MR_OK;
  }
}

static mr_status mr_argument_table_create(mr_gfx_backend *self,
                                          const mr_argument_table_desc *desc,
                                          mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || desc == NULL || out == NULL) return MR_ERR_INVALID;

  mr_arg_table *t = (mr_arg_table *)calloc(1, sizeof(*t));
  if (t == NULL) return MR_ERR_NOMEM;
  t->max_buffers = desc->max_buffers;
  t->max_textures = desc->max_textures;
  t->max_samplers = desc->max_samplers;

  if (t->max_buffers != 0) {
    t->buffers = (mr_gfx_handle *)calloc(t->max_buffers, sizeof(mr_gfx_handle));
    t->offsets = (uint64_t *)calloc(t->max_buffers, sizeof(uint64_t));
  }
  if (t->max_textures != 0) {
    t->textures = (mr_gfx_handle *)calloc(t->max_textures, sizeof(mr_gfx_handle));
  }
  if (t->max_samplers != 0) {
    t->samplers = (mr_gfx_handle *)calloc(t->max_samplers, sizeof(mr_gfx_handle));
  }

  if ((t->max_buffers != 0 && (t->buffers == NULL || t->offsets == NULL)) ||
      (t->max_textures != 0 && t->textures == NULL) ||
      (t->max_samplers != 0 && t->samplers == NULL)) {
    free(t->buffers);
    free(t->offsets);
    free(t->textures);
    free(t->samplers);
    free(t);
    return MR_ERR_NOMEM;
  }

  mr_gfx_handle h = mr_intern(c, MR_OBJ_ARG_TABLE, nil);
  if (h == MR_GFX_HANDLE_NONE) {
    free(t->buffers);
    free(t->offsets);
    free(t->textures);
    free(t->samplers);
    free(t);
    return MR_ERR_NOMEM;
  }
  mr_slot(c, h)->data = t;
  *out = h;
  return MR_OK;
}

static void mr_argument_table_destroy(mr_gfx_backend *self, mr_gfx_handle table) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, table, MR_OBJ_ARG_TABLE);
  if (s == NULL) return;
  mr_arg_table *t = (mr_arg_table *)s->data;
  if (t != NULL) {
    free(t->buffers);
    free(t->offsets);
    free(t->textures);
    free(t->samplers);
  }
  mr_slot_free(c, s);
}

static mr_status mr_argument_table_set_buffer(mr_gfx_backend *self,
                                              mr_gfx_handle table,
                                              uint32_t index,
                                              mr_gfx_handle buffer,
                                              uint64_t offset) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, table, MR_OBJ_ARG_TABLE);
  if (s == NULL) return MR_ERR_INVALID;
  mr_arg_table *t = (mr_arg_table *)s->data;
  if (t == NULL || index >= t->max_buffers) return MR_ERR_RANGE;
  if (buffer != MR_GFX_HANDLE_NONE &&
      mr_slot_as(c, buffer, MR_OBJ_BUFFER) == NULL) {
    return MR_ERR_INVALID;
  }
  t->buffers[index] = buffer;
  t->offsets[index] = offset;
  return MR_OK;
}

static mr_status mr_argument_table_set_texture(mr_gfx_backend *self,
                                               mr_gfx_handle table,
                                               uint32_t index,
                                               mr_gfx_handle texture) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, table, MR_OBJ_ARG_TABLE);
  if (s == NULL) return MR_ERR_INVALID;
  mr_arg_table *t = (mr_arg_table *)s->data;
  if (t == NULL || index >= t->max_textures) return MR_ERR_RANGE;
  if (texture != MR_GFX_HANDLE_NONE &&
      mr_slot_as(c, texture, MR_OBJ_TEXTURE) == NULL) {
    return MR_ERR_INVALID;
  }
  t->textures[index] = texture;
  return MR_OK;
}

static mr_status mr_argument_table_set_sampler(mr_gfx_backend *self,
                                               mr_gfx_handle table,
                                               uint32_t index,
                                               mr_gfx_handle sampler) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, table, MR_OBJ_ARG_TABLE);
  if (s == NULL) return MR_ERR_INVALID;
  mr_arg_table *t = (mr_arg_table *)s->data;
  if (t == NULL || index >= t->max_samplers) return MR_ERR_RANGE;
  (void)sampler;
  t->samplers[index] = sampler;
  return MR_OK;
}

/*
 * Applies a table to an encoder.
 *
 * This is the emulation the header promises: Metal 3 has no argument table
 * object, so binding one means making the encoder calls the table stands for.
 *
 * Slots that were never set are bound as nil across the whole declared range,
 * rather than left alone. That is deliberate and it is the D3D11 semantic: a
 * shader resource view that D3D11 does not have bound must not see whatever the
 * previous draw left in the slot, and "leave it alone" is precisely how a
 * stale-binding bug renders a texture that the game stopped using an hour ago.
 */
static mr_status mr_bind_argument_table(mr_gfx_backend *self,
                                        mr_gfx_handle encoder, uint32_t stage,
                                        mr_gfx_handle table) {
  mr_metal_ctx *c = mr_ctx(self);

  @autoreleasepool {
    mr_obj_slot *es = mr_slot_as(c, encoder, MR_OBJ_ENCODER);
    if (es == NULL) return MR_ERR_INVALID;
    id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)es->obj;
    if (enc == nil) return mr_state(c, "bind on an encoder that has been ended");

    mr_obj_slot *ts = mr_slot_as(c, table, MR_OBJ_ARG_TABLE);
    if (ts == NULL) return MR_ERR_INVALID;
    mr_arg_table *t = (mr_arg_table *)ts->data;
    if (t == NULL) return MR_ERR_INVALID;

    bool vertex = (stage & MR_STAGE_VERTEX) != 0;
    bool fragment = (stage & MR_STAGE_FRAGMENT) != 0;
    if (!vertex && !fragment) {
      return mr_unsupported(c, "argument tables bind to the vertex and "
                               "fragment stages on this backend");
    }

    for (uint32_t i = 0; i < t->max_buffers; i++) {
      mr_gfx_handle bh = t->buffers[i];
      id<MTLBuffer> buffer = nil;
      NSUInteger offset = 0;
      if (bh != MR_GFX_HANDLE_NONE) {
        mr_obj_slot *bs = mr_slot_as(c, bh, MR_OBJ_BUFFER);
        if (bs != NULL) {
          buffer = (id<MTLBuffer>)bs->obj;
          offset = (NSUInteger)t->offsets[i];
        }
      }
      if (vertex) [enc setVertexBuffer:buffer offset:offset atIndex:i];
      if (fragment) [enc setFragmentBuffer:buffer offset:offset atIndex:i];
    }

    for (uint32_t i = 0; i < t->max_textures; i++) {
      id<MTLTexture> texture = nil;
      mr_gfx_handle th = t->textures[i];
      if (th != MR_GFX_HANDLE_NONE) {
        mr_obj_slot *xs = mr_slot_as(c, th, MR_OBJ_TEXTURE);
        if (xs != NULL) texture = (id<MTLTexture>)xs->obj;
      }
      if (vertex) [enc setVertexTexture:texture atIndex:i];
      if (fragment) [enc setFragmentTexture:texture atIndex:i];
    }

    for (uint32_t i = 0; i < t->max_samplers; i++) {
      id<MTLSamplerState> sampler = nil;
      mr_gfx_handle sh = t->samplers[i];
      if (sh != MR_GFX_HANDLE_NONE) {
        mr_obj_slot *xs = mr_slot(c, sh);
        if (xs != NULL) sampler = (id<MTLSamplerState>)xs->obj;
      }
      if (vertex) [enc setVertexSamplerState:sampler atIndex:i];
      if (fragment) [enc setFragmentSamplerState:sampler atIndex:i];
    }
    return MR_OK;
  }
}

/* ---------------------------------------------------------------- residency */

static mr_status mr_residency_set_create(mr_gfx_backend *self, const char *label,
                                         mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || out == NULL) return MR_ERR_INVALID;
  (void)label;

  mr_residency *r = (mr_residency *)calloc(1, sizeof(*r));
  if (r == NULL) return MR_ERR_NOMEM;
  r->capacity = 64;
  r->items = (mr_gfx_handle *)calloc(r->capacity, sizeof(mr_gfx_handle));
  if (r->items == NULL) {
    free(r);
    return MR_ERR_NOMEM;
  }

  mr_gfx_handle h = mr_intern(c, MR_OBJ_RESIDENCY, nil);
  if (h == MR_GFX_HANDLE_NONE) {
    free(r->items);
    free(r);
    return MR_ERR_NOMEM;
  }
  mr_slot(c, h)->data = r;
  *out = h;
  return MR_OK;
}

static void mr_residency_set_destroy(mr_gfx_backend *self, mr_gfx_handle set) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, set, MR_OBJ_RESIDENCY);
  if (s == NULL) return;
  mr_residency *r = (mr_residency *)s->data;
  if (r != NULL) free(r->items);
  mr_slot_free(c, s);
}

static mr_status mr_residency_add(mr_gfx_backend *self, mr_gfx_handle set,
                                  mr_gfx_handle allocation) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, set, MR_OBJ_RESIDENCY);
  if (s == NULL) return MR_ERR_INVALID;
  mr_residency *r = (mr_residency *)s->data;
  if (r == NULL) return MR_ERR_INVALID;
  if (mr_slot(c, allocation) == NULL) return MR_ERR_INVALID;
  if (r->count == r->capacity) return MR_ERR_NOMEM;
  r->items[r->count++] = allocation;
  return MR_OK;
}

static mr_status mr_residency_remove(mr_gfx_backend *self, mr_gfx_handle set,
                                     mr_gfx_handle allocation) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, set, MR_OBJ_RESIDENCY);
  if (s == NULL) return MR_ERR_INVALID;
  mr_residency *r = (mr_residency *)s->data;
  if (r == NULL) return MR_ERR_INVALID;
  for (uint32_t i = 0; i < r->count; i++) {
    if (r->items[i] != allocation) continue;
    r->items[i] = r->items[r->count - 1];
    r->count--;
    return MR_OK;
  }
  return MR_ERR_NOTFOUND;
}

static mr_status mr_residency_commit(mr_gfx_backend *self, mr_gfx_handle set) {
  mr_metal_ctx *c = mr_ctx(self);
  /* Metal 3 keeps resources resident by tracking what the encoders use, so
   * there is nothing to submit. Saying that is different from doing nothing for
   * no reason: the caller's residency set is honoured through use_residency_set
   * at encode time, and this call is the point where Metal 4 would make it
   * official. */
  if (mr_slot_as(c, set, MR_OBJ_RESIDENCY) == NULL) return MR_ERR_INVALID;
  return MR_OK;
}

static mr_status mr_use_residency_set(mr_gfx_backend *self,
                                      mr_gfx_handle encoder,
                                      mr_gfx_handle set) {
  mr_metal_ctx *c = mr_ctx(self);

  @autoreleasepool {
    mr_obj_slot *es = mr_slot_as(c, encoder, MR_OBJ_ENCODER);
    if (es == NULL) return MR_ERR_INVALID;
    id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)es->obj;
    if (enc == nil) return mr_state(c, "bind on an encoder that has been ended");

    mr_obj_slot *ss = mr_slot_as(c, set, MR_OBJ_RESIDENCY);
    if (ss == NULL) return MR_ERR_INVALID;
    mr_residency *r = (mr_residency *)ss->data;
    if (r == NULL) return MR_ERR_INVALID;

    for (uint32_t i = 0; i < r->count; i++) {
      mr_obj_slot *os = mr_slot(c, r->items[i]);
      if (os == NULL || os->obj == nil) continue;
      /* Read and write, because the set says the resource is in play, not what
       * for, and under-declaring the usage is the direction that risks a
       * dependency being missed. */
      [enc useResource:os->obj
                   usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    }
    return MR_OK;
  }
}

/* ---------------------------------------------------------------- pipelines */

static mr_status mr_library_load(mr_gfx_backend *self, const void *metallib,
                                 size_t len, mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || metallib == NULL || len == 0 || out == NULL) {
    return MR_ERR_INVALID;
  }

  @autoreleasepool {
    dispatch_data_t data =
        dispatch_data_create(metallib, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == NULL) return MR_ERR_NOMEM;

    NSError *error = nil;
    id<MTLLibrary> library = [c->device newLibraryWithData:data error:&error];
    /*
     * Which release applies depends on whether libdispatch objects are
     * Objective-C objects in this translation unit. Each spelling is correct in
     * its own configuration and does not compile in the other, and the compiler
     * is the only thing that knows which configuration this is.
     */
#if OS_OBJECT_USE_OBJC
    [data release];
#else
    dispatch_release(data);
#endif
    if (library == nil) {
      const char *text = error != nil ? error.localizedDescription.UTF8String
                                      : "unknown error";
      snprintf(c->reason, sizeof(c->reason),
               "Metal rejected the metallib: %s", text != NULL ? text : "");
      return MR_ERR_PARSE;
    }

    mr_gfx_handle h = mr_intern(c, MR_OBJ_LIBRARY, library);
    if (h == MR_GFX_HANDLE_NONE) {
      [library release];
      return MR_ERR_NOMEM;
    }
    *out = h;
    return MR_OK;
  }
}

static void mr_library_destroy(mr_gfx_backend *self, mr_gfx_handle library) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, library, MR_OBJ_LIBRARY);
  if (s == NULL) return;
  mr_slot_free(c, s);
}

static id<MTLFunction> mr_resolve_function(mr_metal_ctx *c,
                                           const mr_shader_function *fn,
                                           const char *stage,
                                           NSError **error_out) {
  if (fn == NULL || fn->library == MR_GFX_HANDLE_NONE || fn->function[0] == '\0') {
    return nil;
  }
  mr_obj_slot *ls = mr_slot_as(c, fn->library, MR_OBJ_LIBRARY);
  if (ls == NULL) return nil;
  id<MTLLibrary> lib = (id<MTLLibrary>)ls->obj;
  if (lib == nil) return nil;

  NSString *name = [NSString stringWithUTF8String:fn->function];
  if (name == nil) return nil;

  NSError *error = nil;
  id<MTLFunction> f = [lib newFunctionWithName:name];
  if (f == nil) {
    if (error_out != NULL) *error_out = error;
    snprintf(c->reason, sizeof(c->reason),
             "the %s function '%s' is not in that library", stage,
             fn->function);
    return nil;
  }
  return f; /* owned; the caller releases */
}

static mr_status mr_compile_render_pipeline(mr_gfx_backend *self,
                                            const mr_render_pipeline_desc *desc,
                                            mr_gfx_handle *out_pso) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL || desc == NULL || out_pso == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    if (desc->function_constant_count != 0) {
      /*
       * Refused rather than guessed.
       *
       * mr_render_pipeline_desc stores function constants as uint32 slots
       * documented as "(index, value) pairs", and says nothing about their
       * type: Metal needs to know whether each one is a bool, an int, a float
       * or a uint to build the constant values object, and the wrong answer
       * produces a shader that compiles and renders incorrectly. A silent
       * wrong frame is the failure mode this project cannot afford, so the ABI
       * has to say what the type is before this can be implemented.
       */
      return mr_unsupported(
          c, "function constants: mr_render_pipeline_desc does not say which "
             "Metal data type each (index, value) pair has");
    }

    MTLRenderPipelineDescriptor *d =
        [[MTLRenderPipelineDescriptor alloc] init];
    if (desc->label != NULL) {
      d.label = [NSString stringWithUTF8String:desc->label];
    }

    NSError *error = nil;
    id<MTLFunction> vs = mr_resolve_function(c, &desc->vertex, "vertex", &error);
    if (vs == nil) {
      [d release];
      return MR_ERR_NOTFOUND;
    }
    d.vertexFunction = vs;
    [vs release];

    if (desc->has_mesh || desc->has_object) {
      [d release];
      return mr_unsupported(c, "mesh and object shaders are not offered by the "
                               "Metal 3 backend");
    }

    id<MTLFunction> fs =
        mr_resolve_function(c, &desc->fragment, "fragment", &error);
    if (fs == nil) {
      [d release];
      return MR_ERR_NOTFOUND;
    }
    d.fragmentFunction = fs;
    [fs release];

    if (desc->color_count == 0 || desc->color_count > 8) {
      [d release];
      return MR_ERR_INVALID;
    }
    for (uint32_t i = 0; i < desc->color_count; i++) {
      MTLPixelFormat pf = MTLPixelFormatInvalid;
      if (!mr_to_pixel_format(desc->color_formats[i], &pf)) {
        [d release];
        return mr_unsupported(c, "a colour attachment format is not mapped by "
                                 "this backend");
      }
      d.colorAttachments[i].pixelFormat = pf;
    }

    if (desc->depth_format != MR_FMT_UNKNOWN) {
      MTLPixelFormat pf = MTLPixelFormatInvalid;
      if (!mr_to_pixel_format(desc->depth_format, &pf)) {
        [d release];
        return mr_unsupported(c, "the depth attachment format is not mapped by "
                                 "this backend");
      }
      d.depthAttachmentPixelFormat = pf;
      if (pf == MTLPixelFormatDepth24Unorm_Stencil8 ||
          pf == MTLPixelFormatDepth32Float_Stencil8) {
        d.stencilAttachmentPixelFormat = pf;
      }
    }

    d.rasterSampleCount =
        desc->raster_sample_count != 0 ? (NSUInteger)desc->raster_sample_count : 1;
    d.alphaToCoverageEnabled = desc->alpha_to_coverage;

    id<MTLRenderPipelineState> pso =
        [c->device newRenderPipelineStateWithDescriptor:d error:&error];
    [d release];
    if (pso == nil) {
      const char *text = error != nil ? error.localizedDescription.UTF8String
                                      : "unknown error";
      snprintf(c->reason, sizeof(c->reason), "Metal rejected the pipeline: %s",
               text != NULL ? text : "");
      return MR_ERR_PARSE;
    }

    mr_gfx_handle h = mr_intern(c, MR_OBJ_PIPELINE, pso);
    if (h == MR_GFX_HANDLE_NONE) {
      [pso release];
      return MR_ERR_NOMEM;
    }
    *out_pso = h;
    return MR_OK;
  }
}

static void mr_pipeline_destroy(mr_gfx_backend *self, mr_gfx_handle pso) {
  mr_metal_ctx *c = mr_ctx(self);
  mr_obj_slot *s = mr_slot_as(c, pso, MR_OBJ_PIPELINE);
  if (s == NULL) return;
  mr_slot_free(c, s);
}

/* ------------------------------------------------------------------ present */

mr_status mr_metal_read_texture(mr_gfx_backend *backend, mr_gfx_handle texture,
                                void *out, size_t out_len,
                                uint32_t bytes_per_row) {
  mr_metal_ctx *c = mr_ctx(backend);
  if (c == NULL || out == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    mr_obj_slot *s = mr_slot_as(c, texture, MR_OBJ_TEXTURE);
    if (s == NULL) return MR_ERR_INVALID;
    id<MTLTexture> t = (id<MTLTexture>)s->obj;
    if (t == nil) return MR_ERR_INVALID;
    if (t.storageMode == MTLStorageModePrivate) {
      return mr_unsupported(c, "read-back needs a CPU-visible texture; a private "
                               "one has no host address");
    }
    size_t need = (size_t)bytes_per_row * (size_t)t.height;
    if (out_len < need) {
      mr_reason(c, "the read-back buffer is smaller than the texture");
      return MR_ERR_RANGE;
    }
    MTLRegion region = MTLRegionMake2D(0, 0, t.width, t.height);
    [t getBytes:out
        bytesPerRow:(NSUInteger)bytes_per_row
         fromRegion:region
        mipmapLevel:0];
    return MR_OK;
  }
}

mr_status mr_metal_set_drawable_layer(mr_gfx_backend *backend, void *layer) {
  mr_metal_ctx *c = mr_ctx(backend);
  if (c == NULL || backend->kind != MR_BACKEND_METAL3) {
    return MR_ERR_UNSUPPORTED;
  }
  @autoreleasepool {
    if (c->layer != nil) { [c->layer release]; c->layer = nil; }
    if (layer != NULL) {
      c->layer = (id<CAMetalLayer>)layer;
      [c->layer retain];
      /* A CAMetalLayer with no device cannot produce a drawable, and a caller
       * has no reason to know which device the backend is using. Filling it in
       * only when it is unset leaves a caller that set its own alone. */
      if (c->layer.device == nil) c->layer.device = c->device;
    }
  }
  return MR_OK;
}

static mr_status mr_present(mr_gfx_backend *self, mr_gfx_handle texture) {
  mr_metal_ctx *c = mr_ctx(self);
  if (c == NULL) return MR_ERR_INVALID;

  @autoreleasepool {
    if (c->layer == nil) {
      return mr_state(c, "no drawable layer has been registered; call "
                         "mr_metal_set_drawable_layer() first");
    }
    mr_obj_slot *ts = mr_slot_as(c, texture, MR_OBJ_TEXTURE);
    if (ts == NULL) return MR_ERR_INVALID;
    id<MTLTexture> src = (id<MTLTexture>)ts->obj;
    if (src == nil) return MR_ERR_INVALID;

    id<CAMetalDrawable> drawable = [c->layer nextDrawable];
    if (drawable == nil) {
      return mr_state(c, "the layer had no drawable to give (it is not attached "
                         "to a window, or all of its drawables are in flight)");
    }

    id<MTLTexture> dst = drawable.texture;
    if (dst.pixelFormat != src.pixelFormat) {
      /* A format conversion belongs in a render pass with a blit or a shader.
       * Doing it silently here would be wrong; refusing names the mismatch. */
      return mr_unsupported(c, "the back buffer's pixel format differs from the "
                               "drawable's; present needs a matching format");
    }
    if (dst.width != src.width || dst.height != src.height) {
      return mr_unsupported(c, "the back buffer is not the size of the drawable; "
                               "the swap chain must match its window");
    }

    id<MTLCommandBuffer> cb = [c->queue commandBuffer];
    if (cb == nil) return mr_state(c, "the command queue refused to make a "
                                      "command buffer");
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (blit == nil) return mr_state(c, "Metal refused to make a blit encoder");
    [blit copyFromTexture:src toTexture:dst];
    [blit endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    return MR_OK;
  }
}

/* ------------------------------------------------------------- not yet here */

/*
 * The rest of the vtable, refused by name.
 *
 * Every one of these returns MR_ERR_UNSUPPORTED and leaves a reason instead of
 * succeeding quietly. The distinction matters more than it looks: a stub that
 * returned MR_OK would let a caller build a renderer on top of a feature that
 * does not exist, and the failure would arrive much later as a wrong image or a
 * hang, with nothing pointing back here.
 */
#define MR_UNSUPPORTED_NAMED(what)                                            \
  do {                                                                        \
    return mr_unsupported(c, what);                                            \
  } while (0)

static mr_status mr_no_compute_encoder_begin(mr_gfx_backend *self,
                                             mr_gfx_handle cmd,
                                             mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)cmd;
  (void)out;
  MR_UNSUPPORTED_NAMED("compute encoders are not implemented in this backend");
}

static mr_status mr_no_compute_encoder_end(mr_gfx_backend *self,
                                           mr_gfx_handle encoder) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  MR_UNSUPPORTED_NAMED("compute encoders are not implemented in this backend");
}

static mr_status mr_no_dispatch(mr_gfx_backend *self, mr_gfx_handle encoder,
                                const mr_dispatch_args *args) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)args;
  MR_UNSUPPORTED_NAMED("dispatch needs a compute encoder, which this backend "
                       "does not implement");
}

static mr_status mr_no_barrier(mr_gfx_backend *self, mr_gfx_handle encoder,
                               const mr_barrier_desc *desc) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)desc;
  MR_UNSUPPORTED_NAMED("explicit barriers: Metal 3 tracks hazards implicitly, "
                       "so there is no barrier to insert");
}

static mr_status mr_no_fence_create(mr_gfx_backend *self, mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)out;
  MR_UNSUPPORTED_NAMED("fences are not implemented; commit waits are done with "
                       "mr_metal_wait_idle() for now");
}

static mr_status mr_no_update_fence(mr_gfx_backend *self, mr_gfx_handle encoder,
                                    mr_gfx_handle fence, uint32_t stages) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)fence;
  (void)stages;
  MR_UNSUPPORTED_NAMED("fences are not implemented");
}

static mr_status mr_no_wait_for_fence(mr_gfx_backend *self,
                                      mr_gfx_handle encoder, mr_gfx_handle fence,
                                      uint32_t stages) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)fence;
  (void)stages;
  MR_UNSUPPORTED_NAMED("fences are not implemented");
}

static mr_status mr_no_shared_event_create(mr_gfx_backend *self,
                                           mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)out;
  MR_UNSUPPORTED_NAMED("shared events are not implemented; they are the "
                       "cross-queue and cross-process half of D3D11 syncing");
}

static mr_status mr_no_signal_event(mr_gfx_backend *self, mr_gfx_handle encoder,
                                    mr_gfx_handle event, uint64_t value) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)event;
  (void)value;
  MR_UNSUPPORTED_NAMED("shared events are not implemented");
}

static mr_status mr_no_wait_for_event(mr_gfx_backend *self,
                                      mr_gfx_handle encoder, mr_gfx_handle event,
                                      uint64_t value) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)encoder;
  (void)event;
  (void)value;
  MR_UNSUPPORTED_NAMED("shared events are not implemented");
}

static mr_status mr_no_heap_create(mr_gfx_backend *self,
                                   const mr_heap_desc *desc,
                                   mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)desc;
  (void)out;
  MR_UNSUPPORTED_NAMED("heaps: D3D11's placement resources need MTLHeap "
                       "sub-allocation, which this backend does not do yet");
}

static mr_status mr_no_compile_compute_pipeline(
    mr_gfx_backend *self, const mr_compute_pipeline_desc *desc,
    mr_gfx_handle *out_pso) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)desc;
  (void)out_pso;
  MR_UNSUPPORTED_NAMED("compute pipelines are not implemented in this backend");
}

static mr_status mr_no_compile_render_pipeline_async(
    mr_gfx_backend *self, const mr_render_pipeline_desc *desc,
    mr_compiler_task *task) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)desc;
  (void)task;
  MR_UNSUPPORTED_NAMED("asynchronous pipeline compilation: the ABI's task "
                       "handle has no completion path wired up yet");
}

static mr_status mr_no_await_compiler_task(mr_gfx_backend *self,
                                           mr_compiler_task *task,
                                           mr_gfx_handle *out_pso) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)task;
  (void)out_pso;
  MR_UNSUPPORTED_NAMED("asynchronous pipeline compilation is not implemented");
}

static mr_status mr_no_archive_open(mr_gfx_backend *self, const char *path,
                                    bool for_writing, mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)path;
  (void)for_writing;
  (void)out;
  MR_UNSUPPORTED_NAMED("binary archives: the shader cache's pipeline half is "
                       "not wired to MTLBinaryArchive yet");
}

static mr_status mr_no_archive_add_pipeline(mr_gfx_backend *self,
                                            mr_gfx_handle archive,
                                            mr_gfx_handle pso) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)archive;
  (void)pso;
  MR_UNSUPPORTED_NAMED("binary archives are not implemented");
}

static mr_status mr_no_archive_serialize(mr_gfx_backend *self,
                                         mr_gfx_handle archive) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)archive;
  MR_UNSUPPORTED_NAMED("binary archives are not implemented");
}

static void mr_no_archive_close(mr_gfx_backend *self, mr_gfx_handle archive) {
  (void)self;
  (void)archive;
}

static mr_status mr_sampler_create(mr_gfx_backend *self, const void *state,
                                   mr_gfx_handle *out) {
  mr_metal_ctx *c = mr_ctx(self);
  (void)state;
  (void)out;
  /*
   * mr_backend.h takes an opaque `const void *state` and does not define a
   * layout for it, so there is no way to read D3D11's sampler description out
   * of it. Guessing a struct here would produce a sampler that compiles and
   * filters wrongly, which is the one outcome that is worse than a refusal.
   */
  MR_UNSUPPORTED_NAMED("sampler_create: the ABI passes sampler state as an "
                       "opaque blob with no defined layout");
}

/* ----------------------------------------------------------------- assembly */

static void mr_metal_destroy_v(mr_gfx_backend *self) {
  mr_metal_destroy_backend(self);
}

const char *mr_metal_last_reason(const mr_gfx_backend *backend) {
  const mr_metal_ctx *c = (const mr_metal_ctx *)backend;
  if (c == NULL) return "";
  return c->reason;
}

const char *mr_metal_last_commit_status(const mr_gfx_backend *backend) {
  const mr_metal_ctx *c = (const mr_metal_ctx *)backend;
  if (c == NULL) return "";
  return c->commit_status;
}

static mr_metal_ctx *mr_metal_open(const mr_host_caps *host, const char **reason) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      if (reason != NULL) {
        *reason = "MTLCreateSystemDefaultDevice returned nil";
      }
      return NULL;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
      if (reason != NULL) *reason = "the Metal device refused a command queue";
      return NULL;
    }

    mr_metal_ctx *c = (mr_metal_ctx *)calloc(1, sizeof(*c));
    if (c == NULL) {
      [queue release];
      if (reason != NULL) *reason = "out of memory";
      return NULL;
    }

    c->slot_count = 4096;
    c->slots = (mr_obj_slot *)calloc(c->slot_count, sizeof(mr_obj_slot));
    if (c->slots == NULL) {
      [queue release];
      free(c);
      if (reason != NULL) *reason = "out of memory";
      return NULL;
    }

    c->pub.name = "metal3";
    c->pub.kind = MR_BACKEND_METAL3;
    c->pub.ctx = c;
    c->device = [device retain];
    c->queue = [queue retain];
    [queue release];
    c->current_encoder = MR_GFX_HANDLE_NONE;
    c->layer = nil;
    mr_reason(c, "");

    mr_gfx_backend *pub = &c->pub;
    pub->caps = mr_metal_caps;
    pub->destroy = mr_metal_destroy_v;

    pub->allocator_create = mr_allocator_create;
    pub->allocator_reset = mr_allocator_reset;
    pub->allocator_destroy = mr_allocator_destroy;
    pub->command_buffer_create = mr_command_buffer_create;
    pub->command_buffer_destroy = mr_command_buffer_destroy;
    pub->begin_command_buffer = mr_begin_command_buffer;
    pub->end_command_buffer = mr_end_command_buffer;
    pub->commit = mr_commit;

    pub->render_encoder_begin = mr_render_encoder_begin;
    pub->render_encoder_end = mr_render_encoder_end;
    pub->compute_encoder_begin = mr_no_compute_encoder_begin;
    pub->compute_encoder_end = mr_no_compute_encoder_end;
    pub->draw = mr_draw;
    pub->dispatch = mr_no_dispatch;
    pub->draw_indexed = mr_draw_indexed;

    pub->argument_table_create = mr_argument_table_create;
    pub->argument_table_destroy = mr_argument_table_destroy;
    pub->argument_table_set_buffer = mr_argument_table_set_buffer;
    pub->argument_table_set_texture = mr_argument_table_set_texture;
    pub->argument_table_set_sampler = mr_argument_table_set_sampler;
    pub->bind_argument_table = mr_bind_argument_table;

    pub->residency_set_create = mr_residency_set_create;
    pub->residency_set_destroy = mr_residency_set_destroy;
    pub->residency_add = mr_residency_add;
    pub->residency_remove = mr_residency_remove;
    pub->residency_commit = mr_residency_commit;
    pub->use_residency_set = mr_use_residency_set;

    pub->barrier = mr_no_barrier;
    pub->fence_create = mr_no_fence_create;
    pub->update_fence = mr_no_update_fence;
    pub->wait_for_fence = mr_no_wait_for_fence;
    pub->shared_event_create = mr_no_shared_event_create;
    pub->signal_event = mr_no_signal_event;
    pub->wait_for_event = mr_no_wait_for_event;

    pub->buffer_create = mr_buffer_create;
    pub->texture_create = mr_texture_create;
    pub->heap_create = mr_no_heap_create;
    pub->resource_destroy = mr_resource_destroy;
    pub->buffer_contents = mr_buffer_contents;
    pub->copy_into_texture = mr_copy_into_texture;

    pub->library_load = mr_library_load;
    pub->library_destroy = mr_library_destroy;
    pub->compile_render_pipeline = mr_compile_render_pipeline;
    pub->compile_compute_pipeline = mr_no_compile_compute_pipeline;
    pub->compile_render_pipeline_async = mr_no_compile_render_pipeline_async;
    pub->await_compiler_task = mr_no_await_compiler_task;
    pub->pipeline_destroy = mr_pipeline_destroy;

    pub->archive_open = mr_no_archive_open;
    pub->archive_add_pipeline = mr_no_archive_add_pipeline;
    pub->archive_serialize = mr_no_archive_serialize;
    pub->archive_close = mr_no_archive_close;

    pub->sampler_create = mr_sampler_create;
    pub->present = mr_present;

    (void)host;
    if (reason != NULL) {
      *reason = "the Metal 3 backend is available";
    }
    return c;
  }
}

mr_gfx_backend *mr_gfx_backend_create_apple(const mr_host_caps *host,
                                            const char **reason) {
  if (host == NULL) {
    if (reason != NULL) *reason = "no host capabilities were provided";
    return NULL;
  }

  const char *pick = NULL;
  mr_backend want = mr_host_pick_backend(host, true, &pick);
  if (want == MR_BACKEND_NONE) {
    if (reason != NULL) *reason = pick != NULL ? pick : "no Metal backend";
    return NULL;
  }

  /*
   * Metal 4 is chosen by the probe but not yet implemented. Falling back to
   * Metal 3 here, with the reason saying so, is the behaviour that keeps the
   * runtime honest: a device that reports Metal 4 gets a working frame path and
   * a log line explaining why it is not using the newer one, instead of a NULL
   * backend and a launch that fails for no visible reason.
   */
  if (want == MR_BACKEND_METAL4) {
    mr_metal_ctx *c = mr_metal_open(host, reason);
    if (c == NULL) return NULL;
    if (reason != NULL) {
      *reason = "this device supports Metal 4 but only the Metal 3 backend is "
                "implemented, so the frame path is running on Metal 3";
    }
    return &c->pub;
  }

  mr_metal_ctx *c = mr_metal_open(host, reason);
  if (c == NULL) return NULL;
  return &c->pub;
}
