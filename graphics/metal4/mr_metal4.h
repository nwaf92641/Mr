/* Mr — Metal 4 layer.
 *
 * The one place in Mr that names MTL4 types. D3D11 (through DXMT) and D3D12
 * both translate into this, so neither has to know what a Metal 4 object is,
 * and an API rename lands in one file instead of two backends.
 *
 * This header is C, not Objective-C: the Metal objects live behind opaque
 * handles. Callers hand back the same handle they were given, and the type
 * names below carry the Metal type only as a comment.
 *
 * Availability: Metal 4 is iOS 26 / macOS 26 and A14 Bionic / M1 or later.
 * mr_mtl4_available() answers for the running device; every other function
 * fails cleanly (NULL / false) when it is 0.
 */
#ifndef MR_METAL4_H
#define MR_METAL4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MTLPrimitiveType, MTLIndexType, MTLLoadAction, MTLStoreAction. The values
 * are asserted against Metal's in mr_metal4.mm, so these are not a guess. */
typedef enum {
  MR_MTL4_PRIMITIVE_POINT = 0,
  MR_MTL4_PRIMITIVE_LINE = 1,
  MR_MTL4_PRIMITIVE_LINE_STRIP = 2,
  MR_MTL4_PRIMITIVE_TRIANGLE = 3,
  MR_MTL4_PRIMITIVE_TRIANGLE_STRIP = 4,
} mr_mtl4_primitive_type;

typedef enum {
  MR_MTL4_INDEX_UINT16 = 0,
  MR_MTL4_INDEX_UINT32 = 1,
} mr_mtl4_index_type;

typedef enum {
  MR_MTL4_LOAD_DONT_CARE = 0,
  MR_MTL4_LOAD_LOAD = 1,
  MR_MTL4_LOAD_CLEAR = 2,
} mr_mtl4_load_action;

typedef enum {
  MR_MTL4_STORE_DONT_CARE = 0,
  MR_MTL4_STORE_STORE = 1,
  MR_MTL4_STORE_MULTISAMPLE_RESOLVE = 2,
} mr_mtl4_store_action;

/* Render stages, matching MTLRenderStage. */
typedef enum {
  MR_MTL4_STAGE_VERTEX = 1u << 0,
  MR_MTL4_STAGE_FRAGMENT = 1u << 1,
  MR_MTL4_STAGE_TILE = 1u << 2,
  MR_MTL4_STAGE_OBJECT = 1u << 3,
  MR_MTL4_STAGE_MESH = 1u << 4,
} mr_mtl4_stage;

typedef struct mr_mtl4_device mr_mtl4_device;                 /* id<MTLDevice>            */
typedef struct mr_mtl4_table mr_mtl4_table;                   /* id<MTL4ArgumentTable>    */
typedef struct mr_mtl4_frame mr_mtl4_frame;                   /* allocator + buffer + state */

/* ------------------------------------------------------------------ probe */

/* 1 when the running device reports MTLGPUFamilyMetal4. This is the gate: it
 * is what separates "Metal 4 is the target" from "Metal 4 is running". */
int mr_mtl4_available(void);

/* A short reason for the last failure, or "" if there has not been one. Never
 * NULL, so a caller can log it unconditionally. */
const char *mr_mtl4_last_error(void);

/* ----------------------------------------------------------------- device */

/* Owns the MTLDevice and the Metal 4 factories. NULL when Metal 4 is
 * unavailable; check mr_mtl4_available() first to tell that apart from a
 * failed allocation. */
mr_mtl4_device *mr_mtl4_device_create(void);
void mr_mtl4_device_destroy(mr_mtl4_device *device);

/* The MTLDevice name, for logs. Valid until the device is destroyed. */
const char *mr_mtl4_device_name(mr_mtl4_device *device);

/* The raw id<MTLDevice>, for the parts of DXMT that still create Metal 3
 * objects (buffers, textures, samplers, pipeline states). Those objects are
 * unchanged in Metal 4 and are bound through an argument table. */
void *mr_mtl4_device_metal_device(mr_mtl4_device *device);

/* The raw id<MTL4CommandQueue>, for residency attachment. Metal 4 requires
 * every resource read at draw or dispatch time -- argument-table bindings
 * included -- to be in a residency set attached to the queue, which Metal 3 did
 * not ask for. mr_mtl4_device_add_residency_set is the supported way to do it. */
void *mr_mtl4_device_queue(mr_mtl4_device *device);

/* Attaches a residency set (id<MTLResidencySet>) to the frame queue, so bound
 * buffers and textures are resident at draw time. False if Metal rejects it. */
bool mr_mtl4_device_add_residency_set(mr_mtl4_device *device, void *mtl_residency_set);

/* One argument table per encoding thread is the model: tables are reusable
 * across encoders and frames, so they are sized for the widest shader set they
 * will see, not per draw. stages selects where the table is used; a render
 * encoder needs at least vertex and fragment. Returns NULL on failure, with
 * mr_mtl4_last_error() set. */
