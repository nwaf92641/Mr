/* Mr — winemetal command stream to Metal 4. See mr_metal4_winemetal.h.
 *
 * Resource handling goes through the layer (mr_mtl4_table_*), so the address
 * and resource-ID translation lives in one place. Encoder commands are issued
 * on the encoder the caller already created, because that is the vocabulary the
 * stream is expressed in.
 */
#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>

#include "mr_metal4.h"
#include "mr_metal4_winemetal.h"
#include "winemetal.h"

/* obj_handle_t is an integer-sized guest handle, not a pointer type, so the
 * bridge to an object has to go through void*: __bridge needs a non-retainable
 * pointer on one side. */
#define MR_WMT_OBJ(handle) (__bridge id)(void *)(uintptr_t)(handle)

/* For the layer's void* object parameters. The handle is already the object's
 * address, so this is an integer-to-pointer cast and needs no ARC bridge -- and
 * taking one would ask the compiler for ownership this code does not want. */
#define MR_WMT_RAW(handle) ((void *)(uintptr_t)(handle))

namespace {

/* D3D11 allows 16 vertex and 16 pixel constant/vertex buffers per stage in the
 * common limits, and 32 when the feature level is 11.0+. The shadow only has to
 * cover the indices the guest can bind. */
constexpr uint32_t kMaxBufferSlots = 32;

struct Stage {
  __unsafe_unretained id buffers[kMaxBufferSlots];
  uint64_t offsets[kMaxBufferSlots];

