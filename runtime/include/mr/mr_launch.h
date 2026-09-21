/*
 * The launch planner.
 *
 * The planner is separated from the launcher on purpose. Planning is pure: it
 * reads a profile, the analyzer's facts and a description of the host, and
 * returns a fully-resolved plan -- the argv, the environment, the DLL override
 * string, the backend, the code-cache size -- plus a trace explaining every
 * choice. Execution then does nothing but carry that plan out.
 *
 * The split exists because planning is where the runtime's judgement lives and
 * it is the only part that can be tested without a device. It is also how a
 * user gets a straight answer to "why is this game running at 22 fps": the
 * trace says which backend was picked, whether Metal 4 was refused and on what
 * evidence, how big the code cache is and what constrained it.
 *
 * The rule the trace enforces is that no optimisation is applied because it
 * sounds good. Each decision names the observation that produced it.
 */
#ifndef MR_LAUNCH_H
#define MR_LAUNCH_H

#include "mr/mr_analyze.h"
#include "mr/mr_host.h"
#include "mr/mr_profile.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MR_LAUNCH_MAX_ARGS 128
#define MR_LAUNCH_MAX_ENV 192
#define MR_LAUNCH_MAX_DECISIONS 32
#define MR_LAUNCH_PATH_MAX 1024

/*
 * Where every moving part lives on this installation. Resolved once, at plan
 * time, so a missing component is reported before anything is spawned.
 */
typedef struct {
  char root[MR_LAUNCH_PATH_MAX];           /* install root */
  char runtime_dir[MR_LAUNCH_PATH_MAX];    /* root/runtime */
  char prefix_dir[MR_LAUNCH_PATH_MAX];     /* the Wine prefix */
  char profiles_dir[MR_LAUNCH_PATH_MAX];
  char cache_dir[MR_LAUNCH_PATH_MAX];
  char logs_dir[MR_LAUNCH_PATH_MAX];

  /* Wine's unix-side libraries and its PE module farms. */
  char wine_root[MR_LAUNCH_PATH_MAX];
  char wine_loader[MR_LAUNCH_PATH_MAX];    /* wine64 / wine64-preloader */
  char wine_aarch64_modules[MR_LAUNCH_PATH_MAX];
  char wine_arm64ec_modules[MR_LAUNCH_PATH_MAX];
  char wine_sysx64_dir[MR_LAUNCH_PATH_MAX];

  /* FEX-Emu */
  char fex_root[MR_LAUNCH_PATH_MAX];
  char fex_xtajit64[MR_LAUNCH_PATH_MAX];

  /* DXMT */
  char dxmt_root[MR_LAUNCH_PATH_MAX];
  char dxmt_arm64ec_dir[MR_LAUNCH_PATH_MAX];
  char dxmt_aarch64_dir[MR_LAUNCH_PATH_MAX];

  bool fex_present;
  bool wine_present;
  bool dxmt_present;
  bool prefix_initialised;
} mr_runtime_layout;

/* Fills in the conventional locations under `root`. Does not touch the disk. */
void mr_layout_init(mr_runtime_layout *layout, const char *root);
/* Sets the *_present flags from what is actually on disk, and reports the
 * first missing component in `missing` (which may be NULL). */
mr_status mr_layout_probe(mr_runtime_layout *layout, mr_str *missing);

typedef struct {
  char subject[64]; /* "graphics.backend" */
  char choice[96];  /* "metal4" */
  char reason[384];
} mr_decision;

typedef struct {
  char key[128];
  char value[512];
} mr_env_var;

typedef struct {
  char *argv[MR_LAUNCH_MAX_ARGS];
  size_t argc;
  mr_env_var env[MR_LAUNCH_MAX_ENV];
  size_t envc;

  mr_decision decisions[MR_LAUNCH_MAX_DECISIONS];
  size_t decision_count;

  mr_strvec warnings;    /* the run will proceed, but quality is at risk */
  mr_strvec blockers;    /* the run cannot proceed */

  /* Resolved configuration. */
  char game_id[MR_PROFILE_ID_MAX];
  char exe[MR_LAUNCH_PATH_MAX];
  char cwd[MR_LAUNCH_PATH_MAX];
  mr_cpu_path cpu_path;
  mr_backend backend;
  mr_gfx_api gfx_api;
  uint64_t jit_pool_bytes;
  int d3d_feature_level; /* e.g. 11_0 -> 1100; 0 when automatic */
  char wine_dll_overrides[2048];
  bool graphics_enabled;
  /*
   * Whether the guest needs translation at all, which is a different question
   * from whether this host can perform it. `uses_fex` answers the second and is
   * therefore false in exactly the situation the user needs to hear about; a
   * plan that cannot translate must still say so.
   */
  bool needs_translation;
  bool uses_fex;
} mr_launch_plan;

mr_status mr_plan_init(mr_launch_plan *plan);
void mr_plan_free(mr_launch_plan *plan);

void mr_plan_add_decision(mr_launch_plan *plan, const char *subject,
                          const char *choice, const char *fmt, ...);
/* Replaces an existing decision for the same subject, so a later, better-informed
 * rule wins without producing a contradictory trace. */
void mr_plan_set_decision(mr_launch_plan *plan, const char *subject,
                          const char *choice, const char *fmt, ...);
mr_status mr_plan_add_env(mr_launch_plan *plan, const char *key,
                          const char *value);
mr_status mr_plan_add_arg(mr_launch_plan *plan, const char *arg);
mr_status mr_plan_add_warning(mr_launch_plan *plan, const char *fmt, ...);
mr_status mr_plan_add_blocker(mr_launch_plan *plan, const char *fmt, ...);

/*
 * Builds the plan. Never fails for a reason the user can fix: a plan that
 * cannot run is returned with MR_OK and a populated `blockers` list, so the UI
 * can show the reason. MR_ERR_* is reserved for resources the planner itself
 * could not allocate.
 */
mr_status mr_plan_build(const mr_game_facts *facts, const mr_profile *profile,
                        const mr_host_caps *host,
                        const mr_runtime_layout *layout,
                        mr_launch_plan *out);

/* The plan as JSON, for the UI and for bug reports. Caller frees. */
mr_status mr_plan_to_json(const mr_launch_plan *plan, mr_str *out);
/* The plan as human-readable text, one decision per line. Caller frees. */
mr_status mr_plan_to_text(const mr_launch_plan *plan, mr_str *out);

/*
 * Pre-flight checks that planning cannot perform because they depend on the
 * running process: JIT actually enabled, a debugger actually attached, the
 * prefix actually writable, enough disk for the caches.
 *
 * This exists because the most misleading failure in this class of runtime is
 * a code-cache allocation failing for a reason that has nothing to do with
 * allocation. Checking both halves of "JIT works" up front turns an obscure
 * placement error into a sentence the user can act on.
 */
mr_status mr_plan_preflight(const mr_launch_plan *plan,
                            const mr_host_caps *host,
                            const mr_runtime_layout *layout,
                            mr_strvec *problems);

/* Carries out the plan. Not available off-device. */
mr_status mr_plan_execute(const mr_launch_plan *plan,
                          const mr_runtime_layout *layout);

#ifdef __cplusplus
}
#endif

#endif /* MR_LAUNCH_H */
