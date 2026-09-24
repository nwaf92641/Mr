/* Mr — winemetal command stream to Metal 4.
 *
 * DXMT does not build a Metal encoder by calling Metal per command. It builds a
 * serialised command list and hands the head of it to one unix-call, which is
 * where the whole render pass is replayed. That list is the seam this file
 * translates: the guest keeps producing exactly what it produces today, and the
 * replay target becomes an MTL4RenderCommandEncoder with an MTL4ArgumentTable
 * instead of an MTLRenderCommandEncoder with direct binds.
 *
 * The stream is a linked list of structs whose first three fields are always
 * {type, reserved[3], next}, and on arm64 WMTMemoryPointer is a bare void*, so
 * walking it needs no marshalling and no guest pointers on the wire.
 *
 * Called from winemetal_unix.c's _MTLRenderCommandEncoder_encodeCommands on the
 * Metal 4 path. The signature is C on purpose: that call site is C++ but not
 * ARC, and this file is ARC.
 */
#ifndef MR_METAL4_WINEMETAL_H
#define MR_METAL4_WINEMETAL_H

#include <stdint.h>

#include "mr_metal4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* mtl4_render_encoder is an id<MTL4RenderCommandEncoder>, mtl4_transient an
 * mr_mtl4_transient*, and cmd_head the `struct wmtcmd_base *` DXMT passes. The
 * argument table is the layer's handle, not a raw object, so the bindings keep
 * going through mr_mtl4_table_set_buffer/texture/sampler.
 *
 * Every pointer may be NULL, in which case the list is walked and reported but
 * nothing is encoded.
 *
 * Returns the number of commands translated and issued. Command types that are
 * not translated yet are counted and named once each on stderr: DXMT drops a
 * whole batch when a command is unsupported, so silence here would look like a
 * working frame with missing state. */
uint32_t mr_mtl4_wmt_encode_render(void *mtl4_render_encoder, mr_mtl4_table *vertex_table,
                                   mr_mtl4_table *fragment_table, mr_mtl4_table *object_table,
                                   void *mtl4_transient, mr_mtl4_residency *residency,
                                   const void *cmd_head);

/* The type ids the walker translated on the last call, in the order it first
 * met them, so a test can assert what a frame actually contained rather than
 * trusting the absence of warnings. 64 entries is DXMT's own limit. */
uint32_t mr_mtl4_wmt_translated_count(void);
uint32_t mr_mtl4_wmt_translated_type(uint32_t position);

/* Command types the walker has no translation for, across all calls. */
uint32_t mr_mtl4_wmt_untranslated_count(void);
uint32_t mr_mtl4_wmt_untranslated_type(uint32_t position);

/* ------------------------------------------------- session routing for DXMT
 *
 * winemetal_unix.c calls these at the top of the seven slots the Metal 4 path
 * replaces. Each takes and returns plain integers, so that translation unit
 * needs no new include and the bridge needs no knowledge of the unix-call
 * parameter structs.
 *
 * Command buffers and encoders become tagged tokens here, because Metal 4 will
 * not accept the Metal 3 objects the guest is holding. A real arm64 user-space
 * pointer has zero in its top 16 bits and these tokens do not, so a handle can
 * be classified with one mask and no lookup.
 *
 * Each session_* function returns false when the handle is not ours, and the
 * caller must then run its Metal 3 body unchanged. That is what keeps this a
 * route rather than a fork: with Metal 4 unavailable nothing here is reached
 * and DXMT behaves exactly as it does today. */
bool mr_mtl4_wmt_session_available(void);

/* Returns a token for the new command buffer, or 0. */
uint64_t mr_mtl4_wmt_session_command_buffer(uint64_t mtl3_command_buffer);

/* render_pass_info is the WMTRenderPassInfo* the slot receives. On success
 * *out_encoder is the encoder token. */
bool mr_mtl4_wmt_session_render_encoder(uint64_t command_buffer, const void *render_pass_info,
                                        uint64_t *out_encoder);

/* cmd_head is the WMTConstMemoryPointer's .ptr. *out_translated reports how
 * many commands reached Metal, so a frame can be judged rather than assumed. */
bool mr_mtl4_wmt_session_encode(uint64_t encoder, const void *cmd_head, uint32_t *out_translated);

bool mr_mtl4_wmt_session_end_encoding(uint64_t handle);
bool mr_mtl4_wmt_session_commit(uint64_t command_buffer);
bool mr_mtl4_wmt_session_wait(uint64_t command_buffer);
bool mr_mtl4_wmt_session_present(uint64_t command_buffer, uint64_t drawable_handle);

/* Frames and encoders still alive, for a leak check. */
uint32_t mr_mtl4_wmt_session_open_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* MR_METAL4_WINEMETAL_H */