  Stage() { reset(); }
  void reset() {
    for (uint32_t i = 0; i < kMaxBufferSlots; ++i) {
      buffers[i] = nil;
      offsets[i] = 0;
    }
  }
  bool valid(uint32_t index) const { return index < kMaxBufferSlots; }
};

/* One set of shadows per process, matching the lifetime of the argument tables
 * they mirror: the tables keep their bindings across batches, so a shadow that
 * died with a batch would leave a later offset command with nothing to rebind
 * from. That is exactly the shape DXMT uses -- setObjectBuffer:offset:atIndex:
 * in one batch and setObjectBufferOffset:atIndex: in the next. */
struct Stages {
  Stage vertex;
  Stage fragment;
  Stage object;
  Stage mesh;
};

Stages &wmt_stages() {
  static Stages stages;
  return stages;
}

struct State {
  __unsafe_unretained id<MTL4RenderCommandEncoder> encoder;
  mr_mtl4_table *vertex_table;
  mr_mtl4_table *fragment_table;
  mr_mtl4_table *object_table;
  mr_mtl4_table *mesh_table;
  mr_mtl4_transient *transient;
  mr_mtl4_residency *residency;
  uint32_t untranslated; /* types with no translation, reported once each */
  uint32_t reported[64];
  uint32_t reported_count;
};

/* Metal 4 has no per-stage buffer index space: a table bound to several stages
 * binds every one of its slots at the same index for all of them, so D3D11
 * vertex slot 0 and pixel slot 0 would be the same binding. Each stage therefore
 * gets its own table, bound to its own stage, and an index means what the
 * stage's shader was compiled to expect. */
void rebind(Stage &stage, mr_mtl4_table *table, uint32_t index) {
  if (!stage.valid(index) || stage.buffers[index] == nil || table == nullptr) {
    return;
  }
  id<MTLBuffer> buffer = (id<MTLBuffer>)stage.buffers[index];
  const uint64_t address = (uint64_t)[buffer gpuAddress] + stage.offsets[index];
  mr_mtl4_table_set_buffer(table, address, index);
}

void note_untranslated(State &state, uint32_t type) {
  for (uint32_t i = 0; i < state.reported_count; ++i) {
    if (state.reported[i] == type) {
      return;
    }
  }
  state.untranslated += 1;
  if (state.reported_count < 64) {
    state.reported[state.reported_count++] = type;
  }
  /* Named, once per type, never silently dropped: a missing translation removes
   * state from the frame, and a silent removal reads as a rendering bug. */
  fprintf(stderr, "[mr-mtl4] winemetal command type %u is not translated yet (%u so far)\n",
          type, state.untranslated);
}

bool encode_one(State &state, const struct wmtcmd_base *command) {
  switch (command->type) {
    case WMTRenderCommandSetVertexBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!wmt_stages().vertex.valid(cmd->index)) {
        break;
      }
      wmt_stages().vertex.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      wmt_stages().vertex.offsets[cmd->index] = cmd->offset;
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->buffer));
      rebind(wmt_stages().vertex, state.vertex_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!wmt_stages().vertex.valid(cmd->index)) {
        break;
      }
      wmt_stages().vertex.offsets[cmd->index] = cmd->offset;
      rebind(wmt_stages().vertex, state.vertex_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!wmt_stages().fragment.valid(cmd->index)) {
        break;
      }
      wmt_stages().fragment.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      wmt_stages().fragment.offsets[cmd->index] = cmd->offset;
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->buffer));
      rebind(wmt_stages().fragment, state.fragment_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!wmt_stages().fragment.valid(cmd->index)) {
        break;
      }
      wmt_stages().fragment.offsets[cmd->index] = cmd->offset;
      rebind(wmt_stages().fragment, state.fragment_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const auto *cmd = (const struct wmtcmd_render_settexture *)command;
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->texture));
      mr_mtl4_table_set_texture(state.fragment_table, MR_WMT_RAW(cmd->texture), cmd->index);
      return true;
    }
    case WMTRenderCommandSetFragmentBytes: {
      /* setFragmentBytes: is gone in Metal 4; the bytes go into the transient
       * buffer and the binding becomes an address, like any other buffer. */
      const auto *cmd = (const struct wmtcmd_render_setbytes *)command;
      if (state.transient == nullptr) {
        fprintf(stderr, "[mr-mtl4] setFragmentBytes with no transient buffer -- dropped\n");
        return false;
      }
      const uint64_t address =
          mr_mtl4_transient_write(state.transient, cmd->bytes.ptr, cmd->length);
      if (address == 0) {
        return false;
      }
      mr_mtl4_table_set_buffer(state.fragment_table, address, cmd->index);
      return true;
    }
    case WMTRenderCommandSetPSO: {
      const auto *cmd = (const struct wmtcmd_render_setpso *)command;
      id<MTLRenderPipelineState> pipeline = MR_WMT_OBJ(cmd->pso);
      if (pipeline == nil) {
        break;
      }
      /* A Metal 3 pipeline state is legal on a Metal 4 encoder, which is why
       * pipeline creation can stay where DXMT has it while the encoders move. */
      [state.encoder setRenderPipelineState:pipeline];
      return true;
    }
    case WMTRenderCommandNop:
      return true;
    case WMTRenderCommandSetViewport: {
      const auto *cmd = (const struct wmtcmd_render_setviewport *)command;
      MTLViewport viewport = {cmd->viewport.originX, cmd->viewport.originY, cmd->viewport.width,
                              cmd->viewport.height, cmd->viewport.znear, cmd->viewport.zfar};
      [state.encoder setViewports:&viewport count:1];
      return true;
    }
    case WMTRenderCommandSetViewports: {
      const auto *cmd = (const struct wmtcmd_render_setviewports *)command;
      const struct WMTViewport *source = (const struct WMTViewport *)cmd->viewports.ptr;
      if (source == nullptr || cmd->viewport_count == 0) {
        break;
      }
      /* Metal caps the simultaneous viewport count per family and the guest
       * can ask for more than the device takes; passing the whole array through
       * would make Metal raise, so the count is clamped and the clamp is
       * reported -- a silently dropped viewport changes which target a draw
       * lands in. */
      MTLViewport viewports[16];
      uint32_t count = cmd->viewport_count;
      if (count > 16) {
        fprintf(stderr, "[mr-mtl4] %u viewports requested, 16 encoded\n", count);
        count = 16;
      }
      for (uint32_t i = 0; i < count; ++i) {
        viewports[i] = (MTLViewport){source[i].originX, source[i].originY, source[i].width,
                                     source[i].height, source[i].znear, source[i].zfar};
      }
      [state.encoder setViewports:viewports count:(NSUInteger)count];
      return true;
    }
    case WMTRenderCommandSetScissorRect: {
      const auto *cmd = (const struct wmtcmd_render_setscissorrect *)command;
      MTLScissorRect rect = {(NSUInteger)cmd->scissor_rect.x, (NSUInteger)cmd->scissor_rect.y,
                             (NSUInteger)cmd->scissor_rect.width,
                             (NSUInteger)cmd->scissor_rect.height};
      [state.encoder setScissorRects:&rect count:1];
      return true;
    }
    case WMTRenderCommandSetScissorRects: {
      const auto *cmd = (const struct wmtcmd_render_setscissorrects *)command;
      const struct WMTScissorRect *source = (const struct WMTScissorRect *)cmd->scissor_rects.ptr;
      if (source == nullptr || cmd->rect_count == 0) {
        break;
      }
      MTLScissorRect rects[16];
      uint32_t count = cmd->rect_count;
      if (count > 16) {
        fprintf(stderr, "[mr-mtl4] %u scissor rects requested, 16 encoded\n", count);
        count = 16;
      }
      for (uint32_t i = 0; i < count; ++i) {
        rects[i] = (MTLScissorRect){(NSUInteger)source[i].x, (NSUInteger)source[i].y,
                                    (NSUInteger)source[i].width, (NSUInteger)source[i].height};
      }
      [state.encoder setScissorRects:rects count:(NSUInteger)count];
      return true;
    }
    case WMTRenderCommandSetDSSO: {
      const auto *cmd = (const struct wmtcmd_render_setdsso *)command;
      id<MTLDepthStencilState> state_object = MR_WMT_OBJ(cmd->dsso);
      if (state_object == nil) {
        break;
      }
      [state.encoder setDepthStencilState:state_object];
      [state.encoder setStencilReferenceValue:cmd->stencil_ref];
      return true;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      const auto *cmd = (const struct wmtcmd_render_setblendcolor *)command;
      [state.encoder setBlendColorRed:cmd->red green:cmd->green blue:cmd->blue alpha:cmd->alpha];
      [state.encoder setStencilReferenceValue:cmd->stencil_ref];
      return true;
    }
    case WMTRenderCommandSetRasterizerState: {
      const auto *cmd = (const struct wmtcmd_render_setrasterizerstate *)command;
      /* The WMT enums are Metal's own order, so these are the same values and
       * not a translation table. Fill mode and culling live on the encoder in
       * Metal 4 as they did in Metal 3; only the pipeline state's own
       * rasterization fields are metal-3 concept. */
      [state.encoder setTriangleFillMode:(MTLTriangleFillMode)cmd->fill_mode];
      [state.encoder setCullMode:(MTLCullMode)cmd->cull_mode];
      [state.encoder setFrontFacingWinding:(MTLWinding)cmd->winding];
      [state.encoder setDepthClipMode:(MTLDepthClipMode)cmd->depth_clip_mode];
      [state.encoder setDepthBias:cmd->depth_bias
                       slopeScale:cmd->scole_scale
                            clamp:cmd->depth_bias_clamp];
      return true;
    }
    case WMTRenderCommandSetVisibilityMode: {
      const auto *cmd = (const struct wmtcmd_render_setvisibilitymode *)command;
      [state.encoder setVisibilityResultMode:(MTLVisibilityResultMode)cmd->mode
                                      offset:(NSUInteger)cmd->offset];
      return true;
    }
    case WMTRenderCommandDrawIndirect: {
      const auto *cmd = (const struct wmtcmd_render_draw_indirect *)command;
      id<MTLBuffer> arguments = MR_WMT_OBJ(cmd->indirect_args_buffer);
      if (arguments == nil) {
        break;
      }
      /* Metal 4 takes the indirect arguments as an address, like every other
       * buffer, so the offset folds into the address rather than being a
       * separate argument. */
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->indirect_args_buffer));
      [state.encoder drawPrimitives:(MTLPrimitiveType)cmd->primitive_type
                     indirectBuffer:(MTLGPUAddress)((uint64_t)[arguments gpuAddress] +
                                                    cmd->indirect_args_offset)];
      return true;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      const auto *cmd = (const struct wmtcmd_render_draw_indexed_indirect *)command;
      id<MTLBuffer> index_buffer = MR_WMT_OBJ(cmd->index_buffer);
      id<MTLBuffer> arguments = MR_WMT_OBJ(cmd->indirect_args_buffer);
      if (index_buffer == nil || arguments == nil) {
        break;
      }
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->index_buffer));
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->indirect_args_buffer));
      const uint64_t index_address =
          (uint64_t)[index_buffer gpuAddress] + cmd->index_buffer_offset;
      const uint64_t index_length =
          (uint64_t)[index_buffer length] - cmd->index_buffer_offset;
      [state.encoder
          drawIndexedPrimitives:(MTLPrimitiveType)cmd->primitive_type
                     indexType:(MTLIndexType)cmd->index_type
                   indexBuffer:(MTLGPUAddress)index_address
             indexBufferLength:(NSUInteger)index_length
                indirectBuffer:(MTLGPUAddress)((uint64_t)[arguments gpuAddress] +
                                               cmd->indirect_args_offset)];
      return true;
    }
    case WMTRenderCommandUpdateFence: {
      const auto *cmd = (const struct wmtcmd_render_fence_op *)command;
      id<MTLFence> fence = MR_WMT_OBJ(cmd->fence);
      if (fence == nil) {
        break;
      }
      /* Metal 4 kept MTLFence and put the stage set on the selector:
       * updateFence:afterEncoderStages:. The DXMT struct carries one stage set,
       * which is what both directions need. */
      [(id<MTL4CommandEncoder>)state.encoder updateFence:fence
                                      afterEncoderStages:(MTLStages)cmd->stages];
      return true;
    }
    case WMTRenderCommandWaitForFence: {
      const auto *cmd = (const struct wmtcmd_render_fence_op *)command;
      id<MTLFence> fence = MR_WMT_OBJ(cmd->fence);
      if (fence == nil) {
        break;
      }
      [(id<MTL4CommandEncoder>)state.encoder waitForFence:fence
                                      beforeEncoderStages:(MTLStages)cmd->stages];
      return true;
    }
    case WMTRenderCommandMemoryBarrier: {
      const auto *cmd = (const struct wmtcmd_render_memory_barrier *)command;
      /* D3D11 has one barrier notion; Metal 4 splits it into an encoder-local
       * side and a queue-visible side, and the two-barrier form is the one that
       * covers both: barrierAfterStages: orders work already encoded in this
       * encoder, beforeQueueStages: orders what comes after it is submitted.
       * The DXMT struct's stages_before and stages_after map onto those two
       * sides. This orders at least as much as D3D11 asked for and never less,
       * which is the direction to be wrong in: ordering more costs a stall,
       * ordering less is a race.
       *
       * visibilityOptions is passed as 0 rather than by naming an enumerator
       * that has not been read out of the SDK. vm.scope is not used: the
       * encoder-local barrier form carries no scope, and inventing one would
       * claim a guarantee Metal has not been asked for. */
      [(id<MTL4CommandEncoder>)state.encoder
          barrierAfterStages:(MTLStages)cmd->stages_before
           beforeQueueStages:(MTLStages)cmd->stages_after
          visibilityOptions:(MTL4VisibilityOptions)0];
      return true;
    }
    case WMTRenderCommandUseResource: {
      const auto *cmd = (const struct wmtcmd_render_useresource *)command;
      /* Metal 4 removed useResource:usage:stages: and governs access through
       * residency instead, so the resource is added to the set rather than a
       * call being invented. The usage scope has no Metal 4 counterpart: being
       * resident without declaring read/write is the permissive direction, and
       * permissive is the side to err on -- it allows an access rather than
       * denying one that the guest clearly intends.
       *
       * This matters for resources reached through a binding this walk never
       * sees, which is the case useResource exists for. */
      if (cmd->resource == 0) {
        break;
      }
      if (!mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->resource))) {
        break;
      }
      return true;
    }
    case WMTRenderCommandSetObjectBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!wmt_stages().object.valid(cmd->index)) {
        break;
      }
      wmt_stages().object.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      wmt_stages().object.offsets[cmd->index] = cmd->offset;
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->buffer));
      rebind(wmt_stages().object, state.object_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!wmt_stages().object.valid(cmd->index) ||
          wmt_stages().object.buffers[cmd->index] == nil) {
        /* Without the base from an earlier SetObjectBuffer the address this
         * should produce is unknown; rebinding nothing would leave the slot
         * pointing wherever it last did. */
        break;
      }
      wmt_stages().object.offsets[cmd->index] = cmd->offset;
      rebind(wmt_stages().object, state.object_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetMeshBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!wmt_stages().mesh.valid(cmd->index)) {
        break;
      }
      wmt_stages().mesh.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      wmt_stages().mesh.offsets[cmd->index] = cmd->offset;
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->buffer));
      rebind(wmt_stages().mesh, state.mesh_table, cmd->index);
      return true;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!wmt_stages().mesh.valid(cmd->index) || wmt_stages().mesh.buffers[cmd->index] == nil) {
        break;
      }
      wmt_stages().mesh.offsets[cmd->index] = cmd->offset;
      rebind(wmt_stages().mesh, state.mesh_table, cmd->index);
      return true;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      const auto *cmd = (const struct wmtcmd_render_draw_meshthreadgroups *)command;
      /* WMTSize's members are width, height and depth -- read from DXMT's own
       * accessors, not assumed. */
      [state.encoder
          drawMeshThreadgroups:MTLSizeMake(cmd->threadgroup_per_grid.width,
                                           cmd->threadgroup_per_grid.height,
                                           cmd->threadgroup_per_grid.depth)
              threadsPerObjectThreadgroup:MTLSizeMake(cmd->object_threadgroup_size.width,
                                                      cmd->object_threadgroup_size.height,
                                                      cmd->object_threadgroup_size.depth)
                threadsPerMeshThreadgroup:MTLSizeMake(cmd->mesh_threadgroup_size.width,
                                                      cmd->mesh_threadgroup_size.height,
                                                      cmd->mesh_threadgroup_size.depth)];
      return true;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      const auto *cmd = (const struct wmtcmd_render_draw_meshthreadgroups_indirect *)command;
      id<MTLBuffer> arguments = MR_WMT_OBJ(cmd->indirect_args_buffer);
      if (arguments == nil) {
        break;
      }
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->indirect_args_buffer));
      [state.encoder
          drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)((uint64_t)[arguments gpuAddress] +
                                                                 cmd->indirect_args_offset)
                        threadsPerObjectThreadgroup:MTLSizeMake(cmd->object_threadgroup_size.width,
                                                                cmd->object_threadgroup_size.height,
                                                                cmd->object_threadgroup_size.depth)
                          threadsPerMeshThreadgroup:MTLSizeMake(cmd->mesh_threadgroup_size.width,
                                                                cmd->mesh_threadgroup_size.height,
                                                                cmd->mesh_threadgroup_size.depth)];
      return true;
    }
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      /* DXMT's own extension opcodes, emulating D3D11 geometry and tessellation
       * stages as mesh draws. The mapping is the one winemetal_unix.c applies on
       * the Metal 3 path, kept 1:1: object buffer 20 carries the index buffer
       * for the indexed forms, object buffer 21 carries the draw arguments, and
       * the tessellation forms hard-code a mesh threadgroup of (32,1,1) rather
       * than reading one from the command. */
      mr_mtl4_table *table = state.object_table;
      auto bind_object = [&](obj_handle_t buffer, uint64_t offset, uint32_t index) {
        if (buffer == 0) {
          return false;
        }
        if (!wmt_stages().object.valid(index)) {
          return false;
        }
        wmt_stages().object.buffers[index] = MR_WMT_OBJ(buffer);
        wmt_stages().object.offsets[index] = offset;
        mr_mtl4_residency_add(state.residency, MR_WMT_RAW(buffer));
        rebind(wmt_stages().object, table, index);
        return true;
      };
      /* An offset command with no base to rebind from cannot be translated: the
       * address it should produce is unknown, so it is reported instead of
       * binding nothing and leaving a stale address in the table. */
      auto offset_object = [&](uint64_t offset, uint32_t index) {
        if (!wmt_stages().object.valid(index) || wmt_stages().object.buffers[index] == nil) {
          return false;
        }
        wmt_stages().object.offsets[index] = offset;
        rebind(wmt_stages().object, table, index);
        return true;
      };
      constexpr uint32_t kObjectIndexBufferSlot = 20;
      constexpr uint32_t kObjectArgumentsSlot = 21;
      MTLSize mesh_group = MTLSizeMake(1, 1, 1);

      switch (command->type) {
        case WMTRenderCommandDXMTGeometryDraw: {
          const auto *cmd = (const struct wmtcmd_render_dxmt_geometry_draw *)command;
          if (!offset_object(cmd->draw_arguments_offset, kObjectArgumentsSlot)) {
            break;
          }
          [state.encoder drawMeshThreadgroups:MTLSizeMake(cmd->warp_count, cmd->instance_count, 1)
                  threadsPerObjectThreadgroup:MTLSizeMake(cmd->vertex_per_warp, 1, 1)
                    threadsPerMeshThreadgroup:mesh_group];
          return true;
        }
        case WMTRenderCommandDXMTGeometryDrawIndexed: {
          const auto *cmd = (const struct wmtcmd_render_dxmt_geometry_draw_indexed *)command;
          if (!bind_object(cmd->index_buffer, cmd->index_buffer_offset, kObjectIndexBufferSlot) ||
              !offset_object(cmd->draw_arguments_offset, kObjectArgumentsSlot)) {
            break;
          }
          [state.encoder drawMeshThreadgroups:MTLSizeMake(cmd->warp_count, cmd->instance_count, 1)
                  threadsPerObjectThreadgroup:MTLSizeMake(cmd->vertex_per_warp, 1, 1)
                    threadsPerMeshThreadgroup:mesh_group];
          return true;
        }
        case WMTRenderCommandDXMTGeometryDrawIndirect: {
          const auto *cmd = (const struct wmtcmd_render_dxmt_geometry_draw_indirect *)command;
          id<MTLBuffer> dispatch = MR_WMT_OBJ(cmd->dispatch_args_buffer);
          if (dispatch == nil ||
              !bind_object(cmd->indirect_args_buffer, cmd->indirect_args_offset,
                           kObjectArgumentsSlot)) {
            break;
          }
          mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->dispatch_args_buffer));
          [state.encoder drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)((uint64_t)[dispatch gpuAddress] + cmd->dispatch_args_offset)
                                       threadsPerObjectThreadgroup:MTLSizeMake(cmd->vertex_per_warp,
                                                                               1, 1)
                                         threadsPerMeshThreadgroup:mesh_group];
          if (!bind_object(cmd->imm_draw_arguments, 0, kObjectArgumentsSlot)) {
            break;
          }
          return true;
        }
        case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
          const auto *cmd =
              (const struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)command;
          id<MTLBuffer> dispatch = MR_WMT_OBJ(cmd->dispatch_args_buffer);
          if (dispatch == nil ||
              !bind_object(cmd->index_buffer, cmd->index_buffer_offset, kObjectIndexBufferSlot) ||
              !bind_object(cmd->indirect_args_buffer, cmd->indirect_args_offset,
                           kObjectArgumentsSlot)) {
            break;
          }
          mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->dispatch_args_buffer));
          [state.encoder drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)((uint64_t)[dispatch gpuAddress] + cmd->dispatch_args_offset)
                                       threadsPerObjectThreadgroup:MTLSizeMake(cmd->vertex_per_warp,
                                                                               1, 1)
                                         threadsPerMeshThreadgroup:mesh_group];
          if (!bind_object(cmd->imm_draw_arguments, 0, kObjectArgumentsSlot)) {
            break;
          }
          return true;
        }
        case WMTRenderCommandDXMTTessellationMeshDraw: {
          const auto *cmd = (const struct wmtcmd_render_dxmt_tessellation_mesh_draw *)command;
          if (!offset_object(cmd->draw_arguments_offset, kObjectArgumentsSlot)) {
            break;
          }
          [state.encoder
              drawMeshThreadgroups:MTLSizeMake(cmd->patch_per_mesh_instance, cmd->instance_count, 1)
                  threadsPerObjectThreadgroup:MTLSizeMake(cmd->threads_per_patch,
                                                          cmd->patch_per_group, 1)
                    threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
          return true;
        }
        case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
          const auto *cmd =
              (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)command;
          if (!bind_object(cmd->index_buffer, cmd->index_buffer_offset, kObjectIndexBufferSlot) ||
              !offset_object(cmd->draw_arguments_offset, kObjectArgumentsSlot)) {
            break;
          }
          [state.encoder
              drawMeshThreadgroups:MTLSizeMake(cmd->patch_per_mesh_instance, cmd->instance_count, 1)
                  threadsPerObjectThreadgroup:MTLSizeMake(cmd->threads_per_patch,
                                                          cmd->patch_per_group, 1)
                    threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
          return true;
        }
        case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
          const auto *cmd =
              (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)command;
          id<MTLBuffer> dispatch = MR_WMT_OBJ(cmd->dispatch_args_buffer);
          if (dispatch == nil ||
              !bind_object(cmd->indirect_args_buffer, cmd->indirect_args_offset,
                           kObjectArgumentsSlot)) {
            break;
          }
          mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->dispatch_args_buffer));
          [state.encoder
              drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)((uint64_t)[dispatch gpuAddress] + cmd->dispatch_args_offset)
                           threadsPerObjectThreadgroup:MTLSizeMake(cmd->threads_per_patch,
                                                                   cmd->patch_per_group, 1)
                             threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
          if (!bind_object(cmd->imm_draw_arguments, 0, kObjectArgumentsSlot)) {
            break;
          }
          return true;
        }
        case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
          const auto *cmd =
              (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)command;
          id<MTLBuffer> dispatch = MR_WMT_OBJ(cmd->dispatch_args_buffer);
          if (dispatch == nil ||
              !bind_object(cmd->index_buffer, cmd->index_buffer_offset, kObjectIndexBufferSlot) ||
              !bind_object(cmd->indirect_args_buffer, cmd->indirect_args_offset,
                           kObjectArgumentsSlot)) {
            break;
          }
          mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->dispatch_args_buffer));
          [state.encoder
              drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)((uint64_t)[dispatch gpuAddress] + cmd->dispatch_args_offset)
                           threadsPerObjectThreadgroup:MTLSizeMake(cmd->threads_per_patch,
                                                                   cmd->patch_per_group, 1)
                             threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
          if (!bind_object(cmd->imm_draw_arguments, 0, kObjectArgumentsSlot)) {
            break;
          }
          return true;
        }
        default:
          break;
      }
      /* Falling out means a bind could not be translated; the outer switch's
       * default arm counts and names the type rather than the frame going out
       * with a geometry draw that silently did nothing. */
      break;
    }
    case WMTRenderCommandDraw: {
      const auto *cmd = (const struct wmtcmd_render_draw *)command;
      [state.encoder drawPrimitives:(MTLPrimitiveType)cmd->primitive_type
                        vertexStart:(NSUInteger)cmd->vertex_start
                        vertexCount:(NSUInteger)cmd->vertex_count
                      instanceCount:(NSUInteger)cmd->instance_count
                       baseInstance:(NSUInteger)cmd->base_instance];
      return true;
    }
    case WMTRenderCommandDrawIndexed: {
      const auto *cmd = (const struct wmtcmd_render_draw_indexed *)command;
      id<MTLBuffer> index_buffer = MR_WMT_OBJ(cmd->index_buffer);
      if (index_buffer == nil) {
        break;
      }
      mr_mtl4_residency_add(state.residency, MR_WMT_RAW(cmd->index_buffer));
      const uint64_t address = (uint64_t)[index_buffer gpuAddress] + cmd->index_buffer_offset;
      /* Metal 4 bounds-checks index values against this length rather than
       * trusting indexCount, so it is the whole range reachable from the
       * address, not index_count * index_size -- a short length clamps silently
       * and the geometry comes out wrong instead of erroring. */
      const uint64_t length = (uint64_t)[index_buffer length] - cmd->index_buffer_offset;
      [state.encoder drawIndexedPrimitives:(MTLPrimitiveType)cmd->primitive_type
                                indexCount:(NSUInteger)cmd->index_count
                                 indexType:(MTLIndexType)cmd->index_type
                               indexBuffer:(MTLGPUAddress)address
                         indexBufferLength:(NSUInteger)length
                             instanceCount:(NSUInteger)cmd->instance_count
                                baseVertex:(NSInteger)cmd->base_vertex
                              baseInstance:(NSUInteger)cmd->base_instance];
      return true;
    }
    default:
      break;
  }
  note_untranslated(state, command->type);
  return false;
}

