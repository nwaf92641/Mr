/*
 * mrctl: the command-line front end.
 *
 * The graphical front end is the product, but this is the interface the tests
 * drive and the one a bug report quotes. It exists so that every question the
 * runtime can answer is answerable without the UI, which in turn means the UI
 * cannot be the only place a behaviour is implemented.
 *
 * Subcommands are verbs, and each one prints something a person can act on. The
 * rule followed throughout is that a command never says only "failed": it says
 * which layer failed and what the observation was.
 *
 * Exit codes are part of the interface because the tests use them:
 *   0  success
 *   1  the command ran, and the answer is "this cannot run"
 *   2  the command was not understood, or a file was missing
 *   3  an internal failure (out of memory, an unreadable profile)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr/mr_analyze.h"
#include "mr/mr_cache.h"
#include "mr/mr_compat.h"
#include "mr/mr_host.h"
#include "mr/mr_launch.h"
#include "mr/mr_pe.h"
#include "mr/mr_profile.h"
#include "mr/mr_telemetry.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#define EXIT_OK 0
#define EXIT_CANNOT_RUN 1
#define EXIT_USAGE 2
#define EXIT_INTERNAL 3

/* ------------------------------------------------------------------ output */

static void mr_usage(void) {
  fputs(
      "mrctl - inspect, plan and configure Windows titles\n"
      "\n"
      "usage: mrctl <command> [options]\n"
      "\n"
      "commands:\n"
      "  host                     show what this device and process can do\n"
      "  analyze <exe>            identify a title and its dependencies\n"
      "  plan <exe>               analyze, then build a launch plan\n"
      "  profile <exe>            write a profile from the analyzer's output\n"
      "  compat <game-id>         show the compatibility verdict for an id\n"
      "  cache stats <game-id>    report shader and pipeline cache usage\n"
      "  cache clear <game-id>    delete a game's caches\n"
      "  cache sweep              delete caches for ids with no profile\n"
      "\n"
      "options:\n"
      "  --json                   emit machine-readable output\n"
      "  --root <dir>             installation root (default: $MR_ROOT or .)\n"
      "  --profile <dir>          use an existing profile directory\n"
      "  --profiles <dir>         profiles root (default: <root>/profiles)\n"
      "  --compat <file>          load an extra compatibility rules file\n"
      "  --no-compat              skip the built-in compatibility defaults\n"
      "\n"
      "exit codes: 0 ok, 1 cannot run, 2 usage or missing file, 3 internal\n",
      stderr);
}

static void mr_print_status(const char *what, mr_status st) {
  fprintf(stderr, "mrctl: %s: %s\n", what, mr_status_str(st));
}

/*
 * Reports a profile load failure with the parser's own file:line detail.
 *
 * mr_profile_parse_line already composes a precise message -- which file, which
 * line, what was wrong with it -- and the CLI printed only "malformed input",
 * which names neither the file nor the line. The user is the one who has to fix
 * the profile, so the file and the line are the whole point of the message.
 */
static void mr_print_profile_error(const char *what, mr_status st) {
  const char *detail = mr_profile_parse_error();
  if (detail != NULL && detail[0] != '\0') {
    fprintf(stderr, "mrctl: %s: %s: %s\n", what, mr_status_str(st), detail);
  } else {
    mr_print_status(what, st);
  }
}

/*
 * The exit code has to say whose problem it is.
 *
 * A file that is not a PE, or one that stops in the middle, is a bad input: the
 * user can act on it. Reporting those as EXIT_INTERNAL told the user the runtime
 * had broken when in fact the download had, and it is the difference between
 * "install this again" and a bug report.
 */
static int mr_exit_for_status(mr_status st) {
  switch (st) {
    case MR_OK:
      return EXIT_OK;
    case MR_ERR_NOTFOUND:
    case MR_ERR_PARSE:
    case MR_ERR_TRUNCATED:
    case MR_ERR_UNSUPPORTED:
    case MR_ERR_RANGE:
      return EXIT_USAGE;
    case MR_ERR_EXISTS:
    case MR_ERR_STATE:
      return EXIT_CANNOT_RUN;
    default:
      return EXIT_INTERNAL;
  }
}

/* ------------------------------------------------------------------- host */

