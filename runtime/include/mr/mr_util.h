/*
 * Small ownership helpers. The core deliberately uses two explicit containers
 * (a growable byte string and a growable pointer vector) instead of pulling in
 * a general-purpose library: the code has to compile for iPadOS and be
 * auditable, and every allocation failure has to surface as MR_ERR_NOMEM rather
 * than a longjmp or an abort.
 */
#ifndef MR_UTIL_H
#define MR_UTIL_H

#include "mr/mr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- strings */

typedef struct {
  char *data;  /* always NUL-terminated once initialised */
  size_t len;  /* bytes before the terminator */
  size_t cap;
} mr_str;

mr_status mr_str_init(mr_str *s);
void mr_str_free(mr_str *s);
void mr_str_clear(mr_str *s);
mr_status mr_str_append(mr_str *s, const char *bytes, size_t len);
mr_status mr_str_appendz(mr_str *s, const char *cstr);
mr_status mr_str_appendf(mr_str *s, const char *fmt, ...);
/* Detach the buffer. The caller owns it and must free() it. */
char *mr_str_take(mr_str *s);

/* ---------------------------------------------------------------- vectors */

typedef struct {
  char **items;
  size_t count;
  size_t cap;
} mr_strvec;

mr_status mr_strvec_init(mr_strvec *v);
void mr_strvec_free(mr_strvec *v);
/* Copies `s`. */
mr_status mr_strvec_push(mr_strvec *v, const char *s);
/* Takes ownership of `s` on success; on failure `s` is left untouched. */
mr_status mr_strvec_take(mr_strvec *v, char *s);
bool mr_strvec_contains(const mr_strvec *v, const char *s);
void mr_strvec_sort(mr_strvec *v);

/* ------------------------------------------------------------------ bytes */

typedef struct {
  uint8_t *data;
  size_t len;
} mr_bytes;

mr_status mr_bytes_init(mr_bytes *b, size_t len);
void mr_bytes_free(mr_bytes *b);

/* ------------------------------------------------------------------- files */

/* Reads a whole file. Fails with MR_ERR_IO if it cannot be opened. */
mr_status mr_read_file(const char *path, mr_bytes *out);
/* Writes atomically via a temporary file and rename, so a crash mid-write
 * cannot leave a half-written profile or cache manifest behind. */
mr_status mr_write_file_atomic(const char *path, const void *data, size_t len);
/* Creates `path` and every missing parent directory. */
mr_status mr_mkdirs(const char *path);
bool mr_path_is_dir(const char *path);

/* Joins with '/'. Caller frees. */
char *mr_path_join(const char *a, const char *b);
/*
 * Joins into a caller-provided buffer. Returns false when the result would not
 * fit, leaving `dst` empty. Preferred over mr_path_join where the destination is
 * a fixed-size struct field, because snprintf would silently truncate a path and
 * a truncated path names a different file.
 */
bool mr_path_join_into(char *dst, size_t dst_cap, const char *a, const char *b);
/* Caller frees. Returns the pointer just past the final '/' or '\\'. */
char *mr_path_basename_dup(const char *path);

/*
 * True when `name` is safe to use as a single path component: not empty, not
 * "." or "..", containing no separator, no ':' and no control character, and
 * shorter than 256 bytes. Anything that names a directory we did not create goes
 * through this first, because the alternative is a game id or a cache key that
 * walks out of its own directory.
 */
bool mr_path_component_is_safe(const char *name);

/* ------------------------------------------------------------------ hashing */

/*
 * FNV-1a 64. Used for cache keys where the input is already a strong digest of
 * the real payload, and for profile/config fingerprints. It is not a
 * cryptographic hash and must not be used to authenticate anything.
 */
uint64_t mr_fnv1a64(const void *data, size_t len);
uint64_t mr_fnv1a64_update(uint64_t seed, const void *data, size_t len);
/* Renders a value as 16 lowercase hex digits into `out` (needs >= 17 bytes). */
void mr_hex64(uint64_t value, char out[17]);

/* ------------------------------------------------------------------- misc */

/* Bounds-checked unsigned reads, little endian. */
bool mr_read_u8_at(const uint8_t *data, size_t len, size_t off, uint8_t *out);
bool mr_read_u16(const uint8_t *data, size_t len, size_t off, uint16_t *out);
bool mr_read_u32(const uint8_t *data, size_t len, size_t off, uint32_t *out);
bool mr_read_u64(const uint8_t *data, size_t len, size_t off, uint64_t *out);
/* Bounds-checked ASCII reads from a fixed-width field. Writes at most `dst_cap`
 * bytes including the terminator and stops at the first NUL. */
void mr_read_ascii(const uint8_t *data, size_t len, size_t off, size_t width,
                   char *dst, size_t dst_cap);
/* Case-insensitive ASCII comparison, NULL-safe. */
int mr_strcasecmp(const char *a, const char *b);
bool mr_str_iequal(const char *a, const char *b);
bool mr_str_ends_with_ci(const char *s, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif /* MR_UTIL_H */
