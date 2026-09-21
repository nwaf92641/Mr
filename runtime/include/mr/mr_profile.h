/*
 * A game profile is the runtime's entire per-title state, and its on-disk shape
 * is the user-facing contract from the brief:
 *
 *   profiles/<game-id>/config         identity, arch, graphics API, runtime
 *   profiles/<game-id>/environment    KEY = VALUE exported into Wine
 *   profiles/<game-id>/dll-overrides  native/builtin DLL substitution
 *   profiles/<game-id>/graphics       backend, feature level, presentation
 *   profiles/<game-id>/performance    caches, threads, synchronisation
 *   profiles/<game-id>/controller     input mapping
 *
 * Every file is the same tiny format -- `key = value`, `#` comments, blank
 * lines ignored -- so it can be written by the app, read by a human, and
 * diffed in a bug report. The profile is the only thing the user ever has to
 * edit, and in practice they should not have to edit it at all.
 */
#ifndef MR_PROFILE_H
#define MR_PROFILE_H

#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MR_PROFILE_ID_MAX 80
#define MR_PROFILE_PATH_MAX 1024
#define MR_PROFILE_VALUE_MAX 512

/* Which profile file a setting belongs to. Also the filename. */
typedef enum {
  MR_SECTION_CONFIG = 0,
  MR_SECTION_ENVIRONMENT,
  MR_SECTION_DLL_OVERRIDES,
  MR_SECTION_GRAPHICS,
  MR_SECTION_PERFORMANCE,
  MR_SECTION_CONTROLLER,
  MR_SECTION_COUNT,
} mr_profile_section;

const char *mr_profile_section_filename(mr_profile_section section);

/* Resolves a section name ("graphics", "dll-overrides") to its enum. Exported
 * because the UI and the CLI both take a section by name on the command line,
 * and two copies of this table would eventually disagree. */
bool mr_profile_section_from_name(const char *name, mr_profile_section *out);

/*
 * A key/value bag that remembers which section each key came from and whether
 * the user set it explicitly. Unknown keys are preserved rather than dropped:
 * a profile written by a newer build must survive a round trip through an older
 * one, otherwise downgrading silently discards settings.
 */
typedef struct {
  char *key;
  char *value;
  mr_profile_section section;
  bool explicit_set; /* came from disk, not from a default */
} mr_profile_entry;

typedef struct {
  char id[MR_PROFILE_ID_MAX];
  char name[160];
  char exe[MR_PROFILE_PATH_MAX];
  char game_dir[MR_PROFILE_PATH_MAX];
  char profile_dir[MR_PROFILE_PATH_MAX]; /* empty => derive from id + root */

  mr_profile_entry *entries;
  size_t entry_count;
  size_t entry_cap;

  mr_strvec dll_native;
  mr_strvec dll_builtin;
} mr_profile;

mr_status mr_profile_init(mr_profile *p);
void mr_profile_free(mr_profile *p);

/*
 * Loads every section file present in `dir`. A missing file is not an error:
 * profiles are built incrementally, and an absent section simply means "keep
 * the default". A file that exists but is malformed IS an error, because
 * silently ignoring a typo'd key is how a user ends up with a setting that
 * looks applied and is not.
 */
mr_status mr_profile_load_dir(const char *dir, mr_profile *out);
mr_status mr_profile_load_file(const char *path, mr_profile_section section,
                               mr_profile *out);
mr_status mr_profile_save_dir(const char *dir, const mr_profile *p);

/* Raw access. */
const char *mr_profile_get(const mr_profile *p, const char *key);
const char *mr_profile_get_or(const mr_profile *p, const char *key,
                              const char *fallback);
mr_status mr_profile_set(mr_profile *p, mr_profile_section section,
                         const char *key, const char *value);

/*
 * True when the *user* set this key, as opposed to a default or a compatibility
 * rule. The compatibility database uses it to avoid overriding a setting the
 * user typed, which is the difference between a helpful default and a profile
 * whose contents do not mean what they say.
 */
bool mr_profile_is_explicit(const mr_profile *p, const char *key);

/* Typed accessors with defaults. An unparsable value yields `fallback` and the
 * caller can detect that with mr_profile_parse_error(). */
int64_t mr_profile_get_int(const mr_profile *p, const char *key,
                           int64_t fallback);
uint64_t mr_profile_get_uint(const mr_profile *p, const char *key,
                             uint64_t fallback);
bool mr_profile_get_bool(const mr_profile *p, const char *key, bool fallback);

/* Set after a failed numeric parse, so the launcher can warn instead of
 * quietly running with a default the user did not ask for. */
const char *mr_profile_parse_error(void);
void mr_profile_clear_parse_error(void);

/* Derived values. */
mr_cpu_path mr_profile_cpu_path(const mr_profile *p);
mr_backend mr_profile_backend(const mr_profile *p);
mr_gfx_api mr_profile_gfx_api(const mr_profile *p);
bool mr_profile_jit_required(const mr_profile *p);

/* 0 when the profile asks for automatic resolution. */
void mr_profile_resolution(const mr_profile *p, int *width, int *height);

/*
 * Filesystem-safe directory name for a game id. Rejects ids containing '/',
 * '\', "..", or a leading '.', which would otherwise let a crafted game id in
 * an imported manifest write outside the profiles root.
 */
bool mr_profile_id_is_safe(const char *id);
mr_status mr_profile_dir_for(const char *profiles_root, const char *game_id,
                             char out[MR_PROFILE_PATH_MAX]);

/*
 * Writes the standard per-title profile layout for a freshly analysed game,
 * filling every section with the values the host and the analyzer agree on.
 * Existing files are left alone unless `overwrite` is set, so re-importing a
 * game does not discard settings the user tuned.
 */
mr_status mr_profile_write_layout(const char *profiles_root, mr_profile *p,
                                  bool overwrite);

#ifdef __cplusplus
}
#endif

#endif /* MR_PROFILE_H */
