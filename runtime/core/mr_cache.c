/*
 * The shader and pipeline caches.
 *
 * Two caches because they invalidate on different things, and collapsing them
 * would throw away valid translated shaders whenever a blend state changed.
 *
 * Every entry is written through a temporary file and renamed, and every entry
 * carries a header with the key it was stored under plus a checksum of its
 * payload. A killed process or a full disk therefore leaves either no file or a
 * file that fails its checksum -- never a half-written entry that the next run
 * happily feeds to the driver as a shader. That distinction matters more than it
 * looks: a corrupt shader surfaces as a black screen hours later, while a cache
 * miss surfaces as one slow frame now.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h> /* utimes: mark an entry as recently used on a hit */
#include <time.h>
#include <unistd.h>

#define MR_CACHE_MAGIC "MRCH"
#define MR_CACHE_FORMAT_VERSION 1u
#define MR_CACHE_HEADER_SIZE 40u

const char *mr_cache_kind_str(mr_cache_kind kind) {
  switch (kind) {
    case MR_CACHE_SHADER: return "shader";
    case MR_CACHE_PIPELINE: return "pipeline";
    case MR_CACHE_KIND_COUNT: break;
  }
  return "unknown";
}

/* -------------------------------------------------------------- keying */

void mr_cache_key(mr_cache_kind kind, const void *guest_bytecode,
                  size_t bytecode_len, const void *state, size_t state_len,
                  const char *backend_signature,
                  char out[MR_CACHE_KEY_LEN + 1]) {
  /* Domain separation: the kind is mixed in first so a shader and a pipeline
   * with identical inputs cannot collide, which they otherwise would because
   * a pipeline entry's state is often empty for a shader-only lookup. */
  uint64_t h = mr_fnv1a64(&kind, sizeof(kind));
  h = mr_fnv1a64_update(h, guest_bytecode, bytecode_len);
  h = mr_fnv1a64_update(h, state, state_len);
  if (backend_signature != NULL) {
    h = mr_fnv1a64_update(h, backend_signature, strlen(backend_signature));
  }

  char full[17];
  mr_hex64(h, full);
  memcpy(out, full, MR_CACHE_KEY_LEN);
  out[MR_CACHE_KEY_LEN] = '\0';
}

/* ------------------------------------------------------------- lifecycle */

static bool mr_cache_dir_for(const mr_shader_cache *cache, mr_cache_kind kind,
                             char *dst, size_t dst_cap) {
  if (!mr_path_join_into(dst, dst_cap, cache->root, cache->game_id)) return false;
  char kind_dir[1024];
  if (!mr_path_join_into(kind_dir, sizeof(kind_dir), dst,
                         mr_cache_kind_str(kind))) {
    return false;
  }
  snprintf(dst, dst_cap, "%s", kind_dir);
  return true;
}

static bool mr_cache_path_for(const mr_shader_cache *cache, mr_cache_kind kind,
                              const char *key, char *dst, size_t dst_cap) {
  char dir[1024];
  if (!mr_cache_dir_for(cache, kind, dir, sizeof(dir))) return false;

  mr_str name;
  if (mr_str_init(&name) != MR_OK) return false;
  mr_status st = mr_str_appendz(&name, key);
  if (st == MR_OK) st = mr_str_appendz(&name, ".bin");
  if (st != MR_OK) {
    mr_str_free(&name);
    return false;
  }

  bool ok = mr_path_join_into(dst, dst_cap, dir, name.data);
  mr_str_free(&name);
  return ok;
}

