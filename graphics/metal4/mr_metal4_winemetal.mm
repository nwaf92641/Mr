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

struct State {
  __unsafe_unretained id<MTL4RenderCommandEncoder> encoder;
  Stage vertex;
  Stage fragment;
  mr_mtl4_transient *transient;
  uint32_t untranslated; /* types with no translation, reported once each */
  uint32_t reported[64];
  uint32_t reported_count;
};

/* Metal 4 binds a buffer by address, so an offset cannot be changed on its own
 * the way setVertexBufferOffset:atIndex: did -- the whole binding is remade
 * from the address the shadow remembers. */
void rebind(Stage &stage, uint32_t index, mr_mtl4_table *table) {
  if (!stage.valid(index) || stage.buffers[index] == nil) {
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

bool encode_one(State &state, mr_mtl4_table *table, const struct wmtcmd_base *command) {
  switch (command->type) {
    case WMTRenderCommandSetVertexBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!state.vertex.valid(cmd->index)) {
        break;
      }
      state.vertex.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      state.vertex.offsets[cmd->index] = cmd->offset;
      rebind(state.vertex, cmd->index, table);
      return true;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!state.vertex.valid(cmd->index)) {
        break;
      }
      state.vertex.offsets[cmd->index] = cmd->offset;
      rebind(state.vertex, cmd->index, table);
      return true;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const auto *cmd = (const struct wmtcmd_render_setbuffer *)command;
      if (!state.fragment.valid(cmd->index)) {
        break;
      }
      state.fragment.buffers[cmd->index] = MR_WMT_OBJ(cmd->buffer);
      state.fragment.offsets[cmd->index] = cmd->offset;
      rebind(state.fragment, cmd->index, table);
      return true;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const auto *cmd = (const struct wmtcmd_render_setbufferoffset *)command;
      if (!state.fragment.valid(cmd->index)) {
        break;
      }
      state.fragment.offsets[cmd->index] = cmd->offset;
      rebind(state.fragment, cmd->index, table);
      return true;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const auto *cmd = (const struct wmtcmd_render_settexture *)command;
      mr_mtl4_table_set_texture(table, MR_WMT_RAW(cmd->texture), cmd->index);
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
      mr_mtl4_table_set_buffer(table, address, cmd->index);
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

uint32_t mr_mtl4_wmt_encode_render(void *mtl4_render_encoder, mr_mtl4_table *argument_table,
                                   void *mtl4_transient, const void *cmd_head) {
  if (cmd_head == nullptr) {
    return 0;
  }
  if (argument_table == nullptr) {
    fprintf(stderr, "[mr-mtl4] encode_render with no argument table -- nothing encoded\n");
    return 0;
  }

  State state;
  state.encoder = (__bridge id<MTL4RenderCommandEncoder>)mtl4_render_encoder;
  state.transient = (mr_mtl4_transient *)mtl4_transient;

  if (@available(iOS 26.0, macOS 26.0, *)) {
    if (state.encoder != nil) {
      /* One table, both stages: the stream interleaves vertex and fragment
       * bindings into the same index spaces the shaders were compiled against,
       * so splitting it per stage would change what an index means. */
      [(id<MTL4RenderCommandEncoder>)state.encoder
          setArgumentTable:(__bridge id<MTL4ArgumentTable>)mr_mtl4_table_metal_table(argument_table)
                  atStages:(MTLRenderStages)(MTLRenderStageVertex | MTLRenderStageFragment)];
    }
  } else {
    fprintf(stderr, "[mr-mtl4] encode_render on a system without Metal 4 -- nothing encoded\n");
    return 0;
  }

  uint32_t translated = 0;
  Journal &log = journal();
  for (const struct wmtcmd_base *command = (const struct wmtcmd_base *)cmd_head; command != nullptr;
       command = (const struct wmtcmd_base *)command->next.ptr) {
    if (encode_one(state, argument_table, command)) {
      translated += 1;
      if (log.translated_count < 64) {
        log.translated[log.translated_count++] = command->type;
      }
    }
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
