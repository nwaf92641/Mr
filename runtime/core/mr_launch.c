/*
 * The launch planner.
 *
 * Planning is separated from execution so that the runtime's judgement is a
 * pure function of (facts, profile, host, layout). That is what makes it
 * testable without a device, and it is what lets the UI answer "why is this
 * running the way it is" with a specific observation instead of a shrug.
 *
 * Every decision this file makes is recorded in the plan's trace with the
 * observation that produced it. That is not decoration: the brief's rule is that
 * no optimisation is applied because it sounds good, and a trace that has to
 * name its evidence is the cheapest way to enforce that on ourselves.
 *
 * The planner never fails for a reason the user could fix by changing a file or
 * a setting. Those come back as blockers, so the UI can show the sentence. MR_*
 * errors are reserved for exhausted memory.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_launch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * DXMT reads its configuration from the path in DXMT_CONFIG_FILE, or from
 * "dxmt.conf" in the working directory when that is unset. Pointing the variable
 * at a per-game file is what keeps two titles from fighting over one config, and
 * it means the user never has to reason about load order.
 */
#define MR_DXMT_CONFIG_ENV "DXMT_CONFIG_FILE"

/*
 * The four DXMT modules that replace Wine's builtins and must be loaded native.
 * d3d10core is included because DXMT implements the D3D10 surface on top of the
 * same core, and a title that probes for D3D10 first would otherwise get Wine's
 * stub and conclude the GPU is a D3D10 part with no compute.
 */
#define MR_DXMT_NATIVE_MODULES "d3d11;d3d10core;dxgi;winemetal"

/* The feature level a title is given unless its profile asks otherwise.
 *
 * 11_0, not 11_1. Feature level 11_1 advertises a set of optional D3D11.1
 * capabilities that the translation layer then has to answer for, and titles
 * that receive 11_1 are entitled to require them. 11_0 covers everything the
 * 11_0 core mandates, which is what the overwhelming majority of D3D11 titles
 * use; raising it is a per-title fix, not a default.
 */
#define MR_DEFAULT_FEATURE_LEVEL "11_0"

/* ---------------------------------------------------------------- layout */

void mr_layout_init(mr_runtime_layout *layout, const char *root) {
  if (layout == NULL) return;
  memset(layout, 0, sizeof(*layout));

  const char *base = (root != NULL) ? root : ".";
  snprintf(layout->root, sizeof(layout->root), "%s", base);

  struct {
    char *dst;
    size_t dst_cap;
    const char *sub;
  } dirs[] = {
      {layout->runtime_dir, sizeof(layout->runtime_dir), "runtime"},
      {layout->prefix_dir, sizeof(layout->prefix_dir), "prefix"},
      {layout->profiles_dir, sizeof(layout->profiles_dir), "profiles"},
      {layout->cache_dir, sizeof(layout->cache_dir), "cache"},
      {layout->logs_dir, sizeof(layout->logs_dir), "logs"},
      {layout->wine_root, sizeof(layout->wine_root), "third_party/dist/wine"},
      {layout->fex_root, sizeof(layout->fex_root), "third_party/dist/fex"},
      {layout->dxmt_root, sizeof(layout->dxmt_root), "third_party/dist/dxmt"},
  };
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    char *p = mr_path_join(base, dirs[i].sub);
    if (p == NULL) continue;
    snprintf(dirs[i].dst, dirs[i].dst_cap, "%s", p);
    free(p);
  }

  struct {
    char *dst;
    size_t dst_cap;
    const char *sub;
  } files[] = {
      {layout->wine_loader, sizeof(layout->wine_loader),
       "third_party/dist/wine/bin/wine64"},
      {layout->wine_aarch64_modules, sizeof(layout->wine_aarch64_modules),
       "third_party/dist/wine/aarch64-windows"},
      {layout->wine_arm64ec_modules, sizeof(layout->wine_arm64ec_modules),
       "third_party/dist/wine/arm64ec-windows"},
      {layout->wine_sysx64_dir, sizeof(layout->wine_sysx64_dir),
       "third_party/dist/wine/sysx64"},
      {layout->fex_xtajit64, sizeof(layout->fex_xtajit64),
       "third_party/dist/fex/xtajit64.dll"},
      {layout->dxmt_arm64ec_dir, sizeof(layout->dxmt_arm64ec_dir),
       "third_party/dist/dxmt/arm64ec-windows"},
      {layout->dxmt_aarch64_dir, sizeof(layout->dxmt_aarch64_dir),
       "third_party/dist/dxmt/aarch64-windows"},
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char *p = mr_path_join(base, files[i].sub);
    if (p == NULL) continue;
    snprintf(files[i].dst, files[i].dst_cap, "%s", p);
    free(p);
  }
}

static bool mr_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

mr_status mr_layout_probe(mr_runtime_layout *layout, mr_str *missing) {
  if (layout == NULL) return MR_ERR_INVALID;

  layout->wine_present = mr_file_exists(layout->wine_loader) &&
                         mr_path_is_dir(layout->wine_arm64ec_modules);
  layout->fex_present = mr_file_exists(layout->fex_xtajit64);
  layout->dxmt_present = mr_path_is_dir(layout->dxmt_arm64ec_dir);
  layout->prefix_initialised = mr_path_is_dir(layout->prefix_dir);

  if (missing != NULL) {
    mr_str_clear(missing);
    if (!layout->wine_present) {
      (void)mr_str_appendz(missing, "Wine (no loader at ");
      (void)mr_str_appendz(missing, layout->wine_loader);
      (void)mr_str_appendz(missing, ")");
    }
    if (!layout->fex_present) {
      if (missing->len > 0) (void)mr_str_appendz(missing, ", ");
      (void)mr_str_appendz(missing, "FEX-Emu (no xtajit64.dll)");
    }
    if (!layout->dxmt_present) {
      if (missing->len > 0) (void)mr_str_appendz(missing, ", ");
      (void)mr_str_appendz(missing, "DXMT (no arm64ec module directory)");
    }
  }
  return MR_OK;
}

