/*
 * The compatibility database.
 *
 * A list of rules, each naming a title and saying what is known about it. The
 * point is to keep hard-won knowledge out of the code: the day someone works out
 * which DXMT option a title needs, the fix should be a line in a text file, not a
 * rebuild.
 *
 * Two decisions shape this file:
 *
 *   Rules carry their verification level and the build they were verified
 *   against. An untested guess and a confirmed workaround are both useful, but
 *   presenting them as the same thing is how a compatibility list becomes
 *   untrustworthy.
 *
 *   A rule can deny. Saying "this cannot work and here is the reason" is more
 *   valuable than silence, because the user stops looking for a configuration
 *   that does not exist.
 *
 * The built-in rules below are not claims about specific retail titles. They are
 * the entries whose reasons follow from the architecture documented in this
 * repository, and each is written so it can be checked against that reasoning.
 * Anything requiring a device to confirm belongs in the user's own rules file.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp, for the id pattern match */

const char *mr_compat_status_str(mr_compat_status status) {
  switch (status) {
    case MR_COMPAT_UNKNOWN: return "unknown";
    case MR_COMPAT_VERIFIED: return "verified";
    case MR_COMPAT_PLAYABLE: return "playable";
    case MR_COMPAT_RUNS_WITH_ISSUES: return "runs-with-issues";
    case MR_COMPAT_BROKEN: return "broken";
    case MR_COMPAT_UNSUPPORTED: return "unsupported";
  }
  return "unknown";
}

mr_compat_status mr_compat_status_from_str(const char *name) {
  if (name == NULL) return MR_COMPAT_UNKNOWN;
  for (int i = 0; i <= (int)MR_COMPAT_UNSUPPORTED; i++) {
    if (mr_str_iequal(name, mr_compat_status_str((mr_compat_status)i))) {
      return (mr_compat_status)i;
    }
  }
  return MR_COMPAT_UNKNOWN;
}

/* ---------------------------------------------------------------- storage */

mr_status mr_compat_db_init(mr_compat_db *db) {
  if (db == NULL) return MR_ERR_INVALID;
  memset(db, 0, sizeof(*db));
  return MR_OK;
}

void mr_compat_db_free(mr_compat_db *db) {
  if (db == NULL) return;
  for (size_t i = 0; i < db->rule_count; i++) {
    mr_compat_rule *rule = &db->rules[i];
    /* The individual strings as well as the arrays that hold them: the arrays
     * are grown with realloc while the content is strdup'd, so releasing only
     * the containers leaks every key, value and note. */
    for (size_t k = 0; k < rule->setting_count; k++) {
      free(rule->setting_keys[k]);
      free(rule->setting_values[k]);
    }
    for (size_t k = 0; k < rule->env_count; k++) {
      free(rule->env_keys[k]);
      free(rule->env_values[k]);
    }
    free(rule->setting_keys);
    free(rule->setting_values);
    free(rule->env_keys);
    free(rule->env_values);
  }
  free(db->rules);
  memset(db, 0, sizeof(*db));
}

static mr_compat_rule *mr_compat_push_rule(mr_compat_db *db) {
  if (db->rule_count == db->rule_capacity) {
    size_t cap = db->rule_capacity == 0 ? 16 : db->rule_capacity * 2;
    mr_compat_rule *grown =
        (mr_compat_rule *)realloc(db->rules, cap * sizeof(*grown));
    if (grown == NULL) return NULL;
    db->rules = grown;
    db->rule_capacity = cap;
  }
  mr_compat_rule *rule = &db->rules[db->rule_count++];
  memset(rule, 0, sizeof(*rule));
  rule->status = MR_COMPAT_UNKNOWN;
  return rule;
}

