/*
 * The graphics backend ABI.
 *
 * Read this first: it is a test and conformance interface, not the runtime's
 * frame path.
 *
 * It was written as the contract between DXMT's Unix-side Metal layer and the
 * Metal implementation underneath it, so that a Mr Metal backend could sit
 * between the two. Reading DXMT's source settled that it cannot, and the evidence
 * is in docs/graphics-path-study.md against upstream 3Shain/dxmt 7c8dee1c:
 *
 *   - DXMT's unix side calls Metal directly; there is no seam under it to
 *     implement, only its own body to rewrite.
 *   - Wine's macdrv creates the MTLDevice and the CAMetalLayer. DXMT never calls
 *     MTLCreateSystemDefaultDevice. A backend here would own a second device that
 *     cannot share resources with, or present through, Wine's layer.
 *   - DXMT already has two Metal abstractions, WMT:: in Metal.hpp and src/dxmt/.
 *     A third would duplicate one of them and put a dispatch in front of every
 *     draw.
 *
 * So Metal 4 is implemented inside DXMT, as a patch series. This header keeps the
 * job it is actually good at: letting a test exercise a Metal frame path with no
 * Wine, no FEX and no DXMT in the way, so a failure is attributable to one layer.
 * metal/mr_backend_metal3.m draws into its own texture for exactly that reason.
 *
 * The reasoning below about the ABI being shaped by Metal 4 rather than Metal 3
 * still stands. It is now DXMT's problem rather than a layer boundary's, which is
 * the better place for it: the costs it describes are paid inside the component
 * that owns the D3D11 semantics.
 *
 * Only one ABI exists, and it is shaped by Metal 4 rather than by Metal 3,
 * because the two Metal generations differ in ways that cannot be hidden at the
 * call site without giving up what Metal 4 is for:
 *
 *   Metal 3                              Metal 4
 *   -----------------------------------  -------------------------------------
 *   the queue owns command buffer memory  an MTL4CommandAllocator does
 *   encoder binding via setBuffer:...     an MTL4ArgumentTable does
 *   implicit hazard tracking              explicit barriers; no tracking
 *   useResource: / useHeap:               MTLResidencySet
 *   per-encoder draw/dispatch encoders    one unified MTL4CommandEncoder
 *   sync compilation only                 MTL4Compiler + async MTL4CompilerTask
 *
 * Had the ABI been written for Metal 3, every Metal 4 path would have had to
 * reconstruct allocators, argument tables and barriers from a vocabulary that
 * does not contain them, and the result would be a Metal 3 emulation running on
 * Metal 4 -- which is worse than Metal 3 on Metal 3.
 *
 * Instead the ABI states the modern model, and the Metal 3 backend implements
 * that model by emulation. The mapping is exact and is the reason this design
 * costs nothing on older hardware:
 *
 *   allocator           -> a ring of MTLCommandBuffers, one per frame in flight
 *   argument table      -> the binding calls the encoders already take
 *   residency set       -> useResource:/useHeap: on the encoder
 *   explicit barrier    -> MTLBarrierScope on an encoder boundary, or a split
 *                          pass where the scopes cannot express the dependency
 *   MTL4CompilerTask    -> a compilation on a worker queue behind the same
 *                          handle, so callers await the same way
 *
 * So a caller written against this header is correct on both generations, and
 * choosing Metal 4 is a capability decision made once, in mr_host_pick_backend,
 * rather than an #ifdef spread across the renderer.
 *
 * Handles, not pointers: this ABI was designed to be crossed from a PE side,
 * where a host pointer is meaningless, and the test binary and a future in-Wine
 * caller both keep that property.
 */
#ifndef MR_BACKEND_H
#define MR_BACKEND_H

#include "mr/mr_host.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t mr_gfx_handle; /* 0 is never a valid handle */
#define MR_GFX_HANDLE_NONE ((mr_gfx_handle)0)

/* ---------------------------------------------------------------- formats */