/* ------------------------------------------------------------------ plan */

mr_status mr_plan_init(mr_launch_plan *plan) {
  if (plan == NULL) return MR_ERR_INVALID;
  memset(plan, 0, sizeof(*plan));
  mr_status st = mr_strvec_init(&plan->warnings);
  if (st != MR_OK) return st;
  return mr_strvec_init(&plan->blockers);
}

void mr_plan_free(mr_launch_plan *plan) {
  if (plan == NULL) return;

  /* Only argv is heap-allocated. The environment entries are fixed-size arrays
   * *inside* the plan, so their addresses point into the plan itself; freeing
   * them corrupts the heap. Releasing the owning struct is all that is needed. */
  for (size_t i = 0; i < plan->argc; i++) free(plan->argv[i]);
  plan->argc = 0;
  plan->envc = 0;
  plan->decision_count = 0;

  mr_strvec_free(&plan->warnings);
  mr_strvec_free(&plan->blockers);
  memset(plan, 0, sizeof(*plan));
}

static mr_decision *mr_plan_find_decision(mr_launch_plan *plan,
                                          const char *subject) {
  for (size_t i = 0; i < plan->decision_count; i++) {
    if (strcmp(plan->decisions[i].subject, subject) == 0) {
      return &plan->decisions[i];
    }
  }
  return NULL;
}

static void mr_plan_record(mr_launch_plan *plan, const char *subject,
                           const char *choice, const char *fmt, va_list args,
                           bool replace) {
  if (plan == NULL || subject == NULL) return;

  mr_decision *slot = mr_plan_find_decision(plan, subject);
  if (slot == NULL) {
    if (plan->decision_count >= MR_LAUNCH_MAX_DECISIONS) return;
    slot = &plan->decisions[plan->decision_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->subject, sizeof(slot->subject), "%s", subject);
  } else if (!replace) {
    return; /* first answer wins unless the caller is correcting one */
  }

  snprintf(slot->choice, sizeof(slot->choice), "%s", choice != NULL ? choice : "");
  if (fmt != NULL) {
    vsnprintf(slot->reason, sizeof(slot->reason), fmt, args);
  } else {
    slot->reason[0] = '\0';
  }
}

void mr_plan_add_decision(mr_launch_plan *plan, const char *subject,
                          const char *choice, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  mr_plan_record(plan, subject, choice, fmt, args, false);
  va_end(args);
}

void mr_plan_set_decision(mr_launch_plan *plan, const char *subject,
                          const char *choice, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  mr_plan_record(plan, subject, choice, fmt, args, true);
  va_end(args);
}

mr_status mr_plan_add_env(mr_launch_plan *plan, const char *key,
                          const char *value) {
  if (plan == NULL || key == NULL || value == NULL) return MR_ERR_INVALID;
  if (strlen(key) >= sizeof(plan->env[0].key)) return MR_ERR_RANGE;

  /* Replace rather than duplicate: a later rule overriding an earlier one must
   * not leave two conflicting entries in the environment. */
  for (size_t i = 0; i < plan->envc; i++) {
    if (strcmp(plan->env[i].key, key) == 0) {
      snprintf(plan->env[i].value, sizeof(plan->env[i].value), "%s", value);
      return MR_OK;
    }
  }
  if (plan->envc >= MR_LAUNCH_MAX_ENV) return MR_ERR_RANGE;

  /* Fixed-size fields, so a value longer than the field is truncated loudly
   * rather than allocated for. An environment entry that long is a bug in a
   * profile, and truncation keeps the plan renderable so the trace can show it. */
  if (strlen(value) >= sizeof(plan->env[0].value)) return MR_ERR_RANGE;

  mr_env_var *slot = &plan->env[plan->envc];
  snprintf(slot->key, sizeof(slot->key), "%s", key);
  snprintf(slot->value, sizeof(slot->value), "%s", value);
  plan->envc++;
  return MR_OK;
}

mr_status mr_plan_add_arg(mr_launch_plan *plan, const char *arg) {
  if (plan == NULL || arg == NULL) return MR_ERR_INVALID;
  if (plan->argc >= MR_LAUNCH_MAX_ARGS) return MR_ERR_RANGE;
  char *copy = strdup(arg);
  if (copy == NULL) return MR_ERR_NOMEM;
  plan->argv[plan->argc++] = copy;
  return MR_OK;
}

static mr_status mr_plan_msg(mr_strvec *v, const char *fmt, va_list args) {
  char buf[640];
  vsnprintf(buf, sizeof(buf), fmt, args);
  return mr_strvec_push(v, buf);
}

mr_status mr_plan_add_warning(mr_launch_plan *plan, const char *fmt, ...) {
  if (plan == NULL || fmt == NULL) return MR_ERR_INVALID;
  va_list args;
  va_start(args, fmt);
  mr_status st = mr_plan_msg(&plan->warnings, fmt, args);
  va_end(args);
  return st;
}

mr_status mr_plan_add_blocker(mr_launch_plan *plan, const char *fmt, ...) {
  if (plan == NULL || fmt == NULL) return MR_ERR_INVALID;
  va_list args;
  va_start(args, fmt);
  mr_status st = mr_plan_msg(&plan->blockers, fmt, args);
  va_end(args);
  return st;
}

/* ------------------------------------------------------------- planning */

static void mr_plan_identity(mr_launch_plan *plan, const mr_game_facts *facts,
                             const mr_profile *profile) {
  snprintf(plan->exe, sizeof(plan->exe), "%s",
           facts->exe_path[0] != '\0' ? facts->exe_path
                                      : mr_profile_get_or(profile, "exe", ""));
  snprintf(plan->cwd, sizeof(plan->cwd), "%s",
           facts->game_dir[0] != '\0' ? facts->game_dir : ".");

  const char *profile_id = mr_profile_get(profile, "id");
  if (profile_id != NULL && profile_id[0] != '\0') {
    snprintf(plan->game_id, sizeof(plan->game_id), "%s", profile_id);
  } else {
    mr_analyze_game_id(facts, plan->game_id);
  }
}

