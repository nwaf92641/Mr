/*
 * The POSIX feature macros must be set before any header is included, and they
 * are set here rather than in the build system so the file can be compiled by
 * hand during debugging. -std=c11 hides mkstemp, fdopen, fileno, sysconf and
 * statvfs without them.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define MR_MKDIR(p) _mkdir(p)
#define MR_UNLINK(p) _unlink(p)
#else
#include <unistd.h>
#define MR_MKDIR(p) mkdir((p), 0755)
#define MR_UNLINK(p) unlink(p)
#endif

/* ---------------------------------------------------------------- strings */

mr_status mr_str_init(mr_str *s) {
  if (s == NULL) return MR_ERR_INVALID;
  s->cap = 64;
  s->data = (char *)malloc(s->cap);
  if (s->data == NULL) {
    s->cap = 0;
    s->len = 0;
    return MR_ERR_NOMEM;
  }
  s->data[0] = '\0';
  s->len = 0;
  return MR_OK;
}

void mr_str_free(mr_str *s) {
  if (s == NULL) return;
  free(s->data);
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}

void mr_str_clear(mr_str *s) {
  if (s == NULL) return;
  s->len = 0;
  if (s->data != NULL) s->data[0] = '\0';
}

/* Grows to hold `needed` bytes plus a terminator. */
static mr_status mr_str_reserve(mr_str *s, size_t needed) {
  if (s->data == NULL) {
    mr_status st = mr_str_init(s);
    if (st != MR_OK) return st;
  }
  if (needed + 1 <= s->cap) return MR_OK;

  size_t cap = s->cap;
  while (cap < needed + 1) {
    if (cap > (SIZE_MAX / 2)) {
      cap = needed + 1;
      break;
    }
    cap *= 2;
  }
  char *grown = (char *)realloc(s->data, cap);
  if (grown == NULL) return MR_ERR_NOMEM;
  s->data = grown;
  s->cap = cap;
  return MR_OK;
}

mr_status mr_str_append(mr_str *s, const char *bytes, size_t len) {
  if (s == NULL || (bytes == NULL && len != 0)) return MR_ERR_INVALID;
  mr_status st = mr_str_reserve(s, s->len + len);
  if (st != MR_OK) return st;
  if (len != 0) memcpy(s->data + s->len, bytes, len);
  s->len += len;
  s->data[s->len] = '\0';
  return MR_OK;
}

mr_status mr_str_appendz(mr_str *s, const char *cstr) {
  if (cstr == NULL) return MR_ERR_INVALID;
  return mr_str_append(s, cstr, strlen(cstr));
}

mr_status mr_str_appendf(mr_str *s, const char *fmt, ...) {
  if (s == NULL || fmt == NULL) return MR_ERR_INVALID;

  va_list args;
  va_start(args, fmt);
  va_list probe;
  va_copy(probe, args);
  int needed = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);
  if (needed < 0) {
    va_end(args);
    return MR_ERR_INVALID;
  }

  mr_status st = mr_str_reserve(s, s->len + (size_t)needed);
  if (st != MR_OK) {
    va_end(args);
    return st;
  }
  int written = vsnprintf(s->data + s->len, (size_t)needed + 1, fmt, args);
  va_end(args);
  if (written < 0) return MR_ERR_INVALID;
  s->len += (size_t)written;
  return MR_OK;
}

char *mr_str_take(mr_str *s) {
  if (s == NULL) return NULL;
  char *out = s->data;
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
  return out;
}

/* ---------------------------------------------------------------- vectors */

mr_status mr_strvec_init(mr_strvec *v) {
  if (v == NULL) return MR_ERR_INVALID;
  v->items = NULL;
  v->count = 0;
  v->cap = 0;
  return MR_OK;
}

void mr_strvec_free(mr_strvec *v) {
  if (v == NULL) return;
  for (size_t i = 0; i < v->count; i++) free(v->items[i]);
  free(v->items);
  v->items = NULL;
  v->count = 0;
  v->cap = 0;
}