typedef enum {
  MR_FMT_UNKNOWN = 0,
  MR_FMT_R8_UNORM, MR_FMT_R8G8_UNORM, MR_FMT_R8G8B8A8_UNORM, MR_FMT_R8G8B8A8_UNORM_SRGB,
  MR_FMT_B8G8R8A8_UNORM, MR_FMT_B8G8R8A8_UNORM_SRGB,
  MR_FMT_R16_FLOAT, MR_FMT_R16G16_FLOAT, MR_FMT_R16G16B16A16_FLOAT,
  MR_FMT_R32_FLOAT, MR_FMT_R32G32_FLOAT, MR_FMT_R32G32B32A32_FLOAT,
  MR_FMT_R32_UINT, MR_FMT_R32_SINT,
  MR_FMT_D16_UNORM, MR_FMT_D32_FLOAT, MR_FMT_D24_UNORM_S8_UINT,
  MR_FMT_BC1_RGBA_UNORM, MR_FMT_BC2_RGBA_UNORM, MR_FMT_BC3_RGBA_UNORM,
  MR_FMT_BC4_R_UNORM, MR_FMT_BC5_RG_UNORM, MR_FMT_BC6H_RGB_UFLOAT,
  MR_FMT_BC7_RGBA_UNORM,
  MR_FMT_COUNT,
} mr_format;

uint32_t mr_format_bytes_per_pixel(mr_format f);
bool mr_format_is_depth(mr_format f);
bool mr_format_is_block_compressed(mr_format f);

/* ------------------------------------------------------------------ usage */

typedef enum {
  MR_USAGE_UNKNOWN = 0,
  MR_USAGE_SHADER_READ = 1u << 0,
  MR_USAGE_SHADER_WRITE = 1u << 1,
  MR_USAGE_RENDER_TARGET = 1u << 2,
  MR_USAGE_DEPTH_STENCIL = 1u << 3,
  MR_USAGE_TRANSFER_SRC = 1u << 4,
  MR_USAGE_TRANSFER_DST = 1u << 5,
  MR_USAGE_VERTEX = 1u << 6,
  MR_USAGE_INDEX = 1u << 7,
  MR_USAGE_CONSTANT = 1u << 8,
  MR_USAGE_TILE_MEMORY_ONLY = 1u << 9, /* StorageModeMemoryless */
} mr_usage;

/*
 * D3D11 resource states that map onto Metal barriers. Kept as the D3D11 names
 * because that is what the caller has, and translating them twice would only
 * add a place for the mapping to be wrong.
 */
typedef enum {
  MR_STAGE_NONE = 0,
  MR_STAGE_VERTEX = 1u << 0,
  MR_STAGE_FRAGMENT = 1u << 1,
  MR_STAGE_COMPUTE = 1u << 2,
  MR_STAGE_TILE = 1u << 3,
  MR_STAGE_BLIT = 1u << 4,
  MR_STAGE_MESH = 1u << 5,
  MR_STAGE_OBJECT = 1u << 6,
} mr_stage_mask;

typedef enum {
  MR_VISIBILITY_NONE = 0,  /* execution ordering only */
  MR_VISIBILITY_DEVICE,    /* the default: write -> read, write -> write */
  MR_VISIBILITY_RESOURCE_ALIAS,
} mr_visibility;

/* ----------------------------------------------------------------- device */

typedef struct {
  mr_backend backend;
  char name[64]; /* "metal4", "metal3" */
  int gpu_family;
  char device_name[128];
  bool unified_memory;
  bool explicit_residency;
  bool explicit_barriers;
  bool argument_tables;
  bool async_pipeline_compilation;
  bool pipeline_archives;      /* MTL4Archive / MTLBinaryArchive */
  bool flexible_pipelines;     /* MTL4 flexible PSOs */
  bool sampler_clamp_to_border;
  bool mesh_shaders;
  bool bc_textures;
  uint32_t max_argument_table_entries;
  uint32_t max_buffers_per_stage;
  uint32_t max_textures_per_stage;
  uint64_t max_buffer_length;
  uint64_t max_threadgroup_memory;
  size_t max_threads_per_threadgroup;
  uint32_t max_residency_sets_per_queue; /* Metal 4 documents 32 */
  uint32_t max_frames_in_flight;
} mr_gfx_caps;