static void mr_plan_cpu(mr_launch_plan *plan, const mr_game_facts *facts,
                        const mr_profile *profile, const mr_host_caps *host) {
  plan->cpu_path = mr_profile_cpu_path(profile);
  plan->uses_fex = false;

  if (facts->arch == MR_ARCH_ARM64 || facts->arch == MR_ARCH_ARM64X) {
    plan->cpu_path = MR_CPU_PATH_NATIVE;
    mr_plan_add_decision(plan, "cpu.path", "native",
                         "the guest is already arm64, so nothing is translated");
    return;
  }

  /* Past the arm64 early return, so this guest is not arm64: it needs
   * translation whether or not this host can provide it. */
  plan->needs_translation = true;

  if (!mr_host_can_translate_x86(host)) {
    mr_plan_add_decision(
        plan, "cpu.path", mr_cpu_path_str(plan->cpu_path),
        "the guest needs x86-64 translation, but this process cannot execute "
        "memory it wrote (JIT unavailable), so the title cannot start");
    /*
     * A blocker, not only a decision.
     *
     * Recording the reason in the trace is not enough: the caller decides
     * whether to launch from the blocker list, so a title that cannot start and
     * no blocker is a launch that proceeds and fails somewhere inside the
     * translator with nothing pointing back here. The text is the user-facing
     * explanation of a specific, unfixable-in-software condition.
     */
    if (host->platform == MR_PLATFORM_IPADOS) {
      (void)mr_plan_add_blocker(
          plan,
          "This title needs x86-64 translation, which needs a JIT, and this "
          "process does not have one. On iPadOS that requires a debugger "
          "attached with the JIT entitlement; installing a signed build is not "
          "sufficient. Without it the translator cannot allocate executable "
          "memory, so the title cannot start.");
    } else {
      (void)mr_plan_add_blocker(
          plan,
          "This title needs x86-64 translation, but this process is not "
          "permitted to execute memory it has written, so the translator cannot "
          "allocate executable memory. Check that the hardening settings for "
          "the process allow it.");
    }
    return;
  }

  plan->uses_fex = true;
  if (plan->cpu_path == MR_CPU_PATH_FEX_AOT) {
    mr_plan_add_decision(
        plan, "cpu.path", "fex-aot",
        "the profile asked for pre-translation; blocks compiled ahead of use "
        "do not cost frame time at first use");
  } else {
    mr_plan_add_decision(plan, "cpu.path", "fex-jit",
                         "the guest is x86-64 and this host can execute "
                         "generated code, so blocks are translated on demand");
  }
}

static void mr_plan_graphics(mr_launch_plan *plan, const mr_game_facts *facts,
                             const mr_profile *profile,
                             const mr_host_caps *host) {
  plan->gfx_api = facts->gfx;

  bool allow_metal4 = mr_profile_get_bool(profile, "allow_metal4", true);
  const char *force = mr_profile_get(profile, "backend");
  if (force != NULL && mr_str_iequal(force, "metal3")) {
    /*
     * There is no Metal 3 runtime path to force. This used to switch the runtime
     * onto one, which meant a profile could quietly move a title off Metal 4 and a
     * plan would look healthy while the target it claims to test was not running.
     * The setting still has an effect, and the effect is a refusal: the profile
     * asks for something this runtime does not have, and the reader is told so.
     */
    allow_metal4 = false;
  }

  const char *reason = NULL;
  plan->backend = mr_host_pick_backend(host, allow_metal4, &reason);
  mr_plan_add_decision(plan, "graphics.backend", mr_backend_str(plan->backend),
                       "%s", reason != NULL ? reason : "no reason recorded");

  plan->graphics_enabled = (plan->backend != MR_BACKEND_NONE);

  /*
   * A blocked graphics path is a blocker and not only a decision. The decision
   * list is what a reader studies; the blocker list is what the runtime and the
   * interface act on, and a title whose graphics target does not exist has to be
   * visible in it or the launch proceeds and fails somewhere deeper.
   */
  if (!plan->graphics_enabled) {
    (void)mr_plan_add_blocker(
        plan, "no graphics target: %s",
        reason != NULL ? reason : "the host reported no reason");
  }

  /*
   * Feature level. A profile may name a specific level; otherwise the 11_0
   * default applies, and its justification is recorded either way so a later
   * reader can tell a default from a deliberate choice.
   */
  const char *level = mr_profile_get(profile, "feature_level");
  if (level == NULL || mr_str_iequal(level, "auto")) {
    level = MR_DEFAULT_FEATURE_LEVEL;
    mr_plan_add_decision(
        plan, "graphics.feature_level", level,
        "11_0 is the default: it is the full mandatory Direct3D 11.0 feature "
        "set, and advertising 11_1 would make optional 11.1 capabilities "
        "binding on the translation layer");
  } else {
    mr_plan_add_decision(plan, "graphics.feature_level", level,
                         "the game profile requests this level explicitly");
  }

  /* DXMT spells feature levels as MAJOR_MINOR, so the plan carries the same
   * shape. Anything unparsable falls back to the default rather than silently
   * becoming level 0. */
  int major = 0;
  int minor = 0;
  if (sscanf(level, "%d_%d", &major, &minor) != 2 || major < 9 || major > 12) {
    if (sscanf(MR_DEFAULT_FEATURE_LEVEL, "%d_%d", &major, &minor) != 2) {
      major = 11;
      minor = 0;
    }
    (void)mr_plan_add_warning(
        plan,
        "feature_level \"%s\" is not a MAJOR_MINOR Direct3D level; using %s.",
        level, MR_DEFAULT_FEATURE_LEVEL);
  }
  plan->d3d_feature_level = major * 100 + minor;
}