static int mr_cmd_host(const mr_runtime_layout *layout, bool json) {
  mr_host_caps caps;
  mr_host_probe(&caps);

  mr_str missing;
  if (mr_str_init(&missing) != MR_OK) return EXIT_INTERNAL;
  /*
   * The layout was already probed by main, so the missing list is rebuilt from
   * those flags rather than by walking the filesystem again. Probing is the only
   * thing here that touches disk, and doing it twice is a way for the two
   * answers to differ.
   */
  mr_status st = MR_OK;
  if (!layout->wine_present) (void)mr_str_appendz(&missing, "Wine");
  if (!layout->fex_present) {
    if (missing.len > 0) (void)mr_str_appendz(&missing, ", ");
    (void)mr_str_appendz(&missing, "FEX-Emu");
  }
  if (!layout->dxmt_present) {
    if (missing.len > 0) (void)mr_str_appendz(&missing, ", ");
    (void)mr_str_appendz(&missing, "DXMT");
  }

  if (json) {
    mr_str out;
    if (mr_str_init(&out) != MR_OK) {
      mr_str_free(&missing);
      return EXIT_INTERNAL;
    }
    (void)mr_str_appendf(&out,
                         "{\n  \"platform\": \"%s\",\n  \"arch\": \"%s\",\n"
                         "  \"metal4\": %s,\n  \"metal3\": %s,\n"
                         "  \"gpu\": %s,\n  \"jit_capable\": %s,\n"
                         "  \"jit_enabled\": %s,\n  \"jit_hole_mb\": %llu,\n"
                         "  \"memory_mb\": %llu,\n  \"memory_budget_mb\": %llu,\n"
                         "  \"wine\": %s,\n  \"fex\": %s,\n  \"dxmt\": %s\n}\n",
                         mr_platform_str(caps.platform), caps.arch_name,
                         caps.metal4_available ? "true" : "false",
                         caps.metal3_available ? "true" : "false",
                         caps.gpu_available ? "true" : "false",
                         caps.jit_capable ? "true" : "false",
                         caps.jit_enabled ? "true" : "false",
                         (unsigned long long)(caps.jit_hole_bytes / (1024ull * 1024ull)),
                         (unsigned long long)(caps.physical_memory / (1024ull * 1024ull)),
                         (unsigned long long)(caps.memory_budget / (1024ull * 1024ull)),
                         layout->wine_present ? "true" : "false",
                         layout->fex_present ? "true" : "false",
                         layout->dxmt_present ? "true" : "false");
    fputs(out.data, stdout);
    mr_str_free(&out);
    mr_str_free(&missing);
    return EXIT_OK;
  }

  printf("platform:      %s\n", mr_platform_str(caps.platform));
  printf("cpu:           %s\n", caps.arch_name);
  printf("device:        %s\n", caps.device_name);
  printf("os:            %s\n", caps.os_version);
  printf("unified memory: %s\n", caps.unified_memory ? "yes" : "no");
  printf("metal:         %s\n",
         caps.metal4_available ? "Metal 4 available"
                               : (caps.metal3_available ? "Metal 3 only"
                                                        : "unavailable"));
  printf("gpu visible:   %s\n", caps.gpu_available ? "yes" : "no");
  /*
   * JIT is reported as two separate facts because they fail differently and the
   * second is the one that matters. Entitlement without a debugger attached on
   * iPadOS gives jit_capable = true and jit_enabled = false, and the resulting
   * failure upstream is a code-cache placement error a long way from the cause.
   */
  printf("jit capable:   %s (entitlement and W^X probe)\n",
         caps.jit_capable ? "yes" : "no");
  printf("jit enabled:   %s\n", caps.jit_enabled ? "yes" : "no");
  printf("jit hole:      %llu MB contiguous below the guest window\n",
         (unsigned long long)(caps.jit_hole_bytes / (1024ull * 1024ull)));
  printf("memory:        %llu MB total, %llu MB budget\n",
         (unsigned long long)(caps.physical_memory / (1024ull * 1024ull)),
         (unsigned long long)(caps.memory_budget / (1024ull * 1024ull)));
  printf("free disk:     %llu MB\n",
         (unsigned long long)(caps.free_disk_bytes / (1024ull * 1024ull)));
  printf("components:    wine %s, fex %s, dxmt %s\n",
         layout->wine_present ? "ok" : "MISSING",
         layout->fex_present ? "ok" : "MISSING",
         layout->dxmt_present ? "ok" : "MISSING");
  if (missing.len > 0) printf("missing:       %s\n", missing.data);
  printf("jit pool:      %llu MB would be allocated\n",
         (unsigned long long)(mr_host_derive_jit_pool_bytes(
                                  &caps, mr_host_recommended_jit_pool_bytes(&caps),
                                  caps.jit_hole_bytes, 0) /
                              (1024ull * 1024ull)));

  mr_str_free(&missing);
  return st == MR_OK ? EXIT_OK : EXIT_INTERNAL;
}

/* ---------------------------------------------------------------- analyze */