/* --------------------------------------------------------------- resources */

typedef struct {
  uint64_t length;
  mr_usage usage;
  bool shared; /* StorageModeShared vs Private */
  const char *label;
} mr_buffer_desc;

typedef struct {
  mr_format format;
  uint32_t width, height, depth, mip_levels, array_length;
  uint32_t sample_count;
  mr_usage usage;
  bool shared;
  const char *label;
} mr_texture_desc;

typedef struct {
  uint64_t size;
  mr_usage usage; /* the union of what will be placed in the heap */
  const char *label;
} mr_heap_desc;

/* --------------------------------------------------------------- pipelines */

typedef struct {
  mr_gfx_handle library;
  char function[128];
} mr_shader_function;

typedef struct {
  mr_shader_function vertex;
  mr_shader_function fragment;
  bool has_mesh, has_object;
  mr_shader_function mesh, object, fragment_after_mesh;
  mr_format color_formats[8];
  uint32_t color_count;
  mr_format depth_format;
  uint32_t sample_count;
  uint32_t raster_sample_count;
  bool alpha_to_coverage;
  /* Function constants, as (index, value) pairs. Specialising here rather than
   * recompiling lets one pipeline cover many D3D11 permutations. */
  uint32_t function_constants[32];
  uint32_t function_constant_count;
} mr_render_pipeline_desc;

typedef struct {
  mr_shader_function compute;
  uint32_t function_constants[32];
  uint32_t function_constant_count;
  uint32_t threadgroup_size_hint;
} mr_compute_pipeline_desc;

/* An opaque handle to in-flight compilation. Awaiting it is the same operation
 * on both backends, which is what lets async compilation be used unconditionally. */
typedef struct {
  mr_gfx_handle token;
  bool complete;
} mr_compiler_task;

/* ---------------------------------------------------------------- encoding */

typedef struct {
  mr_gfx_handle color[8];
  mr_format color_formats[8];
  mr_gfx_handle depth;
  float clear_color[4];
  bool do_clear_color[8];
  bool do_clear_depth;
  double clear_depth_value;
  uint32_t clear_stencil_value;
  bool store_color[8]; /* false => MTLStoreActionDontCare */
  bool store_depth;
  const char *label;
} mr_render_pass_desc;

typedef struct {
  mr_gfx_handle pipeline;
  uint32_t vertex_count, instance_count;
  uint32_t first_vertex, first_instance;
} mr_draw_args;

typedef struct {
  mr_gfx_handle pipeline;
  uint32_t group_x, group_y, group_z;
  uint32_t threads_x, threads_y, threads_z;
} mr_dispatch_args;

typedef struct {
  uint32_t after_encoder_stages;
  uint32_t before_encoder_stages;
  uint32_t after_queue_stages;
  uint32_t before_queue_stages;
  mr_visibility visibility;
} mr_barrier_desc;

/* Binding index spaces, as DXMT sees them. */
typedef enum {
  MR_BIND_BUFFER = 0,
  MR_BIND_TEXTURE,
  MR_BIND_SAMPLER,
} mr_bind_kind;

typedef struct {
  uint32_t max_buffers;
  uint32_t max_textures;
  uint32_t max_samplers;
  const char *label;
} mr_argument_table_desc;

/* ------------------------------------------------------------------- vtable */

typedef struct mr_gfx_backend mr_gfx_backend;

struct mr_gfx_backend {
  const char *name;
  mr_backend kind;
  void *ctx;

  /* device lifecycle */
  mr_status (*caps)(mr_gfx_backend *self, mr_gfx_caps *out);
  void (*destroy)(mr_gfx_backend *self);