static void mr_plan_jit(mr_launch_plan *plan, const mr_profile *profile,
                        const mr_host_caps *host) {
  if (!plan->uses_fex) {
    plan->jit_pool_bytes = 0;
    mr_plan_add_decision(plan, "cpu.jit_pool", "none",
                         "no translation happens, so no code cache is allocated");
    return;
  }

  uint64_t override_mb = mr_profile_get_uint(profile, "jit_pool_mb", 0);
  uint64_t requested = override_mb != 0
                           ? override_mb * 1024ull * 1024ull
                           : mr_host_recommended_jit_pool_bytes(host);

  uint64_t hole = host->jit_hole_bytes;
  uint64_t chosen = mr_host_derive_jit_pool_bytes(host, requested, hole, 0);

  if (chosen == 0) {
    plan->jit_pool_bytes = 0;
    (void)mr_plan_add_blocker(
        plan,
        "The translator's code cache cannot be placed: the largest contiguous "
        "region below the guest address window is %llu MB, which is under the "
        "256 MB floor. Free memory and try again.",
        (unsigned long long)(hole / (1024ull * 1024ull)));
    mr_plan_add_decision(
        plan, "cpu.jit_pool", "unavailable",
        "the measured hole (%llu MB) is below the floor, and a pool placed "
        "outside the hole hangs the first translated call rather than failing",
        (unsigned long long)(hole / (1024ull * 1024ull)));
    return;
  }

  plan->jit_pool_bytes = chosen;
  char choice[32];
  snprintf(choice, sizeof(choice), "%llu MB",
           (unsigned long long)(chosen / (1024ull * 1024ull)));
  if (chosen < requested) {
    mr_plan_set_decision(
        plan, "cpu.jit_pool", choice,
        "reduced from the %llu MB the device budget suggests, because only "
        "%llu MB is contiguous below the guest window; the pool must fit in "
        "that hole or the first translated call hangs",
        (unsigned long long)(requested / (1024ull * 1024ull)),
        (unsigned long long)(hole / (1024ull * 1024ull)));
  } else {
    mr_plan_set_decision(
        plan, "cpu.jit_pool", choice,
        "%s fit within the measured hole of %llu MB",
        override_mb != 0 ? "the profile's requested size"
                         : "the size derived from the device memory budget",
        (unsigned long long)(hole / (1024ull * 1024ull)));
  }
}

static mr_status mr_plan_dll_overrides(mr_launch_plan *plan,
                                       const mr_profile *profile) {
  mr_str overrides;
  mr_status st = mr_str_init(&overrides);
  if (st != MR_OK) return st;

  if (plan->graphics_enabled && plan->gfx_api == MR_GFX_D3D11) {
    st = mr_str_appendz(&overrides, MR_DXMT_NATIVE_MODULES "=n");
  }

  for (size_t i = 0; st == MR_OK && i < profile->dll_native.count; i++) {
    if (overrides.len > 0) st = mr_str_appendz(&overrides, ";");
    if (st == MR_OK) st = mr_str_appendz(&overrides, profile->dll_native.items[i]);
    if (st == MR_OK) st = mr_str_appendz(&overrides, "=n");
  }
  for (size_t i = 0; st == MR_OK && i < profile->dll_builtin.count; i++) {
    if (overrides.len > 0) st = mr_str_appendz(&overrides, ";");
    if (st == MR_OK) st = mr_str_appendz(&overrides, profile->dll_builtin.items[i]);
    if (st == MR_OK) st = mr_str_appendz(&overrides, "=b");
  }

  if (st == MR_OK) {
    snprintf(plan->wine_dll_overrides, sizeof(plan->wine_dll_overrides), "%s",
             overrides.data != NULL ? overrides.data : "");
  }
  mr_str_free(&overrides);
  return st;
}

/* ------------------------------------------------------------- environment */