static mr_status mr_compat_add_pair(char ***keys, char ***values, size_t *count,
                                    const char *key, const char *value) {
  char **grown_keys = (char **)realloc(*keys, (*count + 1) * sizeof(char *));
  if (grown_keys == NULL) return MR_ERR_NOMEM;
  *keys = grown_keys;

  char **grown_values =
      (char **)realloc(*values, (*count + 1) * sizeof(char *));
  if (grown_values == NULL) return MR_ERR_NOMEM;
  *values = grown_values;

  (*keys)[*count] = strdup(key);
  (*values)[*count] = strdup(value);
  if ((*keys)[*count] == NULL || (*values)[*count] == NULL) return MR_ERR_NOMEM;

  (*count)++;
  return MR_OK;
}

mr_status mr_compat_add(mr_compat_db *db, const char *id, const char *title,
                        mr_compat_status status, const char *status_note,
                        const char *verified_with) {
  if (db == NULL || id == NULL) return MR_ERR_INVALID;

  mr_compat_rule *rule = mr_compat_push_rule(db);
  if (rule == NULL) return MR_ERR_NOMEM;

  snprintf(rule->id, sizeof(rule->id), "%s", id);
  snprintf(rule->title, sizeof(rule->title), "%s", title != NULL ? title : "");
  rule->status = status;
  snprintf(rule->status_note, sizeof(rule->status_note), "%s",
           status_note != NULL ? status_note : "");
  snprintf(rule->verified_with, sizeof(rule->verified_with), "%s",
           verified_with != NULL ? verified_with : "");
  return MR_OK;
}

mr_status mr_compat_add_setting(mr_compat_db *db, const char *id,
                                const char *key, const char *value) {
  if (db == NULL || id == NULL || key == NULL || value == NULL) {
    return MR_ERR_INVALID;
  }
  for (size_t i = 0; i < db->rule_count; i++) {
    if (strcmp(db->rules[i].id, id) != 0) continue;
    mr_compat_rule *rule = &db->rules[i];
    return mr_compat_add_pair(&rule->setting_keys, &rule->setting_values,
                              &rule->setting_count, key, value);
  }
  return MR_ERR_NOTFOUND;
}