static mr_status mr_strvec_reserve(mr_strvec *v, size_t needed) {
  if (needed <= v->cap) return MR_OK;
  size_t cap = v->cap == 0 ? 8 : v->cap;
  while (cap < needed) {
    if (cap > (SIZE_MAX / 2)) {
      cap = needed;
      break;
    }
    cap *= 2;
  }
  char **grown = (char **)realloc(v->items, cap * sizeof(*grown));
  if (grown == NULL) return MR_ERR_NOMEM;
  v->items = grown;
  v->cap = cap;
  return MR_OK;
}

mr_status mr_strvec_take(mr_strvec *v, char *s) {
  if (v == NULL || s == NULL) return MR_ERR_INVALID;
  mr_status st = mr_strvec_reserve(v, v->count + 1);
  if (st != MR_OK) return st;
  v->items[v->count++] = s;
  return MR_OK;
}

mr_status mr_strvec_push(mr_strvec *v, const char *s) {
  if (s == NULL) return MR_ERR_INVALID;
  size_t len = strlen(s);
  char *copy = (char *)malloc(len + 1);
  if (copy == NULL) return MR_ERR_NOMEM;
  memcpy(copy, s, len + 1);
  mr_status st = mr_strvec_take(v, copy);
  if (st != MR_OK) free(copy);
  return st;
}

bool mr_strvec_contains(const mr_strvec *v, const char *s) {
  if (v == NULL || s == NULL) return false;
  for (size_t i = 0; i < v->count; i++) {
    if (strcmp(v->items[i], s) == 0) return true;
  }
  return false;
}

static int mr_cmp_cstr(const void *a, const void *b) {
  const char *const *lhs = (const char *const *)a;
  const char *const *rhs = (const char *const *)b;
  return strcmp(*lhs, *rhs);
}

void mr_strvec_sort(mr_strvec *v) {
  if (v == NULL || v->count < 2) return;
  qsort(v->items, v->count, sizeof(*v->items), mr_cmp_cstr);
}

/* ------------------------------------------------------------------ bytes */

mr_status mr_bytes_init(mr_bytes *b, size_t len) {
  if (b == NULL) return MR_ERR_INVALID;
  b->data = NULL;
  b->len = 0;
  if (len == 0) return MR_OK;
  b->data = (uint8_t *)malloc(len);
  if (b->data == NULL) return MR_ERR_NOMEM;
  b->len = len;
  return MR_OK;
}

void mr_bytes_free(mr_bytes *b) {
  if (b == NULL) return;
  free(b->data);
  b->data = NULL;
  b->len = 0;
}

/* ------------------------------------------------------------------- files */

static mr_status mr_read_stream(FILE *f, mr_bytes *out) {
  size_t cap = 64 * 1024;
  size_t len = 0;
  uint8_t *buf = (uint8_t *)malloc(cap);
  if (buf == NULL) return MR_ERR_NOMEM;

  for (;;) {
    if (len == cap) {
      if (cap > (SIZE_MAX / 2)) {
        free(buf);
        return MR_ERR_NOMEM;
      }
      size_t grown_cap = cap * 2;
      uint8_t *grown = (uint8_t *)realloc(buf, grown_cap);
      if (grown == NULL) {
        free(buf);
        return MR_ERR_NOMEM;
      }
      buf = grown;
      cap = grown_cap;
    }
    size_t got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0) {
      if (ferror(f)) {
        free(buf);
        return MR_ERR_IO;
      }
      break; /* EOF */
    }
  }

  out->data = buf;
  out->len = len;
  return MR_OK;
}

mr_status mr_read_file(const char *path, mr_bytes *out) {
  if (path == NULL || out == NULL) return MR_ERR_INVALID;
  out->data = NULL;
  out->len = 0;

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    /*
     * Distinguish "not there" from "there and unreadable". Callers act on the
     * difference: an absent profile is a first run and gets defaults, whereas an
     * unreadable one is a permissions problem the user has to be told about.
     * Collapsing both into MR_ERR_IO is how a first run turns into an error
     * dialog.
     */
    return errno == ENOENT ? MR_ERR_NOTFOUND : MR_ERR_IO;
  }
  mr_status st = mr_read_stream(f, out);
  (void)fclose(f);
  if (st != MR_OK) {
    out->data = NULL;
    out->len = 0;
  }
  return st;
}