static mr_status mr_plan_environment(mr_launch_plan *plan,
                                     const mr_profile *profile,
                                     const mr_runtime_layout *layout) {
  mr_status st;

  st = mr_plan_add_env(plan, "WINEPREFIX", layout->prefix_dir);
  if (st != MR_OK) return st;

  /* A 64-bit-only prefix: this release has no WoW64 path, and letting Wine
   * create a mixed prefix would produce failures deep inside DLL loading. */
  st = mr_plan_add_env(plan, "WINEARCH", "win64");
  if (st != MR_OK) return st;

  if (plan->wine_dll_overrides[0] != '\0') {
    st = mr_plan_add_env(plan, "WINEDLLOVERRIDES", plan->wine_dll_overrides);
    if (st != MR_OK) return st;
  }

  /* Wine is chatty by default and writes to stderr, which on iPadOS is the
   * device log. Keep it quiet unless the profile asks for diagnostics. */
  const char *winedebug = mr_profile_get(profile, "winedebug");
  st = mr_plan_add_env(plan, "WINEDEBUG",
                       winedebug != NULL ? winedebug : "-all");
  if (st != MR_OK) return st;

  if (plan->uses_fex) {
    char fex_config[MR_LAUNCH_PATH_MAX];
    const char *base = profile->profile_dir[0] != '\0' ? profile->profile_dir
                                                       : layout->profiles_dir;
    if (!mr_path_join_into(fex_config, sizeof(fex_config), base, "fex.txt")) {
      return MR_ERR_RANGE;
    }
    st = mr_plan_add_env(plan, "FEX_APP_CONFIG", fex_config);
    if (st != MR_OK) return st;

    /*
     * Instruction-cache coherency for self-modifying code. On by default: the
     * cost is a check the translator already performs, and the failure it
     * prevents -- a title that patches its own code and then executes the old
     * bytes -- is a mystery crash with no stack that helps.
     */
    bool smc = mr_profile_get_bool(profile, "smc_checks", true);
    st = mr_plan_add_env(plan, "FEX_SMCCHECKS", smc ? "1" : "0");
    if (st != MR_OK) return st;

    /*
     * Total store order emulation. x86-64 is strongly ordered and arm64 is not,
     * so a title that relies on store ordering without a barrier needs this.
     * Left on for correctness; a profile may turn it off for a title that is
     * known not to depend on it, and the trace will say that is why.
     */
    bool tso = mr_profile_get_bool(profile, "tso", true);
    st = mr_plan_add_env(plan, "FEX_TSOENABLED", tso ? "1" : "0");
    if (st != MR_OK) return st;

    /* Translate a run of blocks at a time rather than one at a time. Straight
     * win on sequential code, which is most of it. */
    st = mr_plan_add_env(plan, "FEX_MULTIBLOCK", "1");
    if (st != MR_OK) return st;

    if (plan->jit_pool_bytes != 0) {
      char pool[32];
      snprintf(pool, sizeof(pool), "%llu",
               (unsigned long long)plan->jit_pool_bytes);
      st = mr_plan_add_env(plan, "MR_JIT_POOL_BYTES", pool);
      if (st != MR_OK) return st;
    }
  }

  if (plan->graphics_enabled) {
    char dxmt_config[MR_LAUNCH_PATH_MAX];
    const char *base = profile->profile_dir[0] != '\0' ? profile->profile_dir
                                                       : layout->profiles_dir;
    if (!mr_path_join_into(dxmt_config, sizeof(dxmt_config), base, "dxmt.conf")) {
      return MR_ERR_RANGE;
    }
    st = mr_plan_add_env(plan, MR_DXMT_CONFIG_ENV, dxmt_config);
    if (st != MR_OK) return st;
  }

  st = mr_plan_add_env(plan, "MR_GAME_ID", plan->game_id);
  if (st != MR_OK) return st;
  st = mr_plan_add_env(plan, "MR_BACKEND", mr_backend_str(plan->backend));
  if (st != MR_OK) return st;
  st = mr_plan_add_env(plan, "MR_CACHE_DIR", layout->cache_dir);
  if (st != MR_OK) return st;

  /*
   * Select the ARM64EC module farm. This does not make Wine run ARM64EC -- Wine
   * picks a module by the PE machine word -- it tells the staging layer which
   * farm to link into the prefix, which is the part Wine cannot decide for us.
   */
  st = mr_plan_add_env(plan, "MR_USE_ARM64EC", "1");
  if (st != MR_OK) return st;

  /* Profile-supplied environment last, so a user's explicit setting wins over
   * every default above. */
  for (size_t i = 0; i < profile->entry_count; i++) {
    const mr_profile_entry *e = &profile->entries[i];
    if (e->section != MR_SECTION_ENVIRONMENT) continue;
    st = mr_plan_add_env(plan, e->key, e->value);
    if (st != MR_OK) return st;
  }

  return MR_OK;
}

static mr_status mr_plan_arguments(mr_launch_plan *plan,
                                   const mr_runtime_layout *layout) {
  if (layout->wine_present) {
    mr_status st = mr_plan_add_arg(plan, layout->wine_loader);
    if (st != MR_OK) return st;
  } else {
    /* A placeholder so the plan still renders; the blocker below explains. */
    mr_status st = mr_plan_add_arg(plan, "wine64");
    if (st != MR_OK) return st;
  }
  return mr_plan_add_arg(plan, plan->exe);
}

/* ---------------------------------------------------------- diagnostics */