mr_status mr_compat_add_env(mr_compat_db *db, const char *id, const char *key,
                            const char *value) {
  if (db == NULL || id == NULL || key == NULL || value == NULL) {
    return MR_ERR_INVALID;
  }
  for (size_t i = 0; i < db->rule_count; i++) {
    if (strcmp(db->rules[i].id, id) != 0) continue;
    mr_compat_rule *rule = &db->rules[i];
    return mr_compat_add_pair(&rule->env_keys, &rule->env_values,
                              &rule->env_count, key, value);
  }
  return MR_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ match */

bool mr_compat_id_matches(const char *pattern, const char *game_id) {
  if (pattern == NULL || game_id == NULL) return false;
  if (pattern[0] == '\0') return false;

  /* A bare "*" is the match-everything rule, used for the architecture-wide
   * entries in the built-in list. */
  if (strcmp(pattern, "*") == 0) return true;

  size_t plen = strlen(pattern);
  if (pattern[plen - 1] == '*') {
    /* A trailing '*' is an explicit prefix request. */
    plen--;
    if (plen == 0) return true;
    if (plen > strlen(game_id)) return false;
    return strncasecmp(pattern, game_id, plen) == 0;
  }

  /*
   * No '*', so the id must match exactly.
   *
   * Prefix matching here would be a quiet footgun: a rule written and verified
   * against one title would also fire on its sequel, on its demo, and on any
   * unrelated id that happens to start the same way, and the user would get a
   * confident verdict about a game nobody tested. A wildcard is how a rule asks
   * for the wider behaviour; without one it means the id it names.
   */
  return strcasecmp(pattern, game_id) == 0;
}

const mr_compat_rule *mr_compat_lookup(const mr_compat_db *db,
                                      const char *game_id) {
  if (db == NULL || game_id == NULL) return NULL;

  /* Later rules win: a user's own file is loaded after the built-ins, so a
   * local override takes precedence without needing a mechanism for it. The
   * reserved defaults entry is never a verdict about a title, so it is skipped
   * here even though mr_compat_apply uses it. */
  for (size_t i = db->rule_count; i > 0; i--) {
    const mr_compat_rule *rule = &db->rules[i - 1];
    if (strcmp(rule->id, MR_COMPAT_DEFAULTS_ID) == 0) continue;
    if (mr_compat_id_matches(rule->id, game_id)) return rule;
  }
  return NULL;
}

/* -------------------------------------------------------------- built-ins */

/*
 * The built-in database holds exactly one entry, and it is not a claim about any
 * title.
 *
 * This split is deliberate. Verdicts that follow from a title's *facts* -- it
 * has kernel-level anti-cheat, it is 32-bit, it is Direct3D 12 -- are produced by
 * the planner, which is holding those facts and can explain them in the same
 * breath. Duplicating them here as wildcard rules would mean two places could
 * disagree about whether a title is supported, and the one with less information
 * would sometimes win.
 *
 * What belongs here is tuning whose justification is in the comments below: the
 * settings Mr should use unless a title's own entry or the user's profile says
 * otherwise.
 */
mr_status mr_compat_db_load_builtins(mr_compat_db *db) {
  if (db == NULL) return MR_ERR_INVALID;

  mr_status st = mr_compat_add(
      db, MR_COMPAT_DEFAULTS_ID, "Mr's default settings",
      MR_COMPAT_UNKNOWN,
      "Not a claim about a title: the settings Mr applies when nothing more "
      "specific does.",
      "design");
  if (st != MR_OK) return st;

  /* The two that came from the reference runtime and the two that came from
   * this repository's own reasoning are both listed with their reason, so a
   * later reader can tell which is which. */
  static const struct {
    const char *key;
    const char *value;
  } tuning[] = {
      {"feature_level", "11_0"},
      {"allow_metal4", "true"},
      {"smc_checks", "true"},
      {"tso", "true"},
  };

  for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++) {
    st = mr_compat_add_setting(db, MR_COMPAT_DEFAULTS_ID, tuning[i].key,
                               tuning[i].value);
    if (st != MR_OK) return st;
  }
  return MR_OK;
}

/* ------------------------------------------------------------------ files */

/*
 * The rules file format, deliberately the same shape as a profile:
 *
 *   [game-id]
 *   title = The Game
 *   status = playable
 *   verified_with = Mr 0.1, M1 iPad Air, iPadOS 26.0
 *   setting feature_level = 11_1
 *   setting dxmt.d3d11.defuseFma = True
 *   env FEX_MAXINST = 5000
 *   note = One line of why this is what it is.
 *
 * The `setting` and `env` prefixes keep the three namespaces apart inside one
 * section, so a profile key and an environment variable with the same name
 * cannot be confused.
 */
