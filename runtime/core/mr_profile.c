/*
 * Game profiles.
 *
 * The format is `key = value`, one per line, `#` starts a comment when it is
 * the first non-blank character. That is the whole language. It was chosen over
 * TOML or JSON because the profile is a file a user may have to edit by hand on
 * a device with no editor, and because a diff of two profiles has to be
 * readable in a bug report.
 *
 * Two behaviours are deliberate and worth stating, because the obvious
 * alternative is worse:
 *
 *   Unknown keys are preserved. A profile written by a newer build must survive
 *   a round trip through an older one, otherwise downgrading silently discards
 *   settings the user tuned.
 *
 *   A malformed line is an error, not a warning. Skipping it would let a typo
 *   look like a setting that is applied and is not, which is the single most
 *   expensive class of bug in a runtime whose whole job is configuration.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ format */

static const char *const k_section_filenames[MR_SECTION_COUNT] = {
    "config", "environment", "dll-overrides",
    "graphics", "performance", "controller",
};

const char *mr_profile_section_filename(mr_profile_section section) {
  if (section < 0 || section >= MR_SECTION_COUNT) return "config";
  return k_section_filenames[section];
}

static char g_parse_error[256];

static void mr_set_parse_error(const char *fmt, const char *a, const char *b) {
  snprintf(g_parse_error, sizeof(g_parse_error), fmt, a, b);
}

bool mr_profile_section_from_name(const char *name, mr_profile_section *out) {
  if (name == NULL || out == NULL) return false;
  for (int i = 0; i < MR_SECTION_COUNT; i++) {
    if (mr_str_iequal(name, k_section_filenames[i])) {
      *out = (mr_profile_section)i;
      return true;
    }
  }
  return false;
}

/* The parse error is reported through a thread-local slot rather than an out
 * parameter, because the typed accessors have no room for one and a silent
 * default is the failure mode this exists to prevent. */
static char g_parse_error[256];

const char *mr_profile_parse_error(void) { return g_parse_error; }
void mr_profile_clear_parse_error(void) { g_parse_error[0] = '\0'; }

/* ----------------------------------------------------------------- storage */

mr_status mr_profile_init(mr_profile *p) {
  if (p == NULL) return MR_ERR_INVALID;
  memset(p, 0, sizeof(*p));
  mr_status st = mr_strvec_init(&p->dll_native);
  if (st != MR_OK) return st;
  return mr_strvec_init(&p->dll_builtin);
}

void mr_profile_free(mr_profile *p) {
  if (p == NULL) return;
  for (size_t i = 0; i < p->entry_count; i++) {
    free(p->entries[i].key);
    free(p->entries[i].value);
  }
  free(p->entries);
  p->entries = NULL;
  p->entry_count = 0;
  p->entry_cap = 0;
  mr_strvec_free(&p->dll_native);
  mr_strvec_free(&p->dll_builtin);
}

static mr_profile_entry *mr_profile_find(const mr_profile *p, const char *key) {
  if (p == NULL || key == NULL) return NULL;
  for (size_t i = 0; i < p->entry_count; i++) {
    if (strcmp(p->entries[i].key, key) == 0) return &p->entries[i];
  }
  return NULL;
}

static const mr_profile_entry *mr_profile_find_const(const mr_profile *p,
                                                      const char *key) {
  if (p == NULL || key == NULL) return NULL;
  for (size_t i = 0; i < p->entry_count; i++) {
    if (strcmp(p->entries[i].key, key) == 0) return &p->entries[i];
  }
  return NULL;
}

const char *mr_profile_get(const mr_profile *p, const char *key) {
  const mr_profile_entry *e = mr_profile_find_const(p, key);
  return e != NULL ? e->value : NULL;
}

const char *mr_profile_get_or(const mr_profile *p, const char *key,
                              const char *fallback) {
  const char *v = mr_profile_get(p, key);
  return v != NULL ? v : fallback;
}

bool mr_profile_is_explicit(const mr_profile *p, const char *key) {
  const mr_profile_entry *e = mr_profile_find(p, key);
  return e != NULL && e->explicit_set;
}