mr_mtl4_table *mr_mtl4_device_new_table(mr_mtl4_device *device, uint32_t max_buffers,
                                        uint32_t max_textures, uint32_t max_samplers,
                                        unsigned stages, bool attribute_strides);
void mr_mtl4_table_destroy(mr_mtl4_table *table);

/* The raw id<MTL4ArgumentTable>, for the winemetal bridge, which issues the
 * bindings itself while keeping the address and resource-ID translation in the
 * layer. */
void *mr_mtl4_table_metal_table(mr_mtl4_table *table);

/* ------------------------------------------------------------- argument table */

/* A buffer is bound by GPU address, so binding a region is
 * mr_mtl4_table_set_buffer(..., address + offset, ...) rather than an offset
 * argument: Metal 4 has no per-encoder setBuffer:offset:atIndex:. */
void mr_mtl4_table_set_buffer(mr_mtl4_table *table, uint64_t gpu_address, uint32_t index);
void mr_mtl4_table_set_buffer_strided(mr_mtl4_table *table, uint64_t gpu_address, uint32_t stride,
                                      uint32_t index);

/* Textures and samplers are bound whole, by resource ID. The layer derives
 * that ID, so callers pass the object they already have. */
void mr_mtl4_table_set_texture(mr_mtl4_table *table, void *mtl_texture, uint32_t index);
void mr_mtl4_table_set_sampler(mr_mtl4_table *table, void *mtl_sampler, uint32_t index);

/* ------------------------------------------------------------------ frames */

/* One frame owns one command allocator and one command buffer: the Metal 4
 * rule is one allocator per command buffer open at a time, and a command
 * buffer is created once from the device and reused, not created per frame
 * from the queue. */
mr_mtl4_frame *mr_mtl4_device_new_frame(mr_mtl4_device *device, const char *label);
void mr_mtl4_frame_destroy(mr_mtl4_frame *frame);

/* Resets the allocator and opens the command buffer. False on failure. */
bool mr_mtl4_frame_begin(mr_mtl4_frame *frame);

/* ---------------------------------------------------------------- encoders */

/* rp_desc is an MTL4RenderPassDescriptor from the render pass helpers below.
 * Only one encoder may be open at a time; the layer returns false rather than
 * letting Metal raise. */
bool mr_mtl4_frame_begin_render_pass(mr_mtl4_frame *frame, void *rp_desc);
bool mr_mtl4_frame_begin_render_pass_suspended(mr_mtl4_frame *frame, void *rp_desc);
bool mr_mtl4_frame_begin_render_pass_resumed(mr_mtl4_frame *frame, void *rp_desc);
bool mr_mtl4_frame_begin_compute_pass(mr_mtl4_frame *frame);
bool mr_mtl4_frame_end_encoder(mr_mtl4_frame *frame);
bool mr_mtl4_frame_encoder_is_open(const mr_mtl4_frame *frame);

/* Binds the table for the open encoder. Render encoders take a stage mask;
 * a compute encoder binds the whole encoder. */
void mr_mtl4_frame_bind_table(mr_mtl4_frame *frame, mr_mtl4_table *table);

/* ------------------------------------------------------------- render state */

/* A pipeline state created the Metal 3 way still works on a Metal 4 encoder,
 * so pipeline creation can move to MTL4Compiler later without blocking
 * bring-up. Until it does, this is how a PSO reaches the encoder. */
void mr_mtl4_frame_set_render_pipeline(mr_mtl4_frame *frame, void *mtl_pso);
void mr_mtl4_frame_set_compute_pipeline(mr_mtl4_frame *frame, void *mtl_pso);

void mr_mtl4_frame_set_viewport(mr_mtl4_frame *frame, double x, double y, double width,
                                double height, double znear, double zfar);
void mr_mtl4_frame_set_scissor(mr_mtl4_frame *frame, uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height);
void mr_mtl4_frame_set_blend_color(mr_mtl4_frame *frame, float r, float g, float b, float a);
void mr_mtl4_frame_set_stencil_reference(mr_mtl4_frame *frame, uint32_t value);

/* --------------------------------------------------------------- encoding */

/* The index buffer and its range come in as GPU addresses, exactly as the
 * Metal 4 selector takes them. index_buffer_length is the full range
 * accessible from index_address, not index_count * index_size: Metal 4 uses it
 * to bounds-check index values, and a short length clamps silently. */
void mr_mtl4_frame_draw(mr_mtl4_frame *frame, mr_mtl4_primitive_type type, uint32_t vertex_start,
                        uint32_t vertex_count, uint32_t instance_count, uint32_t base_instance);
void mr_mtl4_frame_draw_indexed(mr_mtl4_frame *frame, mr_mtl4_primitive_type type,
                                uint32_t index_count, mr_mtl4_index_type index_type,
                                uint64_t index_address, uint64_t index_buffer_length,
                                uint32_t instance_count, int32_t base_vertex,
                                uint32_t base_instance);

