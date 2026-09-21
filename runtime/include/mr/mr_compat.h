/*
 * The compatibility database.
 *
 * Mr does not claim to run every Windows game, so the runtime has to be able to
 * say which ones it will run and why -- before the user waits through a launch
 * that was never going to work. The database is a text file of ordered rules
 * keyed by game id; the most specific matching rule supplies the verdict, its
 * explanation, and any settings or environment the title needs.
 *
 * Rules are data, not code, because the answer changes as FEX, Wine and DXMT
 * improve. A rule file can be updated without a new build, which is the
 * difference between "Metal 4 support landed, three more titles now run" and
 * "wait for the next release".
 */
#ifndef MR_COMPAT_H
#define MR_COMPAT_H

#include "mr/mr_analyze.h"
#include "mr/mr_host.h"
#include "mr/mr_launch.h"
#include "mr/mr_profile.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MR_COMPAT_UNKNOWN = 0, /* no rule matched: untested, not supported */
  MR_COMPAT_VERIFIED,    /* confirmed working on the stated hardware */
  MR_COMPAT_PLAYABLE,    /* works; the note says what is imperfect */
  MR_COMPAT_RUNS_WITH_ISSUES, /* starts, but a specific thing is broken */
  MR_COMPAT_BROKEN,      /* starts and then fails; the note says how */
  MR_COMPAT_UNSUPPORTED, /* nothing in this release will run it */
} mr_compat_status;

const char *mr_compat_status_str(mr_compat_status status);
mr_compat_status mr_compat_status_from_str(const char *name);

/*
 * The reserved id of the built-in defaults entry. It is not a claim about any
 * title: it carries the settings Mr applies when nothing more specific does.
 * mr_compat_lookup never returns it, and mr_compat_apply always applies it first
 * so that a more specific rule wins.
 */
#define MR_COMPAT_DEFAULTS_ID "mr-defaults"

typedef struct {
  char id[160]; /* game id, a profile id, or a pattern ending in '*' */
  char title[160];
  mr_compat_status status;
  /* The user-facing reason. Written to be read on its own, because it is what
   * the UI shows when it refuses to launch something. */
  char status_note[1024];
  /*
   * What the entry was confirmed against: a Mr version, a device, an OS. Empty
   * for a rule derived from reasoning rather than observation, and the report
   * says which, because an untested guess and a confirmed workaround should not
   * look alike.
   */
  char verified_with[128];

  /* Profile keys to apply, and environment to add, when this rule matches. */
  char **setting_keys;
  char **setting_values;
  size_t setting_count;
  char **env_keys;
  char **env_values;
  size_t env_count;
} mr_compat_rule;

typedef struct {
  mr_compat_rule *rules;
  size_t rule_count;
  size_t rule_capacity;
  char last_error[256];
} mr_compat_db;

mr_status mr_compat_db_init(mr_compat_db *db);
void mr_compat_db_free(mr_compat_db *db);

mr_status mr_compat_add(mr_compat_db *db, const char *id, const char *title,
                        mr_compat_status status, const char *status_note,
                        const char *verified_with);
mr_status mr_compat_add_setting(mr_compat_db *db, const char *id,
                                const char *key, const char *value);
mr_status mr_compat_add_env(mr_compat_db *db, const char *id, const char *key,
                            const char *value);

/*
 * Loads a rules file. The format is a section per title:
 *
 *   [the-game-id]
 *   title = The Game
 *   status = playable
 *   verified_with = Mr 0.1, M1 iPad Air, iPadOS 26.0
 *   setting feature_level = 11_1
 *   env FEX_MAXINST = 5000
 *   note = Compiles shaders slowly on first launch.
 *
 * A malformed line is an error rather than a skipped warning: a rules file that
 * half-loads produces confident wrong verdicts, which is worse than refusing to
 * load at all.
 */
mr_status mr_compat_db_load_file(mr_compat_db *db, const char *path);

/*
 * True when `pattern` matches `game_id`. A trailing '*' matches any suffix; a
 * bare "*" matches everything, which is how the architecture-wide entries are
 * written.
 */
bool mr_compat_id_matches(const char *pattern, const char *game_id);

/* Most specific rule for a game id, or NULL. Later rules win, so a user's own
 * file, loaded after the built-ins, overrides them. */
const mr_compat_rule *mr_compat_lookup(const mr_compat_db *db,
                                       const char *game_id);

/*
 * Applies every matching rule to a profile and a plan: settings become profile
 * keys, environment entries are added to the plan, and an unsupported or broken
 * verdict adds a blocker.
 *
 * A key the user set explicitly in the profile is never overwritten. The profile
 * is the file the user edits; a database entry that silently beat it would make
 * the profile's contents a lie. `explanation`, when given, receives one line per
 * applied setting.
 */
mr_status mr_compat_apply(const mr_compat_db *db, const char *game_id,
                          mr_profile *profile, mr_launch_plan *plan,
                          mr_str *explanation);

bool mr_compat_rule_has_env(const mr_compat_rule *rule, const char *key);
const char *mr_compat_rule_setting(const mr_compat_rule *rule, const char *key);

/* The rules shipped with the runtime, compiled in so the database cannot be
 * missing on a fresh install. */
mr_status mr_compat_db_load_builtins(mr_compat_db *db);

#ifdef __cplusplus
}
#endif

#endif /* MR_COMPAT_H */