mr_status mr_shader_cache_open(mr_shader_cache *cache, const char *root,
                               const char *game_id, const char *backend_sig,
                               uint64_t max_bytes) {
  if (cache == NULL || root == NULL || game_id == NULL || backend_sig == NULL) {
    return MR_ERR_INVALID;
  }
  if (!mr_path_component_is_safe(game_id)) return MR_ERR_INVALID;

  memset(cache, 0, sizeof(*cache));
  snprintf(cache->root, sizeof(cache->root), "%s", root);
  snprintf(cache->game_id, sizeof(cache->game_id), "%s", game_id);
  snprintf(cache->backend_signature, sizeof(cache->backend_signature), "%s",
           backend_sig);
  cache->max_bytes = max_bytes;

  for (int i = 0; i < MR_CACHE_KIND_COUNT; i++) {
    char dir[1024];
    if (!mr_cache_dir_for(cache, (mr_cache_kind)i, dir, sizeof(dir))) {
      return MR_ERR_RANGE;
    }
    mr_status st = mr_mkdirs(dir);
    if (st != MR_OK) return st;
  }

  return mr_shader_cache_reindex(cache);
}

void mr_shader_cache_close(mr_shader_cache *cache) {
  if (cache == NULL) return;
  memset(cache, 0, sizeof(*cache));
}

/* ---------------------------------------------------------------- entries */

/* FNV-1a again, used here only to detect a truncated or scribbled payload.
 * This is integrity against a crashed writer, not against an attacker: the
 * cache is inside the game's own profile directory and a local attacker who can
 * write there can replace the shader outright. */
static uint64_t mr_payload_checksum(const void *data, size_t len) {
  return mr_fnv1a64(data, len);
}

static void mr_put_u32(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)(v & 0xFFu);
  dst[1] = (uint8_t)((v >> 8) & 0xFFu);
  dst[2] = (uint8_t)((v >> 16) & 0xFFu);
  dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void mr_put_u64(uint8_t *dst, uint64_t v) {
  mr_put_u32(dst, (uint32_t)(v & 0xFFFFFFFFu));
  mr_put_u32(dst + 4, (uint32_t)(v >> 32));
}

static uint64_t mr_cache_key_digest(const char *key) {
  /* The key is the digest rendered as hex; comparing hex strings is enough and
   * avoids storing a second binary form. */
  return mr_fnv1a64(key, strlen(key));
}

mr_status mr_shader_cache_get(mr_shader_cache *cache, mr_cache_kind kind,
                              const char *key, mr_bytes *out) {
  if (cache == NULL || key == NULL || out == NULL) return MR_ERR_INVALID;
  if (kind >= MR_CACHE_KIND_COUNT) return MR_ERR_INVALID;

  out->data = NULL;
  out->len = 0;

  char path[1024];
  if (!mr_cache_path_for(cache, kind, key, path, sizeof(path))) {
    return MR_ERR_RANGE;
  }

  mr_bytes raw;
  mr_status st = mr_read_file(path, &raw);
  if (st != MR_OK) {
    cache->stats[kind].misses++;
    return MR_ERR_NOTFOUND;
  }

  if (raw.len < MR_CACHE_HEADER_SIZE ||
      memcmp(raw.data, MR_CACHE_MAGIC, 4) != 0) {
    cache->stats[kind].rejected_corrupt++;
    mr_bytes_free(&raw);
    return MR_ERR_NOTFOUND;
  }

  uint32_t version = 0;
  uint64_t stored_digest = 0;
  uint64_t payload_len = 0;
  uint64_t stored_checksum = 0;
  (void)mr_read_u32(raw.data, raw.len, 4, &version);
  (void)mr_read_u64(raw.data, raw.len, 12, &stored_digest);
  (void)mr_read_u64(raw.data, raw.len, 20, &payload_len);
  (void)mr_read_u64(raw.data, raw.len, 28, &stored_checksum);

  if (version != MR_CACHE_FORMAT_VERSION ||
      stored_digest != mr_cache_key_digest(key) ||
      payload_len != raw.len - MR_CACHE_HEADER_SIZE) {
    cache->stats[kind].rejected_corrupt++;
    mr_bytes_free(&raw);
    return MR_ERR_NOTFOUND;
  }

  const uint8_t *payload = raw.data + MR_CACHE_HEADER_SIZE;
  if (mr_payload_checksum(payload, (size_t)payload_len) != stored_checksum) {
    cache->stats[kind].rejected_corrupt++;
    mr_bytes_free(&raw);
    return MR_ERR_NOTFOUND;
  }

  /* Hand the payload to the caller with an owned allocation, so the caller can
   * free it with the same mr_bytes_free it uses everywhere else. */
  mr_status alloc = mr_bytes_init(out, (size_t)payload_len);
  if (alloc != MR_OK) {
    mr_bytes_free(&raw);
    return alloc;
  }
  if (payload_len != 0) memcpy(out->data, payload, (size_t)payload_len);

  mr_bytes_free(&raw);

  cache->stats[kind].hits++;
  cache->stats[kind].bytes_read += payload_len;
  cache->current_bytes += 0; /* reads do not change the footprint */

  /* Mark the entry as recently used without rewriting it. */
  (void)utimes(path, NULL);

  return MR_OK;
}