mr_status mr_profile_set(mr_profile *p, mr_profile_section section,
                         const char *key, const char *value) {
  if (p == NULL || key == NULL || value == NULL) return MR_ERR_INVALID;

  /*
   * "id" is the one key with a home outside the entry table, because paths are
   * derived from it. Keeping both in step here is what stops the two from
   * disagreeing: a profile whose `id` key said one thing and whose `p->id` said
   * another would write its files to one directory under a name it does not
   * report, and mr_profile_write_layout would reject it as unsafe.
   */
  if (strcmp(key, "id") == 0) {
    if (!mr_profile_id_is_safe(value)) return MR_ERR_INVALID;
    snprintf(p->id, sizeof(p->id), "%s", value);
  }

  mr_profile_entry *existing = mr_profile_find(p, key);
  if (existing != NULL) {
    char *copy = strdup(value);
    if (copy == NULL) return MR_ERR_NOMEM;
    free(existing->value);
    existing->value = copy;
    existing->section = section;
    existing->explicit_set = true;
    return MR_OK;
  }

  if (p->entry_count == p->entry_cap) {
    size_t cap = p->entry_cap == 0 ? 32 : p->entry_cap * 2;
    mr_profile_entry *grown =
        (mr_profile_entry *)realloc(p->entries, cap * sizeof(*grown));
    if (grown == NULL) return MR_ERR_NOMEM;
    p->entries = grown;
    p->entry_cap = cap;
  }

  mr_profile_entry *slot = &p->entries[p->entry_count];
  slot->key = strdup(key);
  slot->value = strdup(value);
  if (slot->key == NULL || slot->value == NULL) {
    free(slot->key);
    free(slot->value);
    return MR_ERR_NOMEM;
  }
  slot->section = section;
  slot->explicit_set = true;
  p->entry_count++;
  return MR_OK;
}

/* --------------------------------------------------------------- parsing */

static char *mr_trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
  return s;
}

/* Strips one layer of matching quotes. A value is only unquoted when both ends
 * match, so a path containing a single apostrophe survives. */
static void mr_unquote(char *s) {
  size_t n = strlen(s);
  if (n < 2) return;
  char first = s[0];
  if ((first != '"' && first != '\'') || s[n - 1] != first) return;
  memmove(s, s + 1, n - 2);
  s[n - 2] = '\0';
}

/* Appends a comma-separated list to a vector, dropping empty fields. */
static void mr_split_append(mr_strvec *v, char *value) {
  char *cursor = value;
  for (;;) {
    char *comma = strchr(cursor, ',');
    if (comma != NULL) *comma = '\0';
    char *item = mr_trim(cursor);
    if (*item != '\0') (void)mr_strvec_push(v, item);
    if (comma == NULL) break;
    cursor = comma + 1;
  }
}

static bool mr_seen_key(const mr_strvec *seen, const char *key) {
  for (size_t i = 0; i < seen->count; i++) {
    if (strcmp(seen->items[i], key) == 0) return true;
  }
  return false;
}