/* Exposed so a test can assert what a frame contained instead of inferring it
 * from the absence of warnings. One encode at a time, like DXMT's own walker. */
struct Journal {
  uint32_t translated[64];
  uint32_t translated_count;
  uint32_t untranslated[64];
  uint32_t untranslated_count;
};
Journal &journal() {
  static _Thread_local Journal instance;
  return instance;
}

} /* namespace */

uint32_t mr_mtl4_wmt_encode_render(void *mtl4_render_encoder, mr_mtl4_table *vertex_table,
                                   mr_mtl4_table *fragment_table, mr_mtl4_table *object_table,
                                   mr_mtl4_table *mesh_table, void *mtl4_transient,
                                   mr_mtl4_residency *residency, const void *cmd_head) {
  if (cmd_head == nullptr) {
    return 0;
  }
  if (vertex_table == nullptr || fragment_table == nullptr || object_table == nullptr ||
      mesh_table == nullptr) {
    fprintf(stderr, "[mr-mtl4] encode_render with no argument table -- nothing encoded\n");
    return 0;
  }

  State state;
  state.encoder = (__bridge id<MTL4RenderCommandEncoder>)mtl4_render_encoder;
  state.vertex_table = vertex_table;
  state.fragment_table = fragment_table;
  state.object_table = object_table;
  state.mesh_table = mesh_table;
  state.transient = (mr_mtl4_transient *)mtl4_transient;
  state.residency = residency;

  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (state.encoder != nil) {
      /* One table, both stages: the stream interleaves vertex and fragment
       * bindings into the same index spaces the shaders were compiled against,
       * so splitting it per stage would change what an index means. */
      // One table per stage, each bound only to its own: see rebind above for why a
      // shared table would alias vertex slot 0 onto pixel slot 0.
      [(id<MTL4RenderCommandEncoder>)state.encoder
          setArgumentTable:(__bridge id<MTL4ArgumentTable>)mr_mtl4_table_metal_table(vertex_table)
                  atStages:MTLRenderStageVertex];
      [(id<MTL4RenderCommandEncoder>)state.encoder
          setArgumentTable:(__bridge id<MTL4ArgumentTable>)mr_mtl4_table_metal_table(fragment_table)
                  atStages:MTLRenderStageFragment];
      /* The geometry and tessellation pipelines read their arguments from
       * object buffer slots 20 and 21, and 21 is also a legal vertex buffer
       * index in D3D11 -- so the object stage needs its own table or a geometry
       * draw would overwrite a constant buffer the vertex stage is using. */
      [(id<MTL4RenderCommandEncoder>)state.encoder
          setArgumentTable:(__bridge id<MTL4ArgumentTable>)mr_mtl4_table_metal_table(object_table)
                  atStages:MTLRenderStageObject];
      [(id<MTL4RenderCommandEncoder>)state.encoder
          setArgumentTable:(__bridge id<MTL4ArgumentTable>)mr_mtl4_table_metal_table(mesh_table)
                  atStages:MTLRenderStageMesh];
    }
  } else {
    fprintf(stderr, "[mr-mtl4] encode_render on a system without Metal 4 -- nothing encoded\n");
    return 0;
  }

  uint32_t translated = 0;
  Journal &log = journal();
  for (const struct wmtcmd_base *command = (const struct wmtcmd_base *)cmd_head; command != nullptr;
       command = (const struct wmtcmd_base *)command->next.ptr) {
    if (encode_one(state, command)) {
      translated += 1;
      if (log.translated_count < 64) {
        log.translated[log.translated_count++] = command->type;
      }
    }
  }
  if (mr_mtl4_residency_dirty(state.residency)) {
    mr_mtl4_residency_commit(state.residency);
  }
  log.untranslated_count = state.reported_count;
  for (uint32_t i = 0; i < state.reported_count && i < 64; ++i) {
    log.untranslated[i] = state.reported[i];
  }
  return translated;
}