static void mr_print_facts(const mr_game_facts *f) {
  printf("file:          %s\n", f->exe_path);
  printf("name:          %s\n", f->exe_name);
  printf("directory:     %s\n", f->game_dir);
  printf("game id:       ");
  char id[80];
  mr_analyze_game_id(f, id);
  printf("%s\n", id);

  printf("architecture:  %s", mr_arch_str(f->arch));
  if (f->is_wow64) printf(" (32-bit)");
  if (f->pe.is_arm64ec) printf(" (ARM64EC code present)");
  printf("\n");
  printf("machine word:  0x%04X\n", (unsigned)f->pe.machine);
  printf("subsystem:     %s\n", f->pe.subsystem == 3 ? "console" : "windows GUI");
  printf("image size:    %u bytes\n", (unsigned)f->pe.size_of_image);
  printf("entry point:   0x%08X\n", (unsigned)f->pe.entry_point_rva);

  printf("graphics:      %s (confidence: %s)\n", mr_gfx_api_str(f->gfx),
         mr_confidence_str(f->gfx_confidence));
  if (f->gfx_modules.items.count > 0) {
    printf("gfx modules:  ");
    for (size_t i = 0; i < f->gfx_modules.items.count; i++) {
      printf("%s%s", i > 0 ? ", " : "", f->gfx_modules.items.items[i]);
    }
    printf("\n");
  }

  printf("dxbc blobs:    %u\n", (unsigned)f->pe.dxbc_blob_count);
  printf("dxil blobs:    %u\n", (unsigned)f->pe.dxil_blob_count);
  printf("engine:        %s\n", mr_engine_str(f->engine));
  printf("anti-cheat:    %s\n", mr_anticheat_str(f->anticheat));
  printf("drm:           %s\n", mr_drm_str(f->drm));

  printf("managed .net:  %s", f->is_managed_net ? "yes" : "no");
  if (f->net_frame_versions.items.count > 0) {
    printf(" (%s)", f->net_frame_versions.items.items[0]);
  }
  printf("\n");

  printf("vc runtime:    ");
  if (f->vc_runtime_modules.items.count == 0) {
    printf("none detected\n");
  } else {
    for (size_t i = 0; i < f->vc_runtime_modules.items.count; i++) {
      printf("%s%s", i > 0 ? "; " : "", f->vc_runtime_modules.items.items[i]);
    }
    printf("\n");
  }

  printf("input:         ");
  {
    bool any = false;
    struct { bool on; const char *name; } modes[] = {
        {f->input.wants_xinput, "xinput"},
        {f->input.wants_directinput, "directinput"},
        {f->input.wants_raw_input, "raw-input"},
        {f->input.wants_keyboard_mouse, "keyboard-mouse"},
        {f->input.ships_sdl, "sdl"},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
      if (!modes[i].on) continue;
      printf("%s%s", any ? ", " : "", modes[i].name);
      any = true;
    }
    if (!any) printf("none detected");
    printf("\n");
  }

  if (f->default_width > 0) {
    printf("default res:   %dx%d (from a settings file)\n", f->default_width,
           f->default_height);
  }
  if (f->pe.section_count > 0) {
    printf("sections:      ");
    for (uint32_t i = 0; i < f->pe.section_count; i++) {
      printf("%s%s", i > 0 ? "," : "", f->pe.sections[i].name);
    }
    printf("\n");
  }
  printf("packer:        %s\n",
         f->packer_sections.items.count > 0
             ? f->packer_sections.items.items[0]
             : "none detected");
  printf("looks like an installer: %s\n", f->looks_like_setup ? "yes" : "no");
  printf("runnable here: %s\n", mr_analyze_is_runnable(f) ? "yes" : "no");

  if (f->pe.imported_modules.count > 0) {
    printf("imports (%zu):\n", f->pe.imported_modules.count);
    for (size_t i = 0; i < f->pe.imported_modules.count; i++) {
      printf("  %s\n", f->pe.imported_modules.items[i]);
    }
  }

  printf("evidence (%zu):\n", f->evidence_count);
  for (size_t i = 0; i < f->evidence_count; i++) {
    printf("  [%-8s] %s\n            %s\n",
           mr_confidence_str(f->evidence[i].confidence), f->evidence[i].claim,
           f->evidence[i].evidence);
  }
}

static mr_status mr_facts_for(const char *exe, mr_game_facts *facts) {
  mr_status st = mr_analyze_init(facts);
  if (st != MR_OK) return st;
  st = mr_analyze_exe(exe, facts);
  if (st != MR_OK) mr_analyze_free(facts);
  return st;
}