static mr_status mr_profile_parse_line(mr_profile *p,
                                       mr_profile_section section,
                                       char *line, size_t line_number,
                                       const char *path, mr_strvec *seen) {
  char *trimmed = mr_trim(line);
  if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') return MR_OK;

  /* `key += value` appends. Supported only for the list-valued keys, because
   * appending to a scalar is ambiguous and a silent last-writer-wins is how a
   * user ends up unable to change a setting. */
  bool append = false;
  char *eq = strchr(trimmed, '=');
  if (eq == NULL) {
    snprintf(g_parse_error, sizeof(g_parse_error),
             "%s:%zu: no '=' in \"%s\"", path, line_number, trimmed);
    return MR_ERR_PARSE;
  }
  char *key_end = eq;
  if (key_end > trimmed && key_end[-1] == '+') {
    append = true;
    key_end--;
  }

  char saved = *eq;
  *key_end = '\0';
  char *key = mr_trim(trimmed);
  *eq = saved;

  char *value = mr_trim(eq + 1);
  if (*key == '\0') {
    snprintf(g_parse_error, sizeof(g_parse_error), "%s:%zu: empty key", path,
             line_number);
    return MR_ERR_PARSE;
  }

  if (section == MR_SECTION_DLL_OVERRIDES &&
      (mr_str_iequal(key, "native") || mr_str_iequal(key, "builtin"))) {
    mr_unquote(value);
    mr_split_append(mr_str_iequal(key, "native") ? &p->dll_native
                                                 : &p->dll_builtin,
                    value);
    return MR_OK;
  }

  if (append) {
    const char *existing = mr_profile_get(p, key);
    if (existing == NULL) {
      return mr_profile_set(p, section, key, value);
    }
    mr_str joined;
    if (mr_str_init(&joined) != MR_OK) return MR_ERR_NOMEM;
    mr_status st = mr_str_appendz(&joined, existing);
    if (st == MR_OK) st = mr_str_appendz(&joined, ",");
    if (st == MR_OK) st = mr_str_appendz(&joined, value);
    if (st == MR_OK) st = mr_profile_set(p, section, key, joined.data);
    mr_str_free(&joined);
    return st;
  }

  /*
   * A key assigned twice *in this file* is a mistake the user cannot see, so it
   * is an error.
   *
   * The scope is the file, and it used to be the whole profile: the check was
   * `mr_profile_find(p, key) != NULL`, which sees every key any earlier file put
   * there. That made a profile unloadable the moment two files named the same
   * key -- and the round trip failed even with a single file, because the caller
   * seeds `id` before loading `config`, which sets `id` itself. Saving a profile
   * from the UI and launching it is the normal path for this project, and it was
   * broken by a check whose own comment says "in one file".
   *
   * Only a repeated plain assignment counts. `key += value` exists to appear
   * more than once.
   */
  if (section == MR_SECTION_CONFIG && mr_seen_key(seen, key)) {
    snprintf(g_parse_error, sizeof(g_parse_error),
             "%s:%zu: key \"%s\" is set more than once", path, line_number, key);
    return MR_ERR_PARSE;
  }

  mr_unquote(value);
  mr_status st = mr_profile_set(p, section, key, value);
  if (st == MR_OK && section == MR_SECTION_CONFIG) {
    st = mr_strvec_push(seen, key);
  }
  return st;
}

mr_status mr_profile_load_file(const char *path, mr_profile_section section,
                               mr_profile *out) {
  if (path == NULL || out == NULL) return MR_ERR_INVALID;

  mr_bytes bytes;
  mr_status st = mr_read_file(path, &bytes);
  if (st != MR_OK) return st;

  /* A profile over a megabyte is not a profile; refusing keeps a runaway file
   * from being parsed line by line. */
  if (bytes.len > 1024u * 1024u) {
    mr_bytes_free(&bytes);
    snprintf(g_parse_error, sizeof(g_parse_error), "%s: larger than 1 MB", path);
    return MR_ERR_RANGE;
  }

  mr_str line;
  st = mr_str_init(&line);
  if (st != MR_OK) {
    mr_bytes_free(&bytes);
    return st;
  }

  /* Keys assigned in this file, so a duplicate is detected within the file
   * rather than against everything already in `out`. */
  mr_strvec seen;
  st = mr_strvec_init(&seen);
  if (st != MR_OK) {
    mr_str_free(&line);
    mr_bytes_free(&bytes);
    return st;
  }

  size_t line_number = 0;
  size_t start = 0;
  for (size_t i = 0; i <= bytes.len; i++) {
    bool at_end = (i == bytes.len);
    char c = at_end ? '\n' : (char)bytes.data[i];
    if (c != '\n' && c != '\r') {
      if (mr_str_append(&line, &c, 1) != MR_OK) {
        st = MR_ERR_NOMEM;
        break;
      }
      continue;
    }
    if (line.len == 0 && c == '\r') continue;
    (void)start;
    line_number++;
    st = mr_profile_parse_line(out, section, line.data, line_number, path, &seen);
    mr_str_clear(&line);
    if (st != MR_OK) break;
  }

  mr_strvec_free(&seen);
  mr_str_free(&line);
  mr_bytes_free(&bytes);
  return st;
}