uint32_t mr_mtl4_wmt_translated_count(void) { return journal().translated_count; }

uint32_t mr_mtl4_wmt_translated_type(uint32_t position) {
  Journal &log = journal();
  return position < log.translated_count ? log.translated[position] : 0;
}

uint32_t mr_mtl4_wmt_untranslated_count(void) { return journal().untranslated_count; }

uint32_t mr_mtl4_wmt_untranslated_type(uint32_t position) {
  Journal &log = journal();
  return position < log.untranslated_count ? log.untranslated[position] : 0;
}

/* --------------------------------------------------------------- session --- */

namespace {

/* Tagged handles. A real arm64 user-space pointer is below 2^48, so the top 16
 * bits identify a token without a lookup and a stray Metal object can never be
 * mistaken for one. */
constexpr uint64_t kTokenTag = 0x4D52000000000000ULL; /* 'MR' */
constexpr uint32_t kKindFrame = 1;
constexpr uint32_t kKindEncoder = 2;
constexpr uint32_t kMaxFrames = 8;
constexpr uint32_t kMaxEncoders = 8;

bool is_token(uint64_t handle) { return (handle & 0xFFFF000000000000ULL) == kTokenTag; }
uint32_t token_kind(uint64_t handle) { return (uint32_t)((handle >> 20) & 0xFu); }
uint32_t token_index(uint64_t handle) { return (uint32_t)(handle & 0xFFu); }
uint64_t make_token(uint32_t kind, uint32_t index) {
  return kTokenTag | ((uint64_t)kind << 20) | (uint64_t)index;
}

struct Session {
  bool started;
  bool failed;
  mr_mtl4_device *device;
  mr_mtl4_table *vertex_table;
  mr_mtl4_table *fragment_table;
  mr_mtl4_table *object_table;
  mr_mtl4_table *mesh_table;
  mr_mtl4_transient *transient;
  mr_mtl4_residency *residency;
  mr_mtl4_frame *frames[kMaxFrames];
  __unsafe_unretained id encoder_object[kMaxEncoders];
  bool encoder_live[kMaxEncoders];
  uint32_t next_frame;
  uint32_t live_frames;
};

Session &session() {
  static Session instance = {};
  return instance;
}

/* Waiting and releasing an unread frame is what stops a guest that never
 * presents or waits from running the process out of allocators; Metal 4 permits
 * one command buffer per allocator at a time, so the slot cannot be reused
 * without this. */
void retire(mr_mtl4_frame *frame) {
  if (frame == nullptr) {
    return;
  }
  mr_mtl4_frame_wait(frame, 5000);
  mr_mtl4_frame_destroy(frame);
}

Session &start_session() {
  Session &state = session();
  if (state.started) {
    return state;
  }
  state.started = true;
  state.device = mr_mtl4_device_create();
  if (state.device == nullptr) {
    state.failed = true;
    return state;
  }
  /* One table per stage, sized to D3D11's per-stage limits. A single table bound
   * to both stages would alias vertex slot 0 onto pixel slot 0. */
  state.vertex_table =
      mr_mtl4_device_new_table(state.device, 32, 8, 0, MR_MTL4_STAGE_VERTEX, true);
  state.fragment_table =
      mr_mtl4_device_new_table(state.device, 32, 128, 16, MR_MTL4_STAGE_FRAGMENT, false);
  state.object_table =
      mr_mtl4_device_new_table(state.device, 32, 0, 0, MR_MTL4_STAGE_OBJECT, true);
  state.mesh_table =
      mr_mtl4_device_new_table(state.device, 32, 0, 0, MR_MTL4_STAGE_MESH, true);
  state.transient = mr_mtl4_transient_create(mr_mtl4_device_metal_device(state.device), 4u << 20);
  /* Attached to the queue, so every frame's draws see it. The capacity has to
   * cover every distinct resource a frame touches, not every binding. */
  state.residency = mr_mtl4_residency_create(state.device, 4096);
  if (state.residency != nullptr &&
      !mr_mtl4_device_add_residency_set(state.device,
                                        (void *)mr_mtl4_residency_metal_set(state.residency))) {
    fprintf(stderr, "[mr-mtl4] residency attach failed: %s\n", mr_mtl4_last_error());
    state.failed = true;
  }
  if (state.vertex_table == nullptr || state.fragment_table == nullptr ||
      state.object_table == nullptr || state.mesh_table == nullptr ||
      state.transient == nullptr) {
    fprintf(stderr, "[mr-mtl4] session start failed: %s\n", mr_mtl4_last_error());
    state.failed = true;
  }
  return state;
}

mr_mtl4_frame *frame_for(uint64_t token) {
  Session &state = session();
  if (!is_token(token) || token_kind(token) != kKindFrame) {
    return nullptr;
  }
  const uint32_t slot = token_index(token);
  return slot < kMaxFrames ? state.frames[slot] : nullptr;
}

} /* namespace */