void mr_mtl4_frame_dispatch(mr_mtl4_frame *frame, uint32_t groups_x, uint32_t groups_y,
                            uint32_t groups_z, uint32_t threads_x, uint32_t threads_y,
                            uint32_t threads_z);

/* Blit on the compute encoder: Metal 4 has no blit encoder. */
void mr_mtl4_frame_copy_buffer(mr_mtl4_frame *frame, void *src_buffer, uint64_t src_offset,
                               void *dst_buffer, uint64_t dst_offset, uint64_t size);
void mr_mtl4_frame_fill_buffer(mr_mtl4_frame *frame, void *buffer, uint64_t offset,
                               uint64_t length, uint8_t value);
void mr_mtl4_frame_generate_mipmaps(mr_mtl4_frame *frame, void *mtl_texture);

/* ---------------------------------------------------------- submit / present */

/* Closes the encoder if one is open, ends the command buffer and commits the
 * batch. Every submission goes through here: the queue batches, so there is no
 * per-command-buffer commit.
 *
 * A committed frame is in flight until it is waited on. Reusing its allocator
 * before then is a Metal 4 validation error, not a race, so
 * mr_mtl4_frame_begin() refuses instead. Either wait, or hold more than one
 * frame -- one per in-flight frame is the model. */
bool mr_mtl4_frame_commit(mr_mtl4_frame *frame);
bool mr_mtl4_frame_is_in_flight(const mr_mtl4_frame *frame);

/* Waits for the frame's GPU work through a shared event the layer signals at
 * commit (Metal 4's replacement for addCompletedHandler). timeout_ms = 0 waits
 * forever. False on timeout. */
bool mr_mtl4_frame_wait(mr_mtl4_frame *frame, uint64_t timeout_ms);

/* Presents through the queue rather than the command buffer, which is the
 * shape Metal 4 requires: the GPU waits for the drawable, the batch is
 * committed, the queue signals the drawable, the drawable presents. */
bool mr_mtl4_frame_present(mr_mtl4_frame *frame, void *mtl_drawable);

/* GPU-side wait/signal, used for cross-queue ordering. Both are queue
 * operations in Metal 4; the command buffer has neither. */
void mr_mtl4_frame_wait_for_event(mr_mtl4_frame *frame, void *mtl_shared_event, uint64_t value);
void mr_mtl4_frame_signal_event(mr_mtl4_frame *frame, void *mtl_shared_event, uint64_t value);

/* ---------------------------------------------------- render pass descriptor */

/* An MTL4RenderPassDescriptor, so a Metal 4 pass is built without the caller
 * naming the class. Attachments hold id<MTLTexture>. */
void *mr_mtl4_render_pass_create(void);
void mr_mtl4_render_pass_destroy(void *rp_desc);

void mr_mtl4_render_pass_set_size(void *rp_desc, uint32_t width, uint32_t height);

void mr_mtl4_render_pass_set_color(void *rp_desc, uint32_t index, void *mtl_texture,
                                   mr_mtl4_load_action load, mr_mtl4_store_action store,
                                   double clear_r, double clear_g, double clear_b, double clear_a);
void mr_mtl4_render_pass_set_color_resolve(void *rp_desc, uint32_t index,
                                           void *mtl_resolve_texture);
void mr_mtl4_render_pass_set_depth(void *rp_desc, void *mtl_texture, mr_mtl4_load_action load,
                                   mr_mtl4_store_action store, double clear_depth);
void mr_mtl4_render_pass_set_stencil(void *rp_desc, void *mtl_texture, mr_mtl4_load_action load,
                                     mr_mtl4_store_action store, uint32_t clear_stencil);

/* ------------------------------------------------------ transient allocator */

/* Metal 4 removed set*Bytes, so short-lived constants have to live in a real
 * buffer. This hands out suballocations from one Shared buffer and returns the
 * GPU address to bind. Reset once per frame, after the GPU has finished the
 * previous one. */
typedef struct mr_mtl4_transient mr_mtl4_transient;

mr_mtl4_transient *mr_mtl4_transient_create(void *mtl_device, uint64_t capacity);
void mr_mtl4_transient_destroy(mr_mtl4_transient *transient);

/* Returns the GPU address to bind, or 0 when the write would not fit -- a
 * silent overflow here corrupts vertex and constant data rather than crashing,
 * so it is reported. */
uint64_t mr_mtl4_transient_write(mr_mtl4_transient *transient, const void *data, uint64_t size);
void mr_mtl4_transient_reset(mr_mtl4_transient *transient);
uint64_t mr_mtl4_transient_used(const mr_mtl4_transient *transient);
uint64_t mr_mtl4_transient_capacity(const mr_mtl4_transient *transient);
void *mr_mtl4_transient_buffer(mr_mtl4_transient *transient);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MR_METAL4_H */