mr_status mr_profile_load_dir(const char *dir, mr_profile *out) {
  if (dir == NULL || out == NULL) return MR_ERR_INVALID;

  for (int i = 0; i < MR_SECTION_COUNT; i++) {
    char *path = mr_path_join(dir, k_section_filenames[i]);
    if (path == NULL) return MR_ERR_NOMEM;

    mr_bytes probe;
    mr_status st = mr_read_file(path, &probe);
    if (st == MR_OK) {
      mr_bytes_free(&probe);
      /* Present but unreadable-as-config is an error; absent is not. A profile
       * is built incrementally, so a missing section means "keep the default",
       * while a malformed one means the user's intent was lost. */
      st = mr_profile_load_file(path, (mr_profile_section)i, out);
    } else {
      st = MR_OK;
    }
    free(path);
    if (st != MR_OK) return st;
  }
  return MR_OK;
}

/* --------------------------------------------------------------- saving */

static mr_status mr_profile_write_section(const char *dir,
                                          mr_profile_section section,
                                          const mr_profile *p,
                                          bool overwrite) {
  char *path = mr_path_join(dir, k_section_filenames[section]);
  if (path == NULL) return MR_ERR_NOMEM;

  mr_status st = MR_OK;
  if (!overwrite) {
    mr_bytes probe;
    if (mr_read_file(path, &probe) == MR_OK) {
      mr_bytes_free(&probe);
      free(path);
      return MR_OK; /* keep what the user has */
    }
  }

  mr_str body;
  st = mr_str_init(&body);
  if (st != MR_OK) {
    free(path);
    return st;
  }

  st = mr_str_appendf(&body, "# Mr profile section: %s\n",
                      k_section_filenames[section]);
  if (st == MR_OK) {
    st = mr_str_appendz(&body,
                        "# Generated from the Game Analyzer's output. Edit "
                        "freely; unknown keys are preserved.\n\n");
  }

  if (section == MR_SECTION_DLL_OVERRIDES) {
    if (st == MR_OK && p->dll_native.count > 0) {
      st = mr_str_appendz(&body, "native = ");
      for (size_t i = 0; st == MR_OK && i < p->dll_native.count; i++) {
        if (i > 0) st = mr_str_appendz(&body, ",");
        if (st == MR_OK) st = mr_str_appendz(&body, p->dll_native.items[i]);
      }
      if (st == MR_OK) st = mr_str_appendz(&body, "\n");
    }
    if (st == MR_OK && p->dll_builtin.count > 0) {
      st = mr_str_appendz(&body, "builtin = ");
      for (size_t i = 0; st == MR_OK && i < p->dll_builtin.count; i++) {
        if (i > 0) st = mr_str_appendz(&body, ",");
        if (st == MR_OK) st = mr_str_appendz(&body, p->dll_builtin.items[i]);
      }
      if (st == MR_OK) st = mr_str_appendz(&body, "\n");
    }
  }

  for (size_t i = 0; st == MR_OK && i < p->entry_count; i++) {
    const mr_profile_entry *e = &p->entries[i];
    if (e->section != section) continue;
    st = mr_str_appendf(&body, "%s = %s\n", e->key, e->value);
  }

  if (st == MR_OK) st = mr_write_file_atomic(path, body.data, body.len);
  mr_str_free(&body);
  free(path);
  return st;
}

mr_status mr_profile_save_dir(const char *dir, const mr_profile *p) {
  if (dir == NULL || p == NULL) return MR_ERR_INVALID;

  mr_status st = mr_mkdirs(dir);
  if (st != MR_OK) return st;

  for (int i = 0; i < MR_SECTION_COUNT; i++) {
    st = mr_profile_write_section(dir, (mr_profile_section)i, p, true);
    if (st != MR_OK) return st;
  }
  return MR_OK;
}

mr_status mr_profile_write_layout(const char *profiles_root, mr_profile *p,
                                  bool overwrite) {
  if (profiles_root == NULL || p == NULL) return MR_ERR_INVALID;
  if (!mr_profile_id_is_safe(p->id)) return MR_ERR_INVALID;

  char dir[MR_PROFILE_PATH_MAX];
  mr_status st = mr_profile_dir_for(profiles_root, p->id, dir);
  if (st != MR_OK) return st;

  st = mr_mkdirs(dir);
  if (st != MR_OK) return st;

  snprintf(p->profile_dir, sizeof(p->profile_dir), "%s", dir);

  for (int i = 0; i < MR_SECTION_COUNT; i++) {
    st = mr_profile_write_section(dir, (mr_profile_section)i, p, overwrite);
    if (st != MR_OK) return st;
  }
  return MR_OK;
}