bool mr_mtl4_wmt_session_available(void) {
  Session &state = start_session();
  return !state.failed && state.device != nullptr;
}

uint64_t mr_mtl4_wmt_session_command_buffer(uint64_t mtl3_command_buffer) {
  (void)mtl3_command_buffer; /* the Metal 3 buffer is never used on this path */
  Session &state = start_session();
  if (state.failed) {
    return 0;
  }
  const uint32_t slot = state.next_frame;
  state.next_frame = (state.next_frame + 1) % kMaxFrames;
  if (state.frames[slot] != nullptr) {
    retire(state.frames[slot]);
    state.frames[slot] = nullptr;
    state.live_frames -= 1;
  }
  mr_mtl4_frame *frame = mr_mtl4_device_new_frame(state.device, "wmt");
  if (frame == nullptr) {
    fprintf(stderr, "[mr-mtl4] no command buffer: %s\n", mr_mtl4_last_error());
    return 0;
  }
  state.frames[slot] = frame;
  state.live_frames += 1;
  return make_token(kKindFrame, slot);
}

bool mr_mtl4_wmt_session_render_encoder(uint64_t command_buffer, const void *render_pass_info,
                                        uint64_t *out_encoder) {
  Session &state = start_session();
  mr_mtl4_frame *frame = frame_for(command_buffer);
  const auto *info = (const struct WMTRenderPassInfo *)render_pass_info;
  if (state.failed || frame == nullptr || info == nullptr || out_encoder == nullptr) {
    return false;
  }
  if (!mr_mtl4_frame_begin(frame)) {
    fprintf(stderr, "[mr-mtl4] frame begin failed: %s\n", mr_mtl4_last_error());
    return false;
  }
  void *pass = mr_mtl4_render_pass_create();
  if (pass == nullptr) {
    return false;
  }
  mr_mtl4_render_pass_set_size(pass, info->render_target_width, info->render_target_height);
  for (uint32_t i = 0; i < 8; ++i) {
    /* A slot with no texture is an unused attachment, not a cleared one. */
    if (info->colors[i].texture == 0) {
      continue;
    }
    const struct WMTColorAttachmentInfo &color = info->colors[i];
    mr_mtl4_render_pass_set_color(pass, i, MR_WMT_RAW(color.texture),
                                  (mr_mtl4_load_action)color.load_action,
                                  (mr_mtl4_store_action)color.store_action, color.clear_color.r,
                                  color.clear_color.g, color.clear_color.b, color.clear_color.a);
    mr_mtl4_render_pass_set_color_plane(pass, i, color.level, color.slice, color.depth_plane);
    if (color.resolve_texture != 0) {
      mr_mtl4_render_pass_set_color_resolve(pass, i, MR_WMT_RAW(color.resolve_texture));
      mr_mtl4_render_pass_set_color_resolve_plane(pass, i, color.resolve_level, color.resolve_slice,
                                                  color.resolve_depth_plane);
    }
  }
  if (info->depth.texture != 0) {
    mr_mtl4_render_pass_set_depth(pass, MR_WMT_RAW(info->depth.texture),
                                  (mr_mtl4_load_action)info->depth.load_action,
                                  (mr_mtl4_store_action)info->depth.store_action,
                                  info->depth.clear_depth);
    mr_mtl4_render_pass_set_depth_plane(pass, info->depth.level, info->depth.slice,
                                        info->depth.depth_plane);
  }
  if (info->stencil.texture != 0) {
    mr_mtl4_render_pass_set_stencil(pass, MR_WMT_RAW(info->stencil.texture),
                                    (mr_mtl4_load_action)info->stencil.load_action,
                                    (mr_mtl4_store_action)info->stencil.store_action,
                                    info->stencil.clear_stencil);
    mr_mtl4_render_pass_set_stencil_plane(pass, info->stencil.level, info->stencil.slice);
  }
  if (!mr_mtl4_frame_begin_render_pass(frame, pass)) {
    fprintf(stderr, "[mr-mtl4] render encoder failed: %s\n", mr_mtl4_last_error());
    mr_mtl4_render_pass_destroy(pass);
    return false;
  }

  for (uint32_t index = 0; index < kMaxEncoders; ++index) {
    if (state.encoder_live[index]) {
      continue;
    }
    state.encoder_live[index] = true;
    state.encoder_object[index] = (__bridge id)mr_mtl4_frame_metal_encoder(frame);
    *out_encoder = make_token(kKindEncoder, index);
    return true;
  }
  fprintf(stderr, "[mr-mtl4] no free encoder slot\n");
  return false;
}