  /* command submission: allocators own command memory, as in Metal 4 */
  mr_status (*allocator_create)(mr_gfx_backend *self, const char *label,
                                mr_gfx_handle *out);
  mr_status (*allocator_reset)(mr_gfx_backend *self, mr_gfx_handle allocator);
  void (*allocator_destroy)(mr_gfx_backend *self, mr_gfx_handle allocator);
  mr_status (*command_buffer_create)(mr_gfx_backend *self, const char *label,
                                     mr_gfx_handle *out);
  void (*command_buffer_destroy)(mr_gfx_backend *self, mr_gfx_handle cmd);
  mr_status (*begin_command_buffer)(mr_gfx_backend *self, mr_gfx_handle cmd,
                                    mr_gfx_handle allocator);
  mr_status (*end_command_buffer)(mr_gfx_backend *self, mr_gfx_handle cmd);
  mr_status (*commit)(mr_gfx_backend *self, const mr_gfx_handle *cmds,
                      size_t count);

  /* encoding */
  mr_status (*render_encoder_begin)(mr_gfx_backend *self, mr_gfx_handle cmd,
                                    const mr_render_pass_desc *desc,
                                    mr_gfx_handle *out_encoder);
  mr_status (*render_encoder_end)(mr_gfx_backend *self, mr_gfx_handle encoder);
  mr_status (*compute_encoder_begin)(mr_gfx_backend *self, mr_gfx_handle cmd,
                                     mr_gfx_handle *out_encoder);
  mr_status (*compute_encoder_end)(mr_gfx_backend *self, mr_gfx_handle encoder);
  mr_status (*draw)(mr_gfx_backend *self, mr_gfx_handle encoder,
                    const mr_draw_args *args);
  mr_status (*dispatch)(mr_gfx_backend *self, mr_gfx_handle encoder,
                        const mr_dispatch_args *args);
  mr_status (*draw_indexed)(mr_gfx_backend *self, mr_gfx_handle encoder,
                            mr_gfx_handle index_buffer, uint32_t index_count,
                            uint32_t index_offset);

  /* binding: argument tables where available, encoder calls where not */
  mr_status (*argument_table_create)(mr_gfx_backend *self,
                                     const mr_argument_table_desc *desc,
                                     mr_gfx_handle *out);
  void (*argument_table_destroy)(mr_gfx_backend *self, mr_gfx_handle table);
  mr_status (*argument_table_set_buffer)(mr_gfx_backend *self,
                                         mr_gfx_handle table, uint32_t index,
                                         mr_gfx_handle buffer,
                                         uint64_t offset);
  mr_status (*argument_table_set_texture)(mr_gfx_backend *self,
                                          mr_gfx_handle table, uint32_t index,
                                          mr_gfx_handle texture);
  mr_status (*argument_table_set_sampler)(mr_gfx_backend *self,
                                          mr_gfx_handle table, uint32_t index,
                                          mr_gfx_handle sampler);
  mr_status (*bind_argument_table)(mr_gfx_backend *self, mr_gfx_handle encoder,
                                   uint32_t stage, mr_gfx_handle table);

  /* residency: explicit in Metal 4, emulated with useResource: in Metal 3 */
  mr_status (*residency_set_create)(mr_gfx_backend *self, const char *label,
                                    mr_gfx_handle *out);
  void (*residency_set_destroy)(mr_gfx_backend *self, mr_gfx_handle set);
  mr_status (*residency_add)(mr_gfx_backend *self, mr_gfx_handle set,
                             mr_gfx_handle allocation);
  mr_status (*residency_remove)(mr_gfx_backend *self, mr_gfx_handle set,
                                mr_gfx_handle allocation);
  mr_status (*residency_commit)(mr_gfx_backend *self, mr_gfx_handle set);
  mr_status (*use_residency_set)(mr_gfx_backend *self, mr_gfx_handle encoder,
                                 mr_gfx_handle set);