static mr_status mr_compat_apply_line(mr_compat_db *db, mr_compat_rule *rule,
                                      char *line, size_t line_number,
                                      const char *path) {
  char *trimmed = line;
  while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
  size_t n = strlen(trimmed);
  while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t' ||
                   trimmed[n - 1] == '\r')) {
    trimmed[--n] = '\0';
  }
  if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') return MR_OK;

  if (rule == NULL) {
    /* A key before any section header has no rule to belong to. */
    return MR_OK;
  }

  if (strncmp(trimmed, "setting ", 8) == 0 ||
      strncmp(trimmed, "env ", 4) == 0) {
    bool is_env = trimmed[0] == 'e';
    char *rest = trimmed + (is_env ? 4 : 8);
    char *eq = strchr(rest, '=');
    if (eq == NULL) {
      snprintf(db->last_error, sizeof(db->last_error),
               "%s:%zu: expected 'key = value' after '%s'", path, line_number,
               is_env ? "env" : "setting");
      return MR_ERR_PARSE;
    }
    char saved = *eq;
    *eq = '\0';
    char *key = rest;
    size_t klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
      key[--klen] = '\0';
    }
    *eq = saved;

    char *value = eq + 1;
    while (*value == ' ' || *value == '\t') value++;
    size_t vlen = strlen(value);
    if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') &&
        value[vlen - 1] == value[0]) {
      value[vlen - 1] = '\0';
      value++;
    }

    if (*key == '\0') {
      snprintf(db->last_error, sizeof(db->last_error), "%s:%zu: empty key", path,
               line_number);
      return MR_ERR_PARSE;
    }

    return is_env ? mr_compat_add_pair(&rule->env_keys, &rule->env_values,
                                       &rule->env_count, key, value)
                  : mr_compat_add_pair(&rule->setting_keys,
                                       &rule->setting_values,
                                       &rule->setting_count, key, value);
  }

  char *eq = strchr(trimmed, '=');
  if (eq == NULL) {
    snprintf(db->last_error, sizeof(db->last_error), "%s:%zu: no '=' in \"%s\"",
             path, line_number, trimmed);
    return MR_ERR_PARSE;
  }

  char saved = *eq;
  *eq = '\0';
  char *key = trimmed;
  size_t klen = strlen(key);
  while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
    key[--klen] = '\0';
  }
  *eq = saved;

  char *value = eq + 1;
  while (*value == ' ' || *value == '\t') value++;
  size_t vlen = strlen(value);
  if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') &&
      value[vlen - 1] == value[0]) {
    value[vlen - 1] = '\0';
    value++;
  }

  if (mr_str_iequal(key, "title")) {
    snprintf(rule->title, sizeof(rule->title), "%s", value);
  } else if (mr_str_iequal(key, "status")) {
    rule->status = mr_compat_status_from_str(value);
  } else if (mr_str_iequal(key, "note")) {
    snprintf(rule->status_note, sizeof(rule->status_note), "%s", value);
  } else if (mr_str_iequal(key, "verified_with")) {
    snprintf(rule->verified_with, sizeof(rule->verified_with), "%s", value);
  } else {
    /* An unrecognised key is kept as a setting rather than rejected: the file
     * is also a place to record a title's tunings, and a new key must not need a
     * rebuild to be meaningful to whoever reads it. */
    return mr_compat_add_pair(&rule->setting_keys, &rule->setting_values,
                              &rule->setting_count, key, value);
  }
  return MR_OK;
}

mr_status mr_compat_db_load_file(mr_compat_db *db, const char *path) {
  if (db == NULL || path == NULL) return MR_ERR_INVALID;

  mr_bytes bytes;
  mr_status st = mr_read_file(path, &bytes);
  if (st != MR_OK) return st;

  if (bytes.len > 4u * 1024u * 1024u) {
    mr_bytes_free(&bytes);
    snprintf(db->last_error, sizeof(db->last_error), "%s: larger than 4 MB",
             path);
    return MR_ERR_RANGE;
  }

  mr_str line;
  st = mr_str_init(&line);
  if (st != MR_OK) {
    mr_bytes_free(&bytes);
    return st;
  }

  mr_compat_rule *current = NULL;
  size_t line_number = 0;
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
    line_number++;

    const char *scan = line.data;
    while (*scan == ' ' || *scan == '\t') scan++;
    if (*scan == '[') {
      const char *close = strchr(scan, ']');
      if (close == NULL) {
        snprintf(db->last_error, sizeof(db->last_error),
                 "%s:%zu: section header is not closed", path, line_number);
        st = MR_ERR_PARSE;
        mr_str_clear(&line);
        break;
      }
      char id[160];
      size_t idlen = (size_t)(close - scan - 1);
      if (idlen >= sizeof(id)) idlen = sizeof(id) - 1;
      memcpy(id, scan + 1, idlen);
      id[idlen] = '\0';

      current = mr_compat_push_rule(db);
      if (current == NULL) {
        st = MR_ERR_NOMEM;
        mr_str_clear(&line);
        break;
      }
      snprintf(current->id, sizeof(current->id), "%s", id);
      mr_str_clear(&line);
      continue;
    }

    st = mr_compat_apply_line(db, current, line.data, line_number, path);
    mr_str_clear(&line);
    if (st != MR_OK) break;
  }

  mr_str_free(&line);
  mr_bytes_free(&bytes);
  return st;
}