bool mr_mtl4_wmt_session_encode(uint64_t encoder, const void *cmd_head, uint32_t *out_translated) {
  Session &state = start_session();
  if (state.failed || !is_token(encoder) || token_kind(encoder) != kKindEncoder) {
    return false;
  }
  const uint32_t index = token_index(encoder);
  if (index >= kMaxEncoders || !state.encoder_live[index]) {
    return false;
  }
  const uint32_t translated =
      mr_mtl4_wmt_encode_render((__bridge void *)state.encoder_object[index], state.vertex_table,
                                state.fragment_table, state.object_table, state.mesh_table,
                                state.transient, state.residency, cmd_head);
  if (out_translated != nullptr) {
    *out_translated = translated;
  }
  return true;
}

bool mr_mtl4_wmt_session_end_encoding(uint64_t handle) {
  Session &state = session();
  if (!state.started || state.failed || !is_token(handle) || token_kind(handle) != kKindEncoder) {
    return false;
  }
  const uint32_t index = token_index(handle);
  if (index >= kMaxEncoders || !state.encoder_live[index]) {
    return false;
  }
  state.encoder_live[index] = false;
  state.encoder_object[index] = nil;
  /* Ends whichever encoder is open on the frame this handle belongs to; the
   * token does not carry the frame, so the open frame is found by identity. */
  for (uint32_t slot = 0; slot < kMaxFrames; ++slot) {
    mr_mtl4_frame *frame = state.frames[slot];
    if (frame != nullptr && mr_mtl4_frame_encoder_is_open(frame)) {
      mr_mtl4_frame_end_encoder(frame);
    }
  }
  return true;
}