static void mr_plan_verdicts(mr_launch_plan *plan, const mr_game_facts *facts,
                             const mr_host_caps *host,
                             const mr_runtime_layout *layout) {
  if (!facts->pe.is_valid) {
    (void)mr_plan_add_blocker(plan,
                              "The executable could not be parsed as a Windows "
                              "PE image. It may be a shortcut, a disk image or "
                              "a truncated download.");
    return;
  }

  if (facts->pe.is_dll) {
    (void)mr_plan_add_blocker(
        plan, "That file is a DLL, not a program. Import the game's .exe.");
  }

  if (facts->pe.is_truncated) {
    /*
     * A separate blocker from the parse failure above, and worth having: the
     * headers parse perfectly here, so this file looks like a game to every
     * check except this one, and the failure the user would otherwise meet is a
     * fault inside the loader with no mention of the download.
     */
    (void)mr_plan_add_blocker(
        plan,
        "This file is incomplete: it ends before the data its own sections "
        "describe. Re-download or re-copy it; a partial copy is the usual cause, "
        "and no setting here can substitute for the missing bytes.");
  }

  if (facts->looks_like_setup) {
    (void)mr_plan_add_blocker(
        plan,
        "That file looks like an installer rather than a game. Install the "
        "title first, then import the game's own executable.");
  }

  if (facts->is_wow64) {
    (void)mr_plan_add_blocker(
        plan,
        "This is a 32-bit program. This release implements only the x86-64 "
        "path; 32-bit titles need WoW64, which is not in scope.");
  }

  if (facts->arch == MR_ARCH_UNKNOWN) {
    (void)mr_plan_add_blocker(
        plan, "The executable's processor architecture is not one Mr supports.");
  }

  if (facts->anticheat != MR_AC_NONE) {
    (void)mr_plan_add_blocker(
        plan,
        "%s is present. Kernel-level anti-cheat cannot run here: it requires "
        "privileged Windows kernel interfaces that no compatibility layer "
        "provides, and the emulated CPU fails its integrity checks anyway.",
        mr_anticheat_str(facts->anticheat));
  } else if (facts->drm == MR_DRM_DENUVO || facts->drm == MR_DRM_SECUROM ||
             facts->drm == MR_DRM_TAGES) {
    (void)mr_plan_add_blocker(
        plan,
        "%s is present. It verifies the CPU and the surrounding code through "
        "techniques that an emulated x86-64 exposes as inconsistent, so the "
        "title will refuse to start.",
        mr_drm_str(facts->drm));
  }

  if (facts->is_managed_net) {
    (void)mr_plan_add_warning(
        plan,
        "%s needs a .NET runtime. Mr ships none; Wine can supply one, but "
        "managed titles are outside what this release claims to run and may "
        "fail in ways this build cannot diagnose.",
        facts->net_frame_versions.items.count > 0
            ? facts->net_frame_versions.items.items[0]
            : "This title");
  }

  if (facts->gfx == MR_GFX_D3D12) {
    (void)mr_plan_add_blocker(
        plan,
        "This title uses Direct3D 12. Mr translates Direct3D 11 only; DX12 is "
        "deliberately out of scope for this release.");
  } else if (facts->gfx == MR_GFX_VULKAN) {
    (void)mr_plan_add_blocker(
        plan,
        "This title uses Vulkan. Mr has no Vulkan path in this release; that "
        "would need MoltenVK, which is not part of the graphics backend.");
  } else if (facts->gfx == MR_GFX_D3D9 || facts->gfx == MR_GFX_D3D10) {
    (void)mr_plan_add_blocker(
        plan,
        "This title uses %s. The DXMT path in this release targets Direct3D 11 "
        "only.",
        mr_gfx_api_str(facts->gfx));
  } else if (facts->gfx == MR_GFX_OPENGL) {
    (void)mr_plan_add_blocker(plan,
                              "This title uses OpenGL, which is not wired up in "
                              "this release.");
  } else if (facts->gfx == MR_GFX_NONE) {
    (void)mr_plan_add_warning(
        plan,
        "No graphics API was detected in the executable. If the game engine "
        "lives in a sibling DLL, the profile may need the game's real "
        "executable, or the title may be packed.");
  }

  if (facts->needs_shader_model_6) {
    (void)mr_plan_add_warning(
        plan,
        "The image contains shader model 6 bytecode. DXMT translates shader "
        "model 5 and below; SM6 shaders in a Direct3D 11 title need DXIL "
        "support that this release does not claim.");
  }

  if (facts->packer_sections.items.count > 0) {
    (void)mr_plan_add_warning(
        plan,
        "The image is packed or protected. The import table is probably "
        "incomplete, so the dependency list above may understate what the "
        "title needs.");
  }

  if (facts->vc_runtime_modules.items.count == 0 &&
      facts->engine != MR_ENGINE_UNITY && facts->gfx != MR_GFX_NONE) {
    (void)mr_plan_add_warning(
        plan,
        "No Visual C++ runtime was detected. Most D3D11 titles link one "
        "statically instead, so this is normal -- but if the launch fails on a "
        "missing msvcp or vcruntime module, that is why.");
  } else if (facts->vc_runtime_modules.items.count > 0) {
    (void)mr_plan_add_warning(
        plan,
        "This title needs a Visual C++ runtime (%s). Mr does not redistribute "
        "Microsoft's runtime DLLs; install them into the prefix yourself, or "
        "the launch will fail on a missing module.",
        facts->vc_runtime_modules.items.items[0]);
  }

  if (facts->input.wants_xinput) {
    mr_plan_add_decision(
        plan, "input.controller", "xinput",
        "the title imports XInput, so a gamepad must be present and mapped or "
        "the title will report no controller");
  } else if (facts->input.wants_directinput) {
    mr_plan_add_decision(plan, "input.controller", "directinput",
                         "the title imports DirectInput");
  } else {
    mr_plan_add_decision(
        plan, "input.controller", "keyboard-mouse",
        "no controller API was imported, so input is keyboard and mouse");
  }

  if (facts->default_width > 0 && facts->default_height > 0) {
    mr_plan_add_decision(plan, "graphics.resolution", "from-config",
                         "the title's own settings file asks for %dx%d; the "
                         "profile's resolution is what actually gets used when "
                         "set, and this is only a starting point",
                         facts->default_width, facts->default_height);
  } else {
    mr_plan_add_decision(
        plan, "graphics.resolution", "auto",
        "no resolution found in a settings file; the guest display size is "
        "chosen from the drawable, which avoids a mismatched mode with no way "
        "back to a menu");
  }

  if (facts->has_launcher_files) {
    (void)mr_plan_add_warning(
        plan,
        "This looks like a launcher rather than the game itself. Importing the "
        "game's own executable usually avoids a nested launcher that cannot "
        "start its child here.");
  }

  if (!layout->wine_present || !layout->dxmt_present ||
      (plan->uses_fex && !layout->fex_present)) {
    mr_str missing;
    if (mr_str_init(&missing) == MR_OK) {
      /* Reported from the flags the caller already probed, rather than by
       * probing again: preflight must not touch the filesystem, because on
       * iPadOS it may be called from a context that cannot do so. */
      if (!layout->wine_present) {
        (void)mr_str_appendz(&missing, "Wine");
      }
      if (!layout->fex_present) {
        if (missing.len > 0) (void)mr_str_appendz(&missing, ", ");
        (void)mr_str_appendz(&missing, "FEX-Emu");
      }
      if (!layout->dxmt_present) {
        if (missing.len > 0) (void)mr_str_appendz(&missing, ", ");
        (void)mr_str_appendz(&missing, "DXMT");
      }
      (void)mr_plan_add_blocker(
          plan,
          "Required components are missing from this installation: %s. Run "
          "tools/fetch-components.sh to obtain them.",
          missing.len > 0 ? missing.data : "unknown");
      mr_str_free(&missing);
    }
  }

  if (!plan->graphics_enabled && facts->gfx == MR_GFX_D3D11) {
    (void)mr_plan_add_blocker(
        plan,
        "No graphics backend is available for Direct3D 11 on this device. "
        "Either Metal is unavailable to this process, or Metal 4 was requested "
        "and refused.");
  }

  /*
   * The iPadOS "no JIT" blocker lived here and could never fire: it was gated on
   * `uses_fex`, which the CPU step sets only when translation was granted, so the
   * one case it was written for is the case where it stayed silent.
   * mr_plan_cpu now reports that condition with a fuller message, before any of
   * this runs.
   */
}

/* ------------------------------------------------------------------- build */