/* ------------------------------------------------------------ application */

mr_status mr_compat_apply(const mr_compat_db *db, const char *game_id,
                          mr_profile *profile, mr_launch_plan *plan,
                          mr_str *explanation) {
  if (db == NULL || game_id == NULL || profile == NULL) return MR_ERR_INVALID;

  if (explanation != NULL) mr_str_clear(explanation);

  /*
   * Matched rules are applied least specific first, so the most specific one
   * wins. That needs all matches, not just the best one: a title covered by the
   * defaults and by its own entry needs both, and scanning forward gives the
   * ordering for free because the reserved defaults entry is added first and a
   * user's file is loaded last.
   */
  for (size_t i = 0; i < db->rule_count; i++) {
    const mr_compat_rule *r = &db->rules[i];
    bool is_defaults = strcmp(r->id, MR_COMPAT_DEFAULTS_ID) == 0;
    if (!is_defaults && !mr_compat_id_matches(r->id, game_id)) continue;

    for (size_t k = 0; k < r->setting_count; k++) {
      /* An explicit user setting is never overwritten. The profile is the file
       * the user edits; a database entry that silently beat it would make the
       * profile's contents a lie. */
      if (mr_profile_is_explicit(profile, r->setting_keys[k])) {
        if (explanation != NULL) {
          (void)mr_str_appendf(
              explanation, "  %s = %s  (kept: set in the profile)\n",
              r->setting_keys[k], mr_profile_get(profile, r->setting_keys[k]));
        }
        continue;
      }
      mr_status st = mr_profile_set(profile, MR_SECTION_CONFIG,
                                    r->setting_keys[k], r->setting_values[k]);
      if (st != MR_OK) return st;
      if (explanation != NULL) {
        (void)mr_str_appendf(
            explanation, "  %s = %s  (from %s)\n", r->setting_keys[k],
            r->setting_values[k],
            is_defaults ? "Mr's defaults" : r->id);
      }
    }

    if (plan != NULL) {
      for (size_t k = 0; k < r->env_count; k++) {
        (void)mr_plan_add_env(plan, r->env_keys[k], r->env_values[k]);
      }
    }
  }

  /* Only a title-specific verdict can block. The reserved defaults entry is
   * never the reason a title cannot run, and the booleans below would otherwise
   * fire on it. */
  const mr_compat_rule *rule = mr_compat_lookup(db, game_id);
  const char *label = NULL;
  if (rule != NULL) {
    label = rule->title[0] != '\0' ? rule->title : rule->id;
  }

  if (rule != NULL && plan != NULL && rule->status == MR_COMPAT_UNSUPPORTED) {
    (void)mr_plan_add_blocker(plan, "%s: %s", label, rule->status_note);
  } else if (rule != NULL && plan != NULL &&
             rule->status == MR_COMPAT_BROKEN) {
    (void)mr_plan_add_blocker(plan, "%s is known not to work: %s", label,
                              rule->status_note);
  }

  return MR_OK;
}

bool mr_compat_rule_has_env(const mr_compat_rule *rule, const char *key) {
  if (rule == NULL || key == NULL) return false;
  for (size_t i = 0; i < rule->env_count; i++) {
    if (strcmp(rule->env_keys[i], key) == 0) return true;
  }
  return false;
}

const char *mr_compat_rule_setting(const mr_compat_rule *rule, const char *key) {
  if (rule == NULL || key == NULL) return NULL;
  for (size_t i = 0; i < rule->setting_count; i++) {
    if (strcmp(rule->setting_keys[i], key) == 0) return rule->setting_values[i];
  }
  return NULL;
}