bool mr_mtl4_wmt_session_commit(uint64_t command_buffer) {
  Session &state = start_session();
  mr_mtl4_frame *frame = frame_for(command_buffer);
  if (state.failed || frame == nullptr) {
    return false;
  }
  if (!mr_mtl4_frame_commit(frame)) {
    fprintf(stderr, "[mr-mtl4] commit failed: %s\n", mr_mtl4_last_error());
  }
  return true;
}

bool mr_mtl4_wmt_session_wait(uint64_t command_buffer) {
  Session &state = start_session();
  mr_mtl4_frame *frame = frame_for(command_buffer);
  if (state.failed || frame == nullptr) {
    return false;
  }
  mr_mtl4_frame_wait(frame, 0);
  return true;
}

bool mr_mtl4_wmt_session_present(uint64_t command_buffer, uint64_t drawable_handle) {
  Session &state = start_session();
  mr_mtl4_frame *frame = frame_for(command_buffer);
  if (state.failed || frame == nullptr) {
    return false;
  }
  /* Presentation has to happen on the frame that is still open; a frame already
   * committed is waited on and released, which is the other half of the order
   * Metal 4 fixes. */
  for (uint32_t index = 0; index < kMaxFrames; ++index) {
    if (state.frames[index] == frame) {
      const bool presented =
          mr_mtl4_frame_present(frame, (void *)(uintptr_t)drawable_handle);
      if (!presented) {
        fprintf(stderr, "[mr-mtl4] present failed: %s\n", mr_mtl4_last_error());
      }
      mr_mtl4_frame_wait(frame, 5000);
      mr_mtl4_frame_destroy(frame);
      state.frames[index] = nullptr;
      state.live_frames -= 1;
      return true;
    }
  }
  return false;
}

uint32_t mr_mtl4_wmt_session_open_frames(void) { return session().live_frames; }