mr_status mr_write_file_atomic(const char *path, const void *data, size_t len) {
  if (path == NULL || (data == NULL && len != 0)) return MR_ERR_INVALID;

  size_t plen = strlen(path);
  char *tmp = (char *)malloc(plen + 16);
  if (tmp == NULL) return MR_ERR_NOMEM;
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmpXXXXXX", 11); /* includes the terminator */

#if defined(_WIN32)
  FILE *f = fopen(tmp, "wb");
#else
  int fd = mkstemp(tmp);
  FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
#endif
  if (f == NULL) {
    free(tmp);
    return MR_ERR_IO;
  }

  mr_status st = MR_OK;
  if (len != 0 && fwrite(data, 1, len, f) != len) st = MR_ERR_IO;
  if (st == MR_OK && fflush(f) != 0) st = MR_ERR_IO;
#if !defined(_WIN32)
  /* fsync before rename: a profile or cache manifest that survives a crash but
   * contains zeros is worse than one that is simply absent. */
  if (st == MR_OK && fsync(fileno(f)) != 0) st = MR_ERR_IO;
#endif
  if (fclose(f) != 0 && st == MR_OK) st = MR_ERR_IO;

  if (st == MR_OK) {
#if defined(_WIN32)
    (void)MR_UNLINK(path);
#endif
    if (rename(tmp, path) != 0) st = MR_ERR_IO;
  }
  if (st != MR_OK) (void)MR_UNLINK(tmp);
  free(tmp);
  return st;
}

mr_status mr_mkdirs(const char *path) {
  if (path == NULL || path[0] == '\0') return MR_ERR_INVALID;
  size_t len = strlen(path);
  char *scratch = (char *)malloc(len + 1);
  if (scratch == NULL) return MR_ERR_NOMEM;
  memcpy(scratch, path, len + 1);

  mr_status st = MR_OK;
  for (size_t i = 1; i <= len; i++) {
    if (scratch[i] == '/' || scratch[i] == '\0') {
      char saved = scratch[i];
      scratch[i] = '\0';
      if (MR_MKDIR(scratch) != 0 && errno != EEXIST) {
        struct stat stbuf;
        if (stat(scratch, &stbuf) != 0 || !S_ISDIR(stbuf.st_mode)) {
          st = MR_ERR_IO;
          break;
        }
      }
      scratch[i] = saved;
    }
  }
  free(scratch);
  return st;
}

bool mr_path_is_dir(const char *path) {
  if (path == NULL) return false;
  struct stat stbuf;
  return stat(path, &stbuf) == 0 && S_ISDIR(stbuf.st_mode);
}

char *mr_path_join(const char *a, const char *b) {
  if (a == NULL || b == NULL) return NULL;
  size_t alen = strlen(a);
  if (alen == 0) {
    size_t blen = strlen(b) + 1;
    char *only = (char *)malloc(blen);
    if (only != NULL) memcpy(only, b, blen);
    return only;
  }
  bool needs_sep = a[alen - 1] != '/';
  size_t blen = strlen(b);
  char *out = (char *)malloc(alen + (needs_sep ? 1u : 0u) + blen + 1);
  if (out == NULL) return NULL;
  memcpy(out, a, alen);
  size_t off = alen;
  if (needs_sep) out[off++] = '/';
  memcpy(out + off, b, blen + 1);
  return out;
}

bool mr_path_join_into(char *dst, size_t dst_cap, const char *a, const char *b) {
  if (dst == NULL || dst_cap == 0) return false;
  dst[0] = '\0';
  if (a == NULL || b == NULL) return false;

  size_t alen = strlen(a);
  bool needs_sep = alen > 0 && a[alen - 1] != '/';
  size_t blen = strlen(b);
  size_t total = alen + (needs_sep ? 1u : 0u) + blen;

  if (total + 1 > dst_cap) return false;

  size_t off = 0;
  if (alen != 0) {
    memcpy(dst, a, alen);
    off = alen;
    if (needs_sep) dst[off++] = '/';
  }
  memcpy(dst + off, b, blen + 1);
  return true;
}