mr_status mr_shader_cache_put(mr_shader_cache *cache, mr_cache_kind kind,
                              const char *key, const void *data, size_t len) {
  if (cache == NULL || key == NULL || (data == NULL && len != 0)) {
    return MR_ERR_INVALID;
  }
  if (kind >= MR_CACHE_KIND_COUNT) return MR_ERR_INVALID;

  char path[1024];
  if (!mr_cache_path_for(cache, kind, key, path, sizeof(path))) {
    return MR_ERR_RANGE;
  }

  uint8_t header[MR_CACHE_HEADER_SIZE];
  memset(header, 0, sizeof(header));
  memcpy(header, MR_CACHE_MAGIC, 4);
  mr_put_u32(header + 4, MR_CACHE_FORMAT_VERSION);
  mr_put_u32(header + 8, (uint32_t)kind);
  mr_put_u64(header + 12, mr_cache_key_digest(key));
  mr_put_u64(header + 20, (uint64_t)len);
  mr_put_u64(header + 28, mr_payload_checksum(data, len));

  mr_str blob;
  mr_status st = mr_str_init(&blob);
  if (st != MR_OK) return st;
  st = mr_str_append(&blob, (const char *)header, sizeof(header));
  if (st == MR_OK) st = mr_str_append(&blob, (const char *)data, len);
  if (st != MR_OK) {
    mr_str_free(&blob);
    return st;
  }

  st = mr_write_file_atomic(path, blob.data, blob.len);

  /* Account for the write even on failure so a broken cache does not look
   * empty to the eviction pass. */
  if (st == MR_OK) {
    cache->stats[kind].stores++;
    cache->stats[kind].bytes_written += blob.len;
    cache->current_bytes += blob.len;
  }
  mr_str_free(&blob);
  return st;
}

/* --------------------------------------------------------------- eviction */

typedef struct {
  char path[1200];
  uint64_t size;
  time_t used_at;
} mr_cache_entry_info;

static void mr_rmtree(const char *path) {
  DIR *d = opendir(path);
  if (d == NULL) {
    (void)unlink(path);
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    char *child = mr_path_join(path, ent->d_name);
    if (child == NULL) continue;
    struct stat st;
    if (lstat(child, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        mr_rmtree(child);
      } else {
        (void)unlink(child);
      }
    }
    free(child);
  }
  (void)closedir(d);
  (void)rmdir(path);
}

static int mr_compare_entries(const void *a, const void *b) {
  const mr_cache_entry_info *x = (const mr_cache_entry_info *)a;
  const mr_cache_entry_info *y = (const mr_cache_entry_info *)b;
  if (x->used_at < y->used_at) return -1;
  if (x->used_at > y->used_at) return 1;
  /* Deterministic tie-break so an eviction run is reproducible. */
  return strcmp(x->path, y->path);
}

static size_t mr_collect_entries(mr_shader_cache *cache, mr_cache_kind kind,
                                 mr_cache_entry_info *out, size_t capacity,
                                 uint64_t *total_bytes) {
  char dir[1024];
  if (!mr_cache_dir_for(cache, kind, dir, sizeof(dir))) return 0;

  DIR *d = opendir(dir);
  if (d == NULL) return 0;

  size_t count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL && count < capacity) {
    if (ent->d_name[0] == '.') continue;
    char *full = mr_path_join(dir, ent->d_name);
    if (full == NULL) continue;

    struct stat st;
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
      snprintf(out[count].path, sizeof(out[count].path), "%s", full);
      out[count].size = (uint64_t)st.st_size;
      out[count].used_at = st.st_mtime;
      *total_bytes += out[count].size;
      count++;
    }
    free(full);
  }
  (void)closedir(d);
  return count;
}