mr_status mr_plan_build(const mr_game_facts *facts, const mr_profile *profile,
                        const mr_host_caps *host,
                        const mr_runtime_layout *layout,
                        mr_launch_plan *out) {
  if (facts == NULL || profile == NULL || host == NULL || layout == NULL ||
      out == NULL) {
    return MR_ERR_INVALID;
  }

  mr_status st = mr_plan_init(out);
  if (st != MR_OK) return st;

  mr_plan_identity(out, facts, profile);
  mr_plan_cpu(out, facts, profile, host);
  mr_plan_graphics(out, facts, profile, host);
  mr_plan_jit(out, profile, host);

  st = mr_plan_dll_overrides(out, profile);
  if (st == MR_OK) st = mr_plan_environment(out, profile, layout);
  if (st == MR_OK) st = mr_plan_arguments(out, layout);
  if (st != MR_OK) {
    mr_plan_free(out);
    return st;
  }

  mr_plan_verdicts(out, facts, host, layout);
  return MR_OK;
}

/* ----------------------------------------------------------------- render */

/* JSON string escaping. Handles the control characters and the two quote
 * characters; a plan contains file paths, so a stray backslash is plausible. */
static mr_status mr_json_escape(mr_str *s, const char *text) {
  mr_status st = mr_str_appendz(s, "\"");
  for (const char *p = text; *p != '\0' && st == MR_OK; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"': st = mr_str_appendz(s, "\\\""); break;
      case '\\': st = mr_str_appendz(s, "\\\\"); break;
      case '\n': st = mr_str_appendz(s, "\\n"); break;
      case '\r': st = mr_str_appendz(s, "\\r"); break;
      case '\t': st = mr_str_appendz(s, "\\t"); break;
      default:
        if (c < 0x20) {
          st = mr_str_appendf(s, "\\u%04x", (unsigned)c);
        } else {
          st = mr_str_append(s, (const char *)&c, 1);
        }
        break;
    }
  }
  if (st == MR_OK) st = mr_str_appendz(s, "\"");
  return st;
}