static int mr_cmd_analyze(const char *exe, bool json) {
  mr_game_facts facts;
  mr_status st = mr_facts_for(exe, &facts);
  if (st != MR_OK) {
    mr_print_status(exe, st);
    return mr_exit_for_status(st);
  }

  if (json) {
    mr_str out;
    if (mr_str_init(&out) != MR_OK) {
      mr_analyze_free(&facts);
      return EXIT_INTERNAL;
    }
    char id[80];
    mr_analyze_game_id(&facts, id);
    (void)mr_str_appendf(
        &out,
        "{\n  \"game_id\": \"%s\",\n  \"exe\": \"%s\",\n"
        "  \"arch\": \"%s\",\n  \"machine\": %u,\n  \"gfx\": \"%s\",\n"
        "  \"gfx_confidence\": \"%s\",\n  \"engine\": \"%s\",\n"
        "  \"anticheat\": \"%s\",\n  \"drm\": \"%s\",\n"
        "  \"managed\": %s,\n  \"runnable\": %s,\n"
        "  \"imports\": %zu,\n  \"dxbc_blobs\": %u,\n  \"dxil_blobs\": %u\n}\n",
        id, facts.exe_name, mr_arch_str(facts.arch), (unsigned)facts.pe.machine,
        mr_gfx_api_str(facts.gfx), mr_confidence_str(facts.gfx_confidence),
        mr_engine_str(facts.engine), mr_anticheat_str(facts.anticheat),
        mr_drm_str(facts.drm), facts.is_managed_net ? "true" : "false",
        mr_analyze_is_runnable(&facts) ? "true" : "false",
        facts.pe.imported_modules.count, (unsigned)facts.pe.dxbc_blob_count,
        (unsigned)facts.pe.dxil_blob_count);
    fputs(out.data, stdout);
    mr_str_free(&out);
  } else {
    mr_print_facts(&facts);
  }

  bool runnable = mr_analyze_is_runnable(&facts);
  mr_analyze_free(&facts);
  return runnable ? EXIT_OK : EXIT_CANNOT_RUN;
}

/* ------------------------------------------------------------------- plan */

/*
 * Loads a profile for a game id. A missing profile is not an error: a plan has
 * to be buildable from the analyzer's output alone, because that is exactly what
 * happens on the first import, before any profile exists.
 */
static mr_status mr_profile_for_game(const char *profiles_root,
                                     const char *game_id,
                                     const char *profile_dir_override,
                                     mr_profile *out) {
  mr_status st = mr_profile_init(out);
  if (st != MR_OK) return st;

  st = mr_profile_set(out, MR_SECTION_CONFIG, "id", game_id);
  if (st != MR_OK) return st;

  char dir[MR_PROFILE_PATH_MAX];
  if (profile_dir_override != NULL) {
    snprintf(dir, sizeof(dir), "%s", profile_dir_override);
  } else {
    st = mr_profile_dir_for(profiles_root, game_id, dir);
    if (st != MR_OK) return st;
  }

  if (!mr_path_is_dir(dir)) return MR_OK;

  snprintf(out->profile_dir, sizeof(out->profile_dir), "%s", dir);
  return mr_profile_load_dir(dir, out);
}

static mr_status mr_load_compat(const char *extra_file, bool use_builtins,
                               mr_compat_db *db) {
  mr_status st = mr_compat_db_init(db);
  if (st != MR_OK) return st;

  if (use_builtins) {
    st = mr_compat_db_load_builtins(db);
    if (st != MR_OK) return st;
  }

  if (extra_file != NULL) {
    st = mr_compat_db_load_file(db, extra_file);
    if (st != MR_OK) {
      fprintf(stderr, "compat: %s: %s\n", extra_file, db->last_error);
      return st;
    }
  }
  return MR_OK;
}

/*
 * Builds the plan, applies the compatibility database, and rebuilds.
 *
 * The two-pass shape is deliberate. The database can add settings to the
 * profile, and settings change the plan (feature level, backend, whether
 * translation is needed at all). Applying it to a profile that has already been
 * planned around would produce a plan that describes a configuration the runtime
 * is not going to use -- the kind of bug that only shows up as "the setting has
 * no effect".
 *
 * Blockers from the first pass are carried over: the compatibility verdict is
 * added during the apply and a rebuild would otherwise lose it.
 */
static mr_status mr_build_final_plan(const mr_game_facts *facts,
                                    const mr_host_caps *host,
                                    const mr_runtime_layout *layout,
                                    const char *game_id,
                                    const mr_compat_db *compat,
                                    mr_profile *profile, mr_launch_plan *out,
                                    mr_str *notes) {
  mr_launch_plan first;
  mr_status st = mr_plan_build(facts, profile, host, layout, &first);
  if (st != MR_OK) return st;

  st = mr_compat_apply(compat, game_id, profile, &first, notes);
  if (st != MR_OK) {
    mr_plan_free(&first);
    return st;
  }

  bool need_rebuild = false;
  for (size_t i = 0; i < profile->entry_count; i++) {
    const mr_profile_entry *e = &profile->entries[i];
    if (strcmp(e->key, "feature_level") == 0 ||
        strcmp(e->key, "backend") == 0 ||
        strcmp(e->key, "allow_metal4") == 0 ||
        strcmp(e->key, "cpu_translation") == 0 ||
        strcmp(e->key, "arch") == 0) {
      need_rebuild = true;
      break;
    }
  }
  if (!need_rebuild) {
    *out = first;
    return MR_OK;
  }

  mr_launch_plan second;
  st = mr_plan_build(facts, profile, host, layout, &second);
  if (st != MR_OK) {
    *out = first;
    return MR_OK;
  }

  for (size_t i = 0; i < first.blockers.count; i++) {
    if (!mr_strvec_contains(&second.blockers, first.blockers.items[i])) {
      (void)mr_strvec_push(&second.blockers, first.blockers.items[i]);
    }
  }
  mr_plan_free(&first);
  *out = second;
  return MR_OK;
}