mr_status mr_shader_cache_reindex(mr_shader_cache *cache) {
  if (cache == NULL) return MR_ERR_INVALID;

  const size_t capacity = 8192;
  mr_cache_entry_info *entries =
      (mr_cache_entry_info *)calloc(capacity, sizeof(*entries));
  if (entries == NULL) return MR_ERR_NOMEM;

  cache->current_bytes = 0;
  for (int k = 0; k < MR_CACHE_KIND_COUNT; k++) {
    cache->stats[k] = (mr_cache_stats){0};
    uint64_t bytes = 0;
    size_t count =
        mr_collect_entries(cache, (mr_cache_kind)k, entries, capacity, &bytes);
    cache->current_bytes += bytes;

    /*
     * Drop entries whose name is not a 16-hex-digit key. Those cannot have been
     * written by this code; the likeliest origin is a partially-written file
     * from an interrupted rename on a filesystem without atomic rename, and
     * keeping them would make every eviction pass miscount.
     */
    for (size_t i = 0; i < count; i++) {
      const char *name = strrchr(entries[i].path, '/');
      name = name != NULL ? name + 1 : entries[i].path;
      bool valid = strlen(name) == MR_CACHE_KEY_LEN + 4; /* key + ".bin" */
      for (size_t c = 0; valid && c < MR_CACHE_KEY_LEN; c++) {
        char ch = name[c];
        valid = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      }
      if (!valid) {
        (void)unlink(entries[i].path);
        cache->current_bytes -= entries[i].size;
      }
    }
  }

  free(entries);
  return MR_OK;
}

mr_status mr_shader_cache_evict_to(mr_shader_cache *cache,
                                   uint64_t target_bytes) {
  if (cache == NULL) return MR_ERR_INVALID;
  if (cache->current_bytes <= target_bytes) return MR_OK;

  const size_t capacity = 8192;
  mr_cache_entry_info *entries =
      (mr_cache_entry_info *)calloc(capacity, sizeof(*entries));
  if (entries == NULL) return MR_ERR_NOMEM;

  uint64_t total = 0;
  size_t count = 0;
  for (int k = 0; k < MR_CACHE_KIND_COUNT; k++) {
    uint64_t bytes = 0;
    count += mr_collect_entries(cache, (mr_cache_kind)k, entries + count,
                               capacity - count, &bytes);
    total += bytes;
  }

  qsort(entries, count, sizeof(*entries), mr_compare_entries);

  for (size_t i = 0; i < count && total > target_bytes; i++) {
    if (unlink(entries[i].path) == 0) {
      total -= entries[i].size;
      /* Attribute the eviction to the kind whose directory it lives in, so the
       * per-kind statistics stay meaningful. */
      for (int k = 0; k < MR_CACHE_KIND_COUNT; k++) {
        char dir[1024];
        if (!mr_cache_dir_for(cache, (mr_cache_kind)k, dir, sizeof(dir))) break;
        if (strncmp(entries[i].path, dir, strlen(dir)) == 0) {
          cache->stats[k].evictions++;
          break;
        }
      }
    }
  }

  free(entries);
  cache->current_bytes = total;
  return MR_OK;
}

mr_status mr_shader_cache_clear(mr_shader_cache *cache, mr_cache_kind kind) {
  if (cache == NULL) return MR_ERR_INVALID;
  if (kind >= MR_CACHE_KIND_COUNT) return MR_ERR_INVALID;

  char dir[1024];
  if (!mr_cache_dir_for(cache, kind, dir, sizeof(dir))) return MR_ERR_RANGE;

  mr_rmtree(dir);
  cache->stats[kind] = (mr_cache_stats){0};
  return mr_shader_cache_reindex(cache);
}

/* --------------------------------------------------------------- recycling */