mr_status mr_plan_to_json(const mr_launch_plan *plan, mr_str *out) {
  if (plan == NULL || out == NULL) return MR_ERR_INVALID;

  if (out->data == NULL) {
    mr_status init = mr_str_init(out);
    if (init != MR_OK) return init;
  }
  mr_str_clear(out);

  mr_status ok = MR_OK;
  ok = mr_str_appendz(out, "{\n  \"game_id\": ");
  if (ok == MR_OK) ok = mr_json_escape(out, plan->game_id);
  if (ok == MR_OK) ok = mr_str_appendz(out, ",\n  \"exe\": ");
  if (ok == MR_OK) ok = mr_json_escape(out, plan->exe);
  if (ok == MR_OK) ok = mr_str_appendz(out, ",\n  \"cwd\": ");
  if (ok == MR_OK) ok = mr_json_escape(out, plan->cwd);
  if (ok == MR_OK) {
    ok = mr_str_appendf(out,
                        ",\n  \"cpu_path\": \"%s\",\n  \"backend\": \"%s\","
                        "\n  \"graphics_api\": \"%s\","
                        "\n  \"jit_pool_mb\": %llu,\n  \"feature_level\": %d,"
                        "\n  \"graphics_enabled\": %s,\n  \"uses_fex\": %s,\n",
                        mr_cpu_path_str(plan->cpu_path),
                        mr_backend_str(plan->backend),
                        mr_gfx_api_str(plan->gfx_api),
                        (unsigned long long)(plan->jit_pool_bytes /
                                             (1024ull * 1024ull)),
                        plan->d3d_feature_level,
                        plan->graphics_enabled ? "true" : "false",
                        plan->uses_fex ? "true" : "false");
  }

  if (ok == MR_OK) ok = mr_str_appendz(out, "  \"argv\": [");
  for (size_t i = 0; ok == MR_OK && i < plan->argc; i++) {
    if (i > 0) ok = mr_str_appendz(out, ", ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->argv[i]);
  }
  if (ok == MR_OK) ok = mr_str_appendz(out, "],\n");

  if (ok == MR_OK) ok = mr_str_appendz(out, "  \"environment\": {");
  for (size_t i = 0; ok == MR_OK && i < plan->envc; i++) {
    if (i > 0) ok = mr_str_appendz(out, ",");
    if (ok == MR_OK) ok = mr_str_appendz(out, "\n    ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->env[i].key);
    if (ok == MR_OK) ok = mr_str_appendz(out, ": ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->env[i].value);
  }
  if (ok == MR_OK) ok = mr_str_appendz(out, plan->envc > 0 ? "\n  },\n" : "},\n");

  if (ok == MR_OK) ok = mr_str_appendz(out, "  \"decisions\": [");
  for (size_t i = 0; ok == MR_OK && i < plan->decision_count; i++) {
    if (i > 0) ok = mr_str_appendz(out, ",");
    if (ok == MR_OK) ok = mr_str_appendz(out, "\n    {\"subject\": ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->decisions[i].subject);
    if (ok == MR_OK) ok = mr_str_appendz(out, ", \"choice\": ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->decisions[i].choice);
    if (ok == MR_OK) ok = mr_str_appendz(out, ", \"reason\": ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->decisions[i].reason);
    if (ok == MR_OK) ok = mr_str_appendz(out, "}");
  }
  if (ok == MR_OK) {
    ok = mr_str_appendz(out, plan->decision_count > 0 ? "\n  ],\n" : "],\n");
  }

  if (ok == MR_OK) ok = mr_str_appendz(out, "  \"warnings\": [");
  for (size_t i = 0; ok == MR_OK && i < plan->warnings.count; i++) {
    if (i > 0) ok = mr_str_appendz(out, ",");
    if (ok == MR_OK) ok = mr_str_appendz(out, "\n    ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->warnings.items[i]);
  }
  if (ok == MR_OK) {
    ok = mr_str_appendz(out, plan->warnings.count > 0 ? "\n  ],\n" : "],\n");
  }

  if (ok == MR_OK) ok = mr_str_appendz(out, "  \"blockers\": [");
  for (size_t i = 0; ok == MR_OK && i < plan->blockers.count; i++) {
    if (i > 0) ok = mr_str_appendz(out, ",");
    if (ok == MR_OK) ok = mr_str_appendz(out, "\n    ");
    if (ok == MR_OK) ok = mr_json_escape(out, plan->blockers.items[i]);
  }
  if (ok == MR_OK) {
    ok = mr_str_appendz(out, plan->blockers.count > 0 ? "\n  ]\n}\n" : "]\n}\n");
  }

  return ok;
}

mr_status mr_plan_to_text(const mr_launch_plan *plan, mr_str *out) {
  if (plan == NULL || out == NULL) return MR_ERR_INVALID;

  if (out->data == NULL) {
    mr_status init = mr_str_init(out);
    if (init != MR_OK) return init;
  }
  mr_str_clear(out);
  mr_status st = MR_OK;

  st = mr_str_appendf(out, "game:      %s\n", plan->game_id);
  if (st == MR_OK) st = mr_str_appendf(out, "exe:       %s\n", plan->exe);
  if (st == MR_OK) {
    st = mr_str_appendf(out, "cpu:       %s\n",
                        mr_cpu_path_str(plan->cpu_path));
  }
  if (st == MR_OK) {
    st = mr_str_appendf(out, "backend:   %s\n", mr_backend_str(plan->backend));
  }
  if (st == MR_OK && plan->jit_pool_bytes > 0) {
    st = mr_str_appendf(out, "jit pool:  %llu MB\n",
                        (unsigned long long)(plan->jit_pool_bytes /
                                             (1024ull * 1024ull)));
  }

  if (st == MR_OK) st = mr_str_appendz(out, "\ndecisions:\n");
  for (size_t i = 0; st == MR_OK && i < plan->decision_count; i++) {
    st = mr_str_appendf(out, "  %-24s %-16s %s\n", plan->decisions[i].subject,
                        plan->decisions[i].choice, plan->decisions[i].reason);
  }

  if (plan->warnings.count > 0) {
    if (st == MR_OK) st = mr_str_appendz(out, "\nwarnings:\n");
    for (size_t i = 0; st == MR_OK && i < plan->warnings.count; i++) {
      st = mr_str_appendf(out, "  - %s\n", plan->warnings.items[i]);
    }
  }

  if (plan->blockers.count > 0) {
    if (st == MR_OK) st = mr_str_appendz(out, "\ncannot run:\n");
    for (size_t i = 0; st == MR_OK && i < plan->blockers.count; i++) {
      st = mr_str_appendf(out, "  - %s\n", plan->blockers.items[i]);
    }
  }

  if (st == MR_OK) st = mr_str_appendz(out, "\ncommand:\n  ");
  for (size_t i = 0; st == MR_OK && i < plan->argc; i++) {
    st = mr_str_appendf(out, "%s ", plan->argv[i]);
  }
  if (st == MR_OK) st = mr_str_appendz(out, "\n");

  return st;
}

/* --------------------------------------------------------------- preflight */

mr_status mr_plan_preflight(const mr_launch_plan *plan,
                            const mr_host_caps *host,
                            const mr_runtime_layout *layout,
                            mr_strvec *problems) {
  if (plan == NULL || host == NULL || layout == NULL || problems == NULL) {
    return MR_ERR_INVALID;
  }

  if (plan->needs_translation) {
    /*
     * Both halves of "JIT works" are checked, because they fail differently and
     * only one of them is visible to the user. A process can hold the JIT
     * entitlement (so every capability flag says yes) with no debugger attached,
     * in which case the code-cache allocation comes back zero and the failure
     * surfaces far away as a placement error.
     *
     * Gated on `needs_translation` rather than `uses_fex`: when translation is
     * refused, `uses_fex` is false, and gating on it here would make the
     * preflight silent about the one thing that is actually wrong.
     */
    if (!host->jit_capable || !host->jit_enabled) {
      (void)mr_strvec_push(problems,
                           "Executable memory is not available to this process "
                           "(JIT is off). The translator cannot run.");
    } else if (!mr_host_jit_selftest()) {
      (void)mr_strvec_push(
          problems,
          "The JIT entitlement is present but writing and then executing a page "
          "failed. On iPadOS this normally means no debugger is attached; "
          "attach one and retry.");
    }
  }

  if (plan->graphics_enabled && !host->gpu_available) {
    (void)mr_strvec_push(problems,
                         "No Metal device is visible to this process.");
  }

  if (!layout->prefix_initialised) {
    /* Not fatal: the first run creates the prefix. Reported so a failure during
     * prefix creation is not mistaken for a graphics or CPU problem. */
    (void)mr_strvec_push(
        problems,
        "The Wine prefix does not exist yet; it will be created on first run. "
        "This takes a minute and is expected.");
  }

  if (plan->jit_pool_bytes > 0 && host->free_disk_bytes > 0) {
    uint64_t need = plan->jit_pool_bytes / 4;
    if (host->free_disk_bytes < need) {
      (void)mr_strvec_push(
          problems,
          "Free disk space is low; the caches may not fit and shader "
          "compilation will repeat on every run.");
    }
  }

  return MR_OK;
}

mr_status mr_plan_execute(const mr_launch_plan *plan,
                          const mr_runtime_layout *layout) {
  if (plan == NULL || layout == NULL) return MR_ERR_INVALID;

  /*
   * Deliberately not implemented in the portable core. Spawning the guest is
   * the one thing that cannot be expressed portably: on iPadOS there is no
   * fork/exec of a Windows loader, and Wine runs in-process with wineserver as
   * a thread. The Apple platform layer owns that, and the portable core exists
   * so that everything leading up to it is testable without a device.
   */
  (void)layout;
  return MR_ERR_UNSUPPORTED;
}