static int mr_cmd_plan(const char *exe, const char *profiles_root,
                       const char *profile_override, const char *compat_file,
                       bool use_builtins, bool json, bool save_profile) {
  mr_game_facts facts;
  mr_status st = mr_facts_for(exe, &facts);
  if (st != MR_OK) {
    mr_print_status(exe, st);
    return mr_exit_for_status(st);
  }

  char game_id[80];
  mr_analyze_game_id(&facts, game_id);

  mr_profile profile;
  st = mr_profile_for_game(profiles_root, game_id, profile_override, &profile);
  if (st != MR_OK) {
    mr_print_profile_error("profile", st);
    mr_analyze_free(&facts);
    return mr_exit_for_status(st);
  }
  /* The analyzed executable wins over whatever the profile recorded: the user
   * may have re-imported a different build of the same title. */
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "exe", facts.exe_path);

  mr_host_caps host;
  mr_host_probe(&host);

  mr_runtime_layout layout;
  mr_layout_init(&layout, profiles_root);
  (void)mr_layout_probe(&layout, NULL);

  mr_compat_db compat;
  st = mr_load_compat(compat_file, use_builtins, &compat);
  if (st != MR_OK) {
    mr_profile_free(&profile);
    mr_analyze_free(&facts);
    return EXIT_INTERNAL;
  }

  mr_str notes;
  mr_launch_plan plan;
  if (mr_str_init(&notes) != MR_OK) {
    mr_compat_db_free(&compat);
    mr_profile_free(&profile);
    mr_analyze_free(&facts);
    return EXIT_INTERNAL;
  }

  st = mr_build_final_plan(&facts, &host, &layout, game_id, &compat, &profile,
                           &plan, &notes);
  if (st != MR_OK) {
    mr_print_status("plan", st);
    mr_str_free(&notes);
    mr_compat_db_free(&compat);
    mr_profile_free(&profile);
    mr_analyze_free(&facts);
    return EXIT_INTERNAL;
  }

  const mr_compat_rule *rule = mr_compat_lookup(&compat, game_id);

  if (json) {
    mr_str out;
    if (mr_str_init(&out) == MR_OK) {
      (void)mr_plan_to_json(&plan, &out);
      fputs(out.data, stdout);
      mr_str_free(&out);
    }
  } else {
    mr_str out;
    if (mr_str_init(&out) == MR_OK) {
      (void)mr_plan_to_text(&plan, &out);
      if (rule != NULL) {
        (void)mr_str_appendf(
            &out, "\ncompatibility: %s (verified: %s)\n  %s\n",
            mr_compat_status_str(rule->status),
            rule->verified_with[0] != '\0' ? rule->verified_with : "unstated",
            rule->status_note);
      } else {
        (void)mr_str_appendz(
            &out,
            "\ncompatibility: unknown. No rule matched this game id, which "
            "means it is untested rather than supported.\n");
      }
      if (notes.len > 0) {
        (void)mr_str_appendz(&out, "\nsettings applied from the database:\n");
        (void)mr_str_append(&out, notes.data, notes.len);
      }
      fputs(out.data, stdout);
      mr_str_free(&out);
    }
  }

  if (save_profile && profile.profile_dir[0] != '\0') {
    mr_status saved = mr_profile_save_dir(profile.profile_dir, &profile);
    if (saved != MR_OK) {
      fprintf(stderr, "mrctl: could not write the profile: %s\n",
              mr_status_str(saved));
    } else {
      fprintf(stderr, "mrctl: profile written to %s\n", profile.profile_dir);
    }
  }

  int code = plan.blockers.count > 0 ? EXIT_CANNOT_RUN : EXIT_OK;
  mr_str_free(&notes);
  mr_plan_free(&plan);
  mr_compat_db_free(&compat);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  return code;
}

/* ---------------------------------------------------------------- profile */