/* Total size of the regular files under `dir`, recursively. Used to report what
 * a sweep reclaimed, which is why it runs before the delete rather than after. */
static uint64_t mr_cache_tree_bytes(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) return 0;

  uint64_t total = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    char *full = mr_path_join(dir, ent->d_name);
    if (full == NULL) continue;

    struct stat st;
    if (stat(full, &st) == 0) {
      if (S_ISREG(st.st_mode)) {
        total += (uint64_t)st.st_size;
      } else if (S_ISDIR(st.st_mode)) {
        total += mr_cache_tree_bytes(full);
      }
    }
    free(full);
  }
  (void)closedir(d);
  return total;
}

mr_status mr_shader_cache_sweep_orphans(const char *cache_root,
                                        const char *profiles_root,
                                        uint64_t *reclaimed) {
  if (cache_root == NULL || profiles_root == NULL) return MR_ERR_INVALID;
  if (reclaimed != NULL) *reclaimed = 0;

  DIR *d = opendir(cache_root);
  if (d == NULL) return MR_ERR_NOTFOUND;

  /* Names are collected before anything is deleted: deleting inside the
   * readdir loop invalidates the directory stream on some filesystems. */
  mr_strvec names;
  if (mr_strvec_init(&names) != MR_OK) {
    (void)closedir(d);
    return MR_ERR_NOMEM;
  }

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    (void)mr_strvec_push(&names, ent->d_name);
  }
  (void)closedir(d);

  uint64_t freed = 0;
  for (size_t i = 0; i < names.count; i++) {
    char *profile_dir = mr_path_join(profiles_root, names.items[i]);
    if (profile_dir == NULL) continue;

    struct stat st;
    bool has_profile = (stat(profile_dir, &st) == 0 && S_ISDIR(st.st_mode));
    free(profile_dir);
    if (has_profile) continue;

    char *cache_dir = mr_path_join(cache_root, names.items[i]);
    if (cache_dir == NULL) continue;

    /* Summed before deleting, so the caller learns how much was reclaimed. The
     * walk has to be recursive: a game's cache is
     * <cache>/<game>/<kind>/<key>.bin, so a sum over the immediate children only
     * sees the kind directories and returns zero for exactly the layout the
     * runtime creates. */
    freed += mr_cache_tree_bytes(cache_dir);

    mr_rmtree(cache_dir);
    free(cache_dir);
  }

  mr_strvec_free(&names);
  if (reclaimed != NULL) *reclaimed = freed;
  return MR_OK;
}

/* --------------------------------------------------------------- pre-warm */

mr_status mr_shader_cache_warm(mr_shader_cache *cache, mr_cache_kind kind,
                               mr_cache_warm_fn translate, void *ctx,
                               const uint8_t *const *blobs,
                               const size_t *blob_lens, size_t blob_count,
                               size_t *translated_count) {
  if (cache == NULL || translate == NULL || blobs == NULL ||
      blob_lens == NULL) {
    return MR_ERR_INVALID;
  }
  if (translated_count != NULL) *translated_count = 0;

  size_t done = 0;
  for (size_t i = 0; i < blob_count; i++) {
    char key[MR_CACHE_KEY_LEN + 1];
    mr_cache_key(kind, blobs[i], blob_lens[i], NULL, 0,
                 cache->backend_signature, key);

    mr_bytes existing;
    if (mr_shader_cache_get(cache, kind, key, &existing) == MR_OK) {
      mr_bytes_free(&existing);
      continue;
    }

    mr_bytes translated;
    mr_status st = translate(ctx, blobs[i], blob_lens[i], &translated);
    if (st != MR_OK) {
      /*
       * A blob that will not translate is not a reason to abandon the warm
       * pass: the title may never use it, and stopping here would trade a
       * possible stutter for a definite one at whatever it compiles next.
       */
      continue;
    }

    (void)mr_shader_cache_put(cache, kind, key, translated.data, translated.len);
    mr_bytes_free(&translated);
    done++;
  }

  if (translated_count != NULL) *translated_count = done;
  return MR_OK;
}