  /* synchronisation */
  mr_status (*barrier)(mr_gfx_backend *self, mr_gfx_handle encoder,
                       const mr_barrier_desc *desc);
  mr_status (*fence_create)(mr_gfx_backend *self, mr_gfx_handle *out);
  mr_status (*update_fence)(mr_gfx_backend *self, mr_gfx_handle encoder,
                            mr_gfx_handle fence, uint32_t stages);
  mr_status (*wait_for_fence)(mr_gfx_backend *self, mr_gfx_handle encoder,
                              mr_gfx_handle fence, uint32_t stages);
  mr_status (*shared_event_create)(mr_gfx_backend *self, mr_gfx_handle *out);
  mr_status (*signal_event)(mr_gfx_backend *self, mr_gfx_handle encoder,
                            mr_gfx_handle event, uint64_t value);
  mr_status (*wait_for_event)(mr_gfx_backend *self, mr_gfx_handle encoder,
                              mr_gfx_handle event, uint64_t value);

  /* resources */
  mr_status (*buffer_create)(mr_gfx_backend *self, const mr_buffer_desc *desc,
                             mr_gfx_handle *out);
  mr_status (*texture_create)(mr_gfx_backend *self, const mr_texture_desc *desc,
                              mr_gfx_handle *out);
  mr_status (*heap_create)(mr_gfx_backend *self, const mr_heap_desc *desc,
                           mr_gfx_handle *out);
  void (*resource_destroy)(mr_gfx_backend *self, mr_gfx_handle resource);
  void *(*buffer_contents)(mr_gfx_backend *self, mr_gfx_handle buffer);
  mr_status (*copy_into_texture)(mr_gfx_backend *self, mr_gfx_handle cmd,
                                 const void *bytes, size_t len,
                                 uint32_t bytes_per_row,
                                 uint32_t bytes_per_image,
                                 mr_gfx_handle texture);

  /* pipelines */
  mr_status (*library_load)(mr_gfx_backend *self, const void *metallib,
                            size_t len, mr_gfx_handle *out);
  void (*library_destroy)(mr_gfx_backend *self, mr_gfx_handle library);
  mr_status (*compile_render_pipeline)(mr_gfx_backend *self,
                                       const mr_render_pipeline_desc *desc,
                                       mr_gfx_handle *out_pso);
  mr_status (*compile_compute_pipeline)(mr_gfx_backend *self,
                                        const mr_compute_pipeline_desc *desc,
                                        mr_gfx_handle *out_pso);
  mr_status (*compile_render_pipeline_async)(
      mr_gfx_backend *self, const mr_render_pipeline_desc *desc,
      mr_compiler_task *task);
  mr_status (*await_compiler_task)(mr_gfx_backend *self, mr_compiler_task *task,
                                   mr_gfx_handle *out_pso);
  void (*pipeline_destroy)(mr_gfx_backend *self, mr_gfx_handle pso);

  /* pipeline archives: the pipeline half of the shader cache */
  mr_status (*archive_open)(mr_gfx_backend *self, const char *path,
                            bool for_writing, mr_gfx_handle *out);
  mr_status (*archive_add_pipeline)(mr_gfx_backend *self, mr_gfx_handle archive,
                                    mr_gfx_handle pso);
  mr_status (*archive_serialize)(mr_gfx_backend *self, mr_gfx_handle archive);
  void (*archive_close)(mr_gfx_backend *self, mr_gfx_handle archive);

  /* samplers */
  mr_status (*sampler_create)(mr_gfx_backend *self, const void *state,
                              mr_gfx_handle *out);

  /* presentation */
  mr_status (*present)(mr_gfx_backend *self, mr_gfx_handle texture);
};

/*
 * Creates the best backend this device supports. Prefers Metal 4 and falls
 * back to Metal 3 with `reason` set. Returns NULL when neither is available,
 * which on a supported device means the process is not talking to a GPU at all.
 */
mr_gfx_backend *mr_gfx_backend_create(const mr_host_caps *host,
                                      const char **reason);

/*
 * How many frames the caller should keep in flight, and therefore how many
 * allocators to ring. Derived from the backend rather than hardcoded because
 * Metal 4's allocator reuse rule -- an allocator cannot be reset while the
 * commands it encoded are in flight -- makes a wrong value a correctness bug,
 * not a perf bug.
 */
uint32_t mr_gfx_recommended_frames_in_flight(const mr_gfx_caps *caps);

#ifdef __cplusplus
}
#endif

#endif /* MR_BACKEND_H */