static int mr_cmd_profile(const char *exe, const char *profiles_root) {
  mr_game_facts facts;
  mr_status st = mr_facts_for(exe, &facts);
  if (st != MR_OK) {
    mr_print_status(exe, st);
    return mr_exit_for_status(st);
  }

  char game_id[80];
  mr_analyze_game_id(&facts, game_id);

  mr_profile profile;
  st = mr_profile_init(&profile);
  if (st != MR_OK) {
    mr_analyze_free(&facts);
    return EXIT_INTERNAL;
  }

  /*
   * Every value below comes from the analyzer and is written down rather than
   * left implicit, so the user can see it and change it. A setting the user
   * cannot find is a setting the user cannot fix.
   */
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "id", game_id);
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "name", facts.exe_name);
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "exe", facts.exe_path);
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "arch",
                       mr_arch_str(facts.arch));
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "graphics_api",
                       mr_gfx_api_str(facts.gfx));
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "engine",
                       mr_engine_str(facts.engine));
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "graphics_translation",
                       "dxmt");
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "backend", "metal4");
  (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "resolution", "auto");

  if (facts.arch == MR_ARCH_ARM64 || facts.arch == MR_ARCH_ARM64X) {
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "cpu_translation",
                         "none");
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "jit", "false");
  } else {
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "cpu_translation",
                         "fex-jit");
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "jit", "true");
  }

  /* A resolution found in the title's own settings is worth recording, because
   * it is the size the title is about to ask for, and a profile that disagrees
   * silently produces a letterbox rather than an error. */
  if (facts.default_width > 0 && facts.default_height > 0) {
    char res[32];
    snprintf(res, sizeof(res), "%dx%d", facts.default_width,
             facts.default_height);
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "resolution", res);
  }

  if (facts.anticheat != MR_AC_NONE) {
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "anticheat",
                         mr_anticheat_str(facts.anticheat));
  }
  if (facts.drm != MR_DRM_NONE) {
    (void)mr_profile_set(&profile, MR_SECTION_CONFIG, "drm",
                         mr_drm_str(facts.drm));
  }

  (void)mr_profile_set(&profile, MR_SECTION_CONTROLLER, "enabled",
                       facts.input.wants_xinput ? "true" : "false");
  if (facts.input.wants_xinput) {
    (void)mr_profile_set(&profile, MR_SECTION_CONTROLLER, "api", "xinput");
  } else if (facts.input.wants_directinput) {
    (void)mr_profile_set(&profile, MR_SECTION_CONTROLLER, "api", "directinput");
  }

  /* Existing files are kept: the profile is the user's, and re-running the
   * analyzer must not discard settings they added. */
  st = mr_profile_write_layout(profiles_root, &profile, false);
  if (st != MR_OK) {
    mr_print_status("profile", st);
    mr_profile_free(&profile);
    mr_analyze_free(&facts);
    return EXIT_INTERNAL;
  }

  printf("profile: %s\n", profile.profile_dir);
  printf("game id: %s\n", profile.id);
  if (facts.vc_runtime_modules.items.count > 0) {
    printf(
        "\nnote: this title needs a Visual C++ runtime (%s).\n"
        "      Mr does not redistribute Microsoft's runtime DLLs; install them\n"
        "      into the prefix or the launch will fail on a missing module.\n",
        facts.vc_runtime_modules.items.items[0]);
  }

  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  return EXIT_OK;
}

/* ----------------------------------------------------------------- compat */

static int mr_cmd_compat(const char *game_id, const char *compat_file,
                         bool use_builtins, bool json) {
  mr_compat_db db;
  mr_status st = mr_load_compat(compat_file, use_builtins, &db);
  if (st != MR_OK) return EXIT_INTERNAL;

  const mr_compat_rule *rule = mr_compat_lookup(&db, game_id);
  int code = EXIT_OK;

  if (json) {
    if (rule == NULL) {
      printf("{\n  \"game_id\": \"%s\",\n  \"status\": \"unknown\",\n"
             "  \"note\": \"no rule matched\"\n}\n",
             game_id);
    } else {
      printf("{\n  \"game_id\": \"%s\",\n  \"rule\": \"%s\",\n"
             "  \"title\": \"%s\",\n  \"status\": \"%s\",\n"
             "  \"verified_with\": \"%s\",\n  \"note\": \"%s\"\n}\n",
             game_id, rule->id, rule->title, mr_compat_status_str(rule->status),
             rule->verified_with, rule->status_note);
    }
  } else if (rule == NULL) {
    printf("game id: %s\nstatus:  unknown\n\n", game_id);
    printf(
        "No rule matched. That means untested, not supported: nothing here has\n"
        "observed this title running, and no configuration in this release is\n"
        "known to make it work.\n");
  } else {
    printf("game id:  %s\nrule:     %s\ntitle:    %s\nstatus:   %s\n",
           game_id, rule->id, rule->title, mr_compat_status_str(rule->status));
    printf("verified: %s\n\n%s\n",
           rule->verified_with[0] != '\0' ? rule->verified_with
                                          : "nothing (derived from reasoning)",
           rule->status_note);
    if (rule->setting_count > 0) {
      printf("\nsettings:\n");
      for (size_t i = 0; i < rule->setting_count; i++) {
        printf("  %s = %s\n", rule->setting_keys[i], rule->setting_values[i]);
      }
    }
    if (rule->env_count > 0) {
      printf("\nenvironment:\n");
      for (size_t i = 0; i < rule->env_count; i++) {
        printf("  %s = %s\n", rule->env_keys[i], rule->env_values[i]);
      }
    }
    if (rule->status == MR_COMPAT_UNSUPPORTED ||
        rule->status == MR_COMPAT_BROKEN) {
      code = EXIT_CANNOT_RUN;
    }
  }

  mr_compat_db_free(&db);
  return code;
}

