/*
 * Shader and pipeline caches.
 *
 * Two caches, because they invalidate on different things:
 *
 *   shader cache    keyed on the guest bytecode (DXBC/DXIL) plus the backend's
 *                   own identity. Survives a driver or OS update.
 *   pipeline cache  keyed on the shader plus the full render state that the
 *                   pipeline bakes in -- formats, blend, sample count, vertex
 *                   layout. Invalidated by any of those changing.
 *
 * Collapsing the two would mean a render-state change discards translated
 * shaders that are still valid, and the whole point of the cache is to stop
 * paying shader compilation during play. A game that compiles a shader once and
 * then reuses it across forty blend states should translate once.
 *
 * Keys are hex strings derived from the payload. The cache never trusts a key
 * to be well formed: it re-derives it from the bytes it reads and treats a
 * mismatch as a miss, because a truncated write (a killed process, a full disk)
 * must not be served back as a valid shader.
 */
#ifndef MR_CACHE_H
#define MR_CACHE_H

#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MR_CACHE_KEY_LEN 16 /* 16 hex chars of a 64-bit digest */

typedef enum {
  MR_CACHE_SHADER = 0,
  MR_CACHE_PIPELINE,
  MR_CACHE_KIND_COUNT,
} mr_cache_kind;

const char *mr_cache_kind_str(mr_cache_kind kind);

typedef struct {
  uint64_t hits;
  uint64_t misses;
  uint64_t stores;
  uint64_t evictions;
  uint64_t bytes_written;
  uint64_t bytes_read;
  uint64_t rejected_corrupt; /* key matched, payload did not */
} mr_cache_stats;

typedef struct {
  char root[1024]; /* <root>/<game-id>/<kind>/ */
  char game_id[80];
  /*
   * Identifier for everything that can invalidate a translated shader:
   * backend generation, GPU family, OS build. Changing it retires the whole
   * cache for the game without deleting anything the user can still use after
   * a rollback.
   */
  char backend_signature[128];

  uint64_t max_bytes;
  uint64_t current_bytes;
  mr_cache_stats stats[MR_CACHE_KIND_COUNT];
} mr_shader_cache;

mr_status mr_shader_cache_open(mr_shader_cache *cache, const char *root,
                               const char *game_id, const char *backend_sig,
                               uint64_t max_bytes);
void mr_shader_cache_close(mr_shader_cache *cache);

/*
 * Computes the key for a translatable unit.
 *
 * `guest_bytecode` and `state` are both folded in. `state` may be empty (a
 * shader-cache lookup) but must be non-empty for a pipeline key, which is the
 * only difference between the two kinds.
 */
void mr_cache_key(mr_cache_kind kind, const void *guest_bytecode,
                  size_t bytecode_len, const void *state, size_t state_len,
                  const char *backend_signature, char out[MR_CACHE_KEY_LEN + 1]);

/* Returns MR_ERR_NOTFOUND on a miss or a rejected payload. */
mr_status mr_shader_cache_get(mr_shader_cache *cache, mr_cache_kind kind,
                              const char *key, mr_bytes *out);
mr_status mr_shader_cache_put(mr_shader_cache *cache, mr_cache_kind kind,
                              const char *key, const void *data, size_t len);

/* Removes least-recently-used entries until the cache fits `target_bytes`. */
mr_status mr_shader_cache_evict_to(mr_shader_cache *cache, uint64_t target_bytes);
/* Recomputes current_bytes from the directory, and drops entries whose payload
 * does not match their filename. This is the repair path after a crash. */
mr_status mr_shader_cache_reindex(mr_shader_cache *cache);
mr_status mr_shader_cache_clear(mr_shader_cache *cache, mr_cache_kind kind);

/*
 * Removes cache directories for games that no longer exist in `profiles_root`.
 * Returns the number of bytes reclaimed through `reclaimed` when non-NULL.
 */
mr_status mr_shader_cache_sweep_orphans(const char *cache_root,
                                        const char *profiles_root,
                                        uint64_t *reclaimed);

/*
 * A pre-warm pass: translates shaders found in the game's data before the
 * first frame needs them. This is the single most valuable use of the cache,
 * because a stutter at first use is the visible cost of compiling during play.
 */
typedef mr_status (*mr_cache_warm_fn)(void *ctx, const void *bytecode,
                                      size_t len, mr_bytes *translated_out);
mr_status mr_shader_cache_warm(mr_shader_cache *cache, mr_cache_kind kind,
                               mr_cache_warm_fn translate, void *ctx,
                               const uint8_t *const *blobs,
                               const size_t *blob_lens, size_t blob_count,
                               size_t *translated_count);

#ifdef __cplusplus
}
#endif

#endif /* MR_CACHE_H */