/* ------------------------------------------------------------ accessors */

int64_t mr_profile_get_int(const mr_profile *p, const char *key,
                           int64_t fallback) {
  const char *v = mr_profile_get(p, key);
  if (v == NULL) return fallback;

  mr_profile_clear_parse_error();
  char *end = NULL;
  long long parsed = strtoll(v, &end, 0);
  if (end == v || (end != NULL && *end != '\0')) {
    mr_set_parse_error("value for \"%s\" is not a number: \"%s\"", key, v);
    return fallback;
  }
  return (int64_t)parsed;
}

uint64_t mr_profile_get_uint(const mr_profile *p, const char *key,
                             uint64_t fallback) {
  int64_t v = mr_profile_get_int(p, key, -1);
  if (v < 0) {
    if (mr_profile_get(p, key) == NULL) return fallback;
    return fallback;
  }
  return (uint64_t)v;
}

bool mr_profile_get_bool(const mr_profile *p, const char *key, bool fallback) {
  const char *v = mr_profile_get(p, key);
  if (v == NULL) return fallback;
  if (mr_str_iequal(v, "1") || mr_str_iequal(v, "true") ||
      mr_str_iequal(v, "yes") || mr_str_iequal(v, "on")) {
    return true;
  }
  if (mr_str_iequal(v, "0") || mr_str_iequal(v, "false") ||
      mr_str_iequal(v, "no") || mr_str_iequal(v, "off")) {
    return false;
  }
  mr_set_parse_error("value for \"%s\" is not a boolean: \"%s\"", key, v);
  return fallback;
}

/* ------------------------------------------------------------- derived */

mr_cpu_path mr_profile_cpu_path(const mr_profile *p) {
  const char *arch = mr_profile_get(p, "arch");
  if (arch != NULL && mr_arch_from_str(arch) == MR_ARCH_ARM64) {
    return MR_CPU_PATH_NATIVE;
  }
  return mr_cpu_path_from_str(mr_profile_get_or(p, "cpu_translation", "fex-jit"));
}

mr_backend mr_profile_backend(const mr_profile *p) {
  return mr_backend_from_str(mr_profile_get_or(p, "backend", "metal4"));
}

mr_gfx_api mr_profile_gfx_api(const mr_profile *p) {
  const char *name = mr_profile_get(p, "graphics_api");
  if (name != NULL) return mr_gfx_api_from_str(name);
  return MR_GFX_NONE;
}

bool mr_profile_jit_required(const mr_profile *p) {
  return mr_profile_get_bool(p, "jit", true);
}

void mr_profile_resolution(const mr_profile *p, int *width, int *height) {
  int w = 0;
  int h = 0;
  const char *value = mr_profile_get(p, "resolution");
  if (value != NULL && !mr_str_iequal(value, "auto")) {
    /* WIDTHxHEIGHT, both decimal, both sane. */
    const char *sep = strchr(value, 'x');
    if (sep == NULL) sep = strchr(value, 'X');
    if (sep != NULL) {
      long pw = strtol(value, NULL, 10);
      long ph = strtol(sep + 1, NULL, 10);
      if (pw >= 320 && pw <= 16384 && ph >= 240 && ph <= 16384) {
        w = (int)pw;
        h = (int)ph;
      }
    }
  }
  if (width != NULL) *width = w;
  if (height != NULL) *height = h;
}

/* ------------------------------------------------------------------ paths */

bool mr_profile_id_is_safe(const char *id) {
  /* A game id names a directory under profiles/ and under cache/, so it is a
   * path component and is checked as one. */
  return mr_path_component_is_safe(id) && strlen(id) < MR_PROFILE_ID_MAX;
}

mr_status mr_profile_dir_for(const char *profiles_root, const char *game_id,
                             char out[MR_PROFILE_PATH_MAX]) {
  if (profiles_root == NULL || game_id == NULL || out == NULL) {
    return MR_ERR_INVALID;
  }
  if (!mr_profile_id_is_safe(game_id)) return MR_ERR_INVALID;

  char *joined = mr_path_join(profiles_root, game_id);
  if (joined == NULL) return MR_ERR_NOMEM;
  snprintf(out, MR_PROFILE_PATH_MAX, "%s", joined);
  free(joined);
  return MR_OK;
}