/* ------------------------------------------------------------------ cache */

static int mr_cmd_cache(const char *root, const char *action,
                        const char *game_id, bool json) {
  if (action == NULL) {
    fputs("mrctl: cache needs an action: stats, clear or sweep\n", stderr);
    return EXIT_USAGE;
  }

  char cache_root[1024];
  if (!mr_path_join_into(cache_root, sizeof(cache_root), root, "cache")) {
    return EXIT_INTERNAL;
  }

  if (strcmp(action, "sweep") == 0) {
    /* Sweeping needs no game id: it finds cache directories whose game id no
     * longer has a profile, which is what removing a title leaves behind. */
    char profiles_root[1024];
    if (!mr_path_join_into(profiles_root, sizeof(profiles_root), root,
                           "profiles")) {
      return EXIT_INTERNAL;
    }

    uint64_t reclaimed = 0;
    /* A missing cache root means nothing to sweep, which is a success, not an
     * error: it is the state of a fresh install. */
    mr_status st =
        mr_shader_cache_sweep_orphans(cache_root, profiles_root, &reclaimed);
    if (st != MR_OK && st != MR_ERR_NOTFOUND) {
      mr_print_status("cache sweep", st);
      return EXIT_INTERNAL;
    }
    if (json) {
      printf("{\n  \"reclaimed_bytes\": %llu\n}\n",
             (unsigned long long)reclaimed);
    } else {
      printf("swept orphaned caches: %llu MB reclaimed\n",
             (unsigned long long)(reclaimed / (1024ull * 1024ull)));
    }
    return EXIT_OK;
  }

  if (game_id == NULL) {
    fputs("mrctl: cache needs a game id\n", stderr);
    return EXIT_USAGE;
  }

  /*
   * The backend signature is part of every cache key, so an entry written by one
   * backend can never be read by another. "unbound" here means this command is
   * reporting on the whole cache directory rather than on one backend's view of
   * it; the runtime substitutes the real signature at launch.
   */
  mr_shader_cache cache;
  mr_status st = mr_shader_cache_open(&cache, cache_root, game_id, "unbound",
                                      512ull * 1024ull * 1024ull);
  if (st != MR_OK) {
    mr_print_status("cache", st);
    return EXIT_INTERNAL;
  }

  if (strcmp(action, "stats") == 0) {
    if (json) {
      printf("{\n  \"game_id\": \"%s\",\n  \"current_bytes\": %llu,\n"
             "  \"max_bytes\": %llu\n}\n",
             game_id, (unsigned long long)cache.current_bytes,
             (unsigned long long)cache.max_bytes);
    } else {
      printf("game id:    %s\n", game_id);
      printf("cache root: %s\n", cache.root);
      printf("occupancy:  %llu MB of %llu MB\n",
             (unsigned long long)(cache.current_bytes / (1024ull * 1024ull)),
             (unsigned long long)(cache.max_bytes / (1024ull * 1024ull)));
      /*
       * Per kind, the only number this process actually knows is how many
       * payloads failed their checksum. The line here used to read "entries are
       * reported above", which pointed at nothing: occupancy is tracked for the
       * game's cache as a whole, not split by kind, and hits and misses belong to
       * the process that did the looking rather than to the directory on disk.
       */
      for (int k = 0; k < MR_CACHE_KIND_COUNT; k++) {
        const mr_cache_stats *s = &cache.stats[k];
        printf("  %-9s rejected as corrupt: %llu\n",
               mr_cache_kind_str((mr_cache_kind)k),
               (unsigned long long)s->rejected_corrupt);
      }
      printf(
          "\n'occupancy' is everything this game's cache holds between runs,\n"
          "both kinds together. Hit and miss counts are per process and are\n"
          "reported by the telemetry recorder, not here.\n"
          "A non-zero 'rejected' count means entries failed their checksum,\n"
          "which happens after a crash or a full disk and is harmless: the\n"
          "entry is recompiled.\n");
    }
    mr_shader_cache_close(&cache);
    return EXIT_OK;
  }

  if (strcmp(action, "clear") == 0) {
    uint64_t before = cache.current_bytes;
    for (int k = 0; k < MR_CACHE_KIND_COUNT; k++) {
      st = mr_shader_cache_clear(&cache, (mr_cache_kind)k);
      if (st != MR_OK) {
        mr_print_status("cache clear", st);
        mr_shader_cache_close(&cache);
        return EXIT_INTERNAL;
      }
    }
    if (json) {
      printf("{\n  \"cleared_bytes\": %llu\n}\n",
             (unsigned long long)before);
    } else {
      printf("cleared %llu MB for %s\n",
             (unsigned long long)(before / (1024ull * 1024ull)), game_id);
    }
    mr_shader_cache_close(&cache);
    return EXIT_OK;
  }

  fprintf(stderr, "mrctl: unknown cache action \"%s\"\n", action);
  mr_shader_cache_close(&cache);
  return EXIT_USAGE;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
  if (argc < 2) {
    mr_usage();
    return EXIT_USAGE;
  }

  bool json = false;
  const char *root = getenv("MR_ROOT");
  const char *profile_override = NULL;
  const char *profiles_root = NULL;
  const char *compat_file = NULL;
  bool use_builtins = true;
  bool save_profile = false;
  const char *positional[4] = {NULL, NULL, NULL, NULL};
  size_t positional_count = 0;

  /*
   * Options are accepted before and after the command, and the command is the
   * first argument that is not an option.
   *
   * The previous parser took argv[1] as the command unconditionally, so
   * `mrctl --root /x analyze game.exe` -- the order the usage implies, by listing
   * these as global options and saying nothing about position -- failed with
   * "unknown command \"--root\"" and printed the help. The conventional reading is
   * the one users arrive with, and it should work.
   */
  const char *command = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      mr_usage();
      return EXIT_OK;
    }
    if (strcmp(a, "--json") == 0) {
      json = true;
    } else if (strcmp(a, "--save-profile") == 0) {
      save_profile = true;
    } else if (strcmp(a, "--no-compat") == 0) {
      use_builtins = false;
    } else if (strcmp(a, "--root") == 0 && i + 1 < argc) {
      root = argv[++i];
    } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
      profile_override = argv[++i];
    } else if (strcmp(a, "--profiles") == 0 && i + 1 < argc) {
      profiles_root = argv[++i];
    } else if (strcmp(a, "--compat") == 0 && i + 1 < argc) {
      compat_file = argv[++i];
    } else if (a[0] == '-' && a[1] != '\0') {
      fprintf(stderr, "mrctl: unknown option \"%s\"\n", a);
      mr_usage();
      return EXIT_USAGE;
    } else if (command == NULL) {
      command = a;
    } else if (positional_count < sizeof(positional) / sizeof(positional[0])) {
      positional[positional_count++] = a;
    } else {
      fputs("mrctl: too many arguments\n", stderr);
      return EXIT_USAGE;
    }
  }

  if (command == NULL) {
    mr_usage();
    return EXIT_USAGE;
  }
  if (strcmp(command, "help") == 0) {
    mr_usage();
    return EXIT_OK;
  }

  if (root == NULL) root = ".";

  mr_runtime_layout layout;
  mr_layout_init(&layout, root);

  /*
   * A profiles root given on the command line splits the two trees apart, which
   * is what a test needs: a component tree it did not create, and a profiles
   * tree it can throw away.
   */
  if (profiles_root == NULL) profiles_root = layout.profiles_dir;

  if (strcmp(command, "host") == 0) return mr_cmd_host(&layout, json);

  if (strcmp(command, "analyze") == 0) {
    if (positional_count < 1) {
      fputs("mrctl: analyze needs an executable\n", stderr);
      return EXIT_USAGE;
    }
    return mr_cmd_analyze(positional[0], json);
  }

  if (strcmp(command, "plan") == 0) {
    if (positional_count < 1) {
      fputs("mrctl: plan needs an executable\n", stderr);
      return EXIT_USAGE;
    }
    return mr_cmd_plan(positional[0], profiles_root, profile_override,
                       compat_file, use_builtins, json, save_profile);
  }

  if (strcmp(command, "profile") == 0) {
    if (positional_count < 1) {
      fputs("mrctl: profile needs an executable\n", stderr);
      return EXIT_USAGE;
    }
    return mr_cmd_profile(positional[0], profiles_root);
  }

  if (strcmp(command, "compat") == 0) {
    if (positional_count < 1) {
      fputs("mrctl: compat needs a game id\n", stderr);
      return EXIT_USAGE;
    }
    return mr_cmd_compat(positional[0], compat_file, use_builtins, json);
  }

  if (strcmp(command, "cache") == 0) {
    return mr_cmd_cache(root, positional_count >= 1 ? positional[0] : NULL,
                        positional_count >= 2 ? positional[1] : NULL, json);
  }

  fprintf(stderr, "mrctl: unknown command \"%s\"\n", command);
  mr_usage();
  return EXIT_USAGE;
}