bool mr_path_component_is_safe(const char *name) {
  if (name == NULL || name[0] == '\0') return false;
  if (name[0] == '.') return false;
  size_t len = strlen(name);
  if (len >= 256) return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    if (c == '/' || c == '\\' || c == ':') return false;
    if ((unsigned char)c < 0x20) return false;
  }
  /* ".." anywhere is rejected outright rather than normalised: a caller passing
   * one is working from a name we do not control, and resolving it would hide the
   * fact that the name was ever trusted. */
  return strstr(name, "..") == NULL;
}

char *mr_path_basename_dup(const char *path) {
  if (path == NULL) return NULL;
  const char *base = path;
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  size_t len = strlen(base);
  char *out = (char *)malloc(len + 1);
  if (out != NULL) memcpy(out, base, len + 1);
  return out;
}

/* ------------------------------------------------------------------ hashing */

uint64_t mr_fnv1a64(const void *data, size_t len) {
  /*
   * The FNV-1a 64-bit offset basis, 0xcbf29ce484222325. It was mistyped here as
   * ...603 for a digit, which is not the basis of any FNV variant; the hash was
   * still deterministic and still grouped identical inputs together, so nothing
   * broke visibly, but the values did not match any other FNV implementation and
   * could not be verified against one.
   */
  return mr_fnv1a64_update(0xcbf29ce484222325ULL, data, len);
}

uint64_t mr_fnv1a64_update(uint64_t seed, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = seed;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

void mr_hex64(uint64_t value, char out[17]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    out[i] = digits[value & 0xFu];
    value >>= 4;
  }
  out[16] = '\0';
}

/* ------------------------------------------------------------------- misc */

bool mr_read_u8_at(const uint8_t *data, size_t len, size_t off, uint8_t *out) {
  if (data == NULL || off >= len) return false;
  *out = data[off];
  return true;
}

bool mr_read_u16(const uint8_t *data, size_t len, size_t off, uint16_t *out) {
  if (data == NULL || off > len || len - off < 2) return false;
  *out = (uint16_t)((uint16_t)data[off] | ((uint16_t)data[off + 1] << 8));
  return true;
}

bool mr_read_u32(const uint8_t *data, size_t len, size_t off, uint32_t *out) {
  if (data == NULL || off > len || len - off < 4) return false;
  *out = (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8) |
         ((uint32_t)data[off + 2] << 16) | ((uint32_t)data[off + 3] << 24);
  return true;
}

bool mr_read_u64(const uint8_t *data, size_t len, size_t off, uint64_t *out) {
  uint32_t lo = 0;
  uint32_t hi = 0;
  if (!mr_read_u32(data, len, off, &lo)) return false;
  if (!mr_read_u32(data, len, off + 4, &hi)) return false;
  *out = (uint64_t)lo | ((uint64_t)hi << 32);
  return true;
}

void mr_read_ascii(const uint8_t *data, size_t len, size_t off, size_t width,
                   char *dst, size_t dst_cap) {
  if (dst == NULL || dst_cap == 0) return;
  dst[0] = '\0';
  if (data == NULL || off >= len) return;

  size_t available = len - off;
  size_t n = width < available ? width : available;
  size_t written = 0;
  for (size_t i = 0; i < n && written + 1 < dst_cap; i++) {
    if (data[off + i] == '\0') break;
    dst[written++] = (char)data[off + i];
  }
  dst[written] = '\0';
}

int mr_strcasecmp(const char *a, const char *b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;
  while (*a != '\0' && *b != '\0') {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return (int)ca - (int)cb;
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

bool mr_str_iequal(const char *a, const char *b) {
  return mr_strcasecmp(a, b) == 0;
}

bool mr_str_ends_with_ci(const char *s, const char *suffix) {
  if (s == NULL || suffix == NULL) return false;
  size_t slen = strlen(s);
  size_t tlen = strlen(suffix);
  if (tlen > slen) return false;
  return mr_strcasecmp(s + (slen - tlen), suffix) == 0;
}
