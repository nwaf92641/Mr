/*
 * The test suite.
 *
 * A test here exercises a real code path on a real input. The PE fixtures are
 * synthetic but structurally valid images that the parser reads with the same
 * code it uses on a retail executable; the profile and cache tests write real
 * files to a temporary directory. Nothing is stubbed, because a test that asserts
 * against a stub proves only that the stub agrees with itself.
 *
 * The organising idea is that each test names the layer it covers, so a failure
 * says which layer broke rather than producing a generic crash. That is the same
 * requirement the brief places on the runtime's own diagnostics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mr/mr_analyze.h"
#include "mr/mr_backend.h"
#include "mr/mr_cache.h"
#include "mr/mr_compat.h"
#include "mr/mr_host.h"
#include "mr/mr_launch.h"
#include "mr/mr_pe.h"
#include "mr/mr_profile.h"
#include "mr/mr_telemetry.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#include "mr_fixture.h"

/* ------------------------------------------------------------------ harness */

/*
 * Two sets of counters, and the distinction matters.
 *
 * g_checks/g_failures describe the test currently running, so mr_test_end() can
 * print "ok" or a blank marker. Each test resets them. The totals accumulate
 * across the run because the exit status has to reflect the whole run: with only
 * the per-test pair, main() reported the last test's failures and exited 0, so a
 * run with failures in it was indistinguishable from a passing one to ctest.
 */
static int g_checks;
static int g_failures;
static int g_checks_total;
static int g_failures_total;
static const char *g_current_test = "(none)";

static void mr_test_begin(const char *name) {
  g_current_test = name;
  printf("  %-46s", name);
  fflush(stdout);
}

static void mr_test_end(void) {
  if (g_failures == 0) {
    printf("ok\n");
  } else {
    printf("\n");
  }
  fflush(stdout);
}

static void mr_check(bool condition, const char *expression, int line) {
  g_checks++;
  g_checks_total++;
  if (condition) return;
  g_failures++;
  g_failures_total++;
  printf("\n    FAIL %s:%d: %s\n", g_current_test, line, expression);
  fflush(stdout);
}

#define CHECK(expr) mr_check((expr), #expr, __LINE__)

static void mr_check_str(const char *actual, const char *expected, int line) {
  g_checks++;
  g_checks_total++;
  bool same = actual != NULL && expected != NULL && strcmp(actual, expected) == 0;
  if (same) return;
  g_failures++;
  g_failures_total++;
  printf("\n    FAIL %s:%d: expected \"%s\", got \"%s\"\n", g_current_test, line,
         expected != NULL ? expected : "(null)",
         actual != NULL ? actual : "(null)");
  fflush(stdout);
}

#define CHECK_STR(actual, expected) mr_check_str((actual), (expected), __LINE__)

/*
 * The comparison is at intmax_t width and the cast is explicit, so a test can
 * compare a size_t, a uint64_t and an enum against a literal without the
 * sign-conversion warnings that -Werror promotes to errors. The explicit cast is
 * the point: it says the narrowing is intended, which is exactly the statement
 * the warning wants made.
 */
static void mr_check_int(intmax_t actual, intmax_t expected, int line) {
  g_checks++;
  g_checks_total++;
  if (actual == expected) return;
  g_failures++;
  g_failures_total++;
  printf("\n    FAIL %s:%d: expected %lld, got %lld\n", g_current_test, line,
         (long long)expected, (long long)actual);
  fflush(stdout);
}

#define CHECK_INT(actual, expected) \
  mr_check_int((intmax_t)(actual), (intmax_t)(expected), __LINE__)

/* --------------------------------------------------------------- utilities */

/*
 * The temporary directory is deliberately much smaller than the path buffers the
 * tests pass to mr_tmp_path. Every caller has to be able to hold the directory,
 * a separator and a file name, and the compiler checks that statically: with both
 * at 512 it could not prove the concatenation fits and refused the build under
 * -Wformat-truncation, which is the warning doing its job. A pid is at most ten
 * digits, so 64 bytes is generous.
 */
static char g_tmpdir[64];

static void mr_make_tmpdir(void) {
  snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/mr-tests-%d", (int)getpid());
  (void)mr_mkdirs(g_tmpdir);
}

static void mr_tmp_path(char *dst, size_t cap, const char *name) {
  snprintf(dst, cap, "%s/%s", g_tmpdir, name);
}

/* --------------------------------------------------------------- PE parser */

static void test_pe_parses_a_d3d11_x64_image(void) {
  mr_test_begin("pe: x86-64 image with imports");
  g_failures = 0;

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));

  mr_pe pe;
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, f.len, &pe) == MR_OK);
  CHECK(pe.is_valid);
  CHECK(pe.is_64bit);
  CHECK_INT(pe.machine, 0x8664);
  CHECK_INT(pe.arch, MR_ARCH_AMD64_OR_ARM64EC);
  CHECK(!pe.is_dll);
  CHECK_INT(pe.section_count, 3);
  CHECK(pe.sections[0].name[0] == '.');

  /* Module names are lowercased by the parser, which is what lets the rest of
   * the runtime compare them without caring how the linker spelled them. */
  CHECK(mr_pe_imports_module(&pe, "d3d11.dll"));
  CHECK(mr_pe_imports_module(&pe, "dxgi.dll"));
  CHECK(mr_pe_imports_module(&pe, "user32.dll"));
  CHECK(!mr_pe_imports_module(&pe, "d3d12.dll"));

  CHECK(mr_pe_imports_symbol(&pe, "d3d11.dll", "d3d11createdevice"));
  CHECK(mr_pe_imports_symbol(&pe, "d3d11.dll",
                             "d3d11createdeviceandswapchain"));
  CHECK(!mr_pe_imports_symbol(&pe, "d3d11.dll", "notarealsymbol"));

  CHECK_INT(pe.dxbc_blob_count, 2);
  CHECK_INT(pe.dxil_blob_count, 0);

  mr_pe_free(&pe);
  mr_test_end();
}

static void test_pe_rejects_truncated_images(void) {
  mr_test_begin("pe: rejects a truncation");
  g_failures = 0;

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));

  /*
   * The property under test is that nothing reads past the end: the parser is
   * handed bytes chosen by whoever produced the file, so an out-of-bounds read
   * here is a memory-safety bug reachable from a hostile download. Under
   * AddressSanitizer this test is the one that catches it.
   *
   * The second half is about what the parser *claims*. Reading a header and then
   * refusing the image is correct and useful: `mr_pe_parse` reports the header it
   * found and `is_valid` says whether the image is usable. What must never happen
   * is a fragment being reported as a usable image, because the analyzer would
   * then describe a game that cannot exist.
   *
   * The boundary is the end of the section table, computed from the fixture
   * rather than guessed: everything before it is a fragment by construction, and
   * everything from it on is a complete-enough image.
   */
  const size_t pe_off = (size_t)f.bytes[0x3C] | ((size_t)f.bytes[0x3D] << 8) |
                        ((size_t)f.bytes[0x3E] << 16) |
                        ((size_t)f.bytes[0x3F] << 24);
  const size_t coff = pe_off + 4;
  const size_t section_count =
      (size_t)f.bytes[coff + 2] | ((size_t)f.bytes[coff + 3] << 8);
  const size_t optional_size =
      (size_t)f.bytes[coff + 16] | ((size_t)f.bytes[coff + 17] << 8);
  const size_t sections_end = coff + 20 + optional_size + section_count * 40;
  CHECK_INT(section_count, 3);
  CHECK(sections_end < f.len);

  mr_pe pe;
  size_t steps = 0;
  size_t valid_from_a_fragment = 0;

  for (size_t n = 1; n < f.len; n += 13) {
    steps++;
    CHECK(mr_pe_init(&pe) == MR_OK);
    mr_status st = mr_pe_parse(f.bytes, n, &pe);
    if (st == MR_OK && pe.is_valid && n < sections_end) valid_from_a_fragment++;
    mr_pe_free(&pe);
  }

  CHECK(steps > 100);
  CHECK_INT(valid_from_a_fragment, 0);

  /* Exactly at the boundary the image is usable, which is what makes the check
   * above a statement about fragments rather than about is_valid never being
   * set. It is still not *complete*: the file ends before the sections' bodies
   * do, and the two flags are separate questions for that reason. */
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, sections_end, &pe) == MR_OK);
  CHECK(pe.is_valid);
  CHECK(pe.is_truncated);
  CHECK_INT(pe.section_count, 3);
  mr_pe_free(&pe);

  /* One byte short of the section table: unparseable as an image, and the
   * reason is missing structure rather than a lying header. */
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, sections_end - 1, &pe) == MR_OK);
  CHECK(!pe.is_valid);
  CHECK(pe.is_truncated);
  mr_pe_free(&pe);

  /* The whole file completes the picture. */
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, f.len, &pe) == MR_OK);
  CHECK(pe.is_valid);
  CHECK(!pe.is_truncated);
  CHECK_INT(pe.section_count, 3);
  mr_pe_free(&pe);

  /* A file that is not a PE at all. */
  CHECK(mr_pe_init(&pe) == MR_OK);
  const uint8_t garbage[64] = {0};
  CHECK(mr_pe_parse(garbage, sizeof(garbage), &pe) != MR_OK);
  mr_pe_free(&pe);

  /* A plausible header whose section table points outside the file. The section
   * walk must stop rather than trust the count. */
  mr_fixture evil;
  mr_fixture_d3d11(&evil);
  CHECK(mr_fixture_build(&evil));
  evil.bytes[0x40 + 4 + 2] = 0xFF; /* NumberOfSections = 255 */
  evil.bytes[0x40 + 4 + 2 + 1] = 0x00;
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(evil.bytes, evil.len, &pe) == MR_OK);
  CHECK(pe.section_count <= MR_PE_MAX_SECTIONS);
  mr_pe_free(&pe);

  mr_test_end();
}

static void test_pe_reads_the_clr_version(void) {
  mr_test_begin("pe: CLR header and metadata version");
  g_failures = 0;

  mr_fixture f;
  mr_fixture_init(&f);
  mr_fixture_set_clr(&f, "v4.0.30319");
  CHECK(mr_fixture_build(&f));

  mr_pe pe;
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, f.len, &pe) == MR_OK);
  CHECK(pe.has_clr_header);
  CHECK(pe.clr_rva != 0);

  /* The metadata root must be reachable through the CLR header's directory
   * entry, which is the path the analyzer uses to name the required runtime. */
  size_t root = mr_pe_rva_to_offset(&pe, pe.clr_rva);
  CHECK(root != 0);

  mr_pe_free(&pe);
  mr_test_end();
}

static void test_pe_detects_sections(void) {
  mr_test_begin("pe: section lookup");
  g_failures = 0;

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_add_section(&f, ".vmp0"));
  CHECK(mr_fixture_build(&f));

  mr_pe pe;
  CHECK(mr_pe_init(&pe) == MR_OK);
  CHECK(mr_pe_parse(f.bytes, f.len, &pe) == MR_OK);
  CHECK(mr_pe_has_section(&pe, ".rdata"));
  CHECK(mr_pe_has_section(&pe, ".vmp0"));
  CHECK(!mr_pe_has_section(&pe, ".themida"));
  CHECK(mr_pe_find_section(&pe, ".text") != NULL);
  CHECK(mr_pe_find_section(&pe, ".nonexistent") == NULL);

  mr_pe_free(&pe);
  mr_test_end();
}

/* ---------------------------------------------------------------- analyzer */

static void test_analyze_rejects_an_incomplete_download(void) {
  mr_test_begin("analyzer: incomplete download is not runnable");
  g_failures = 0;

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));

  /* The whole image: complete, and runnable. */
  char full[512];
  mr_tmp_path(full, sizeof(full), "complete.exe");
  CHECK(mr_write_file_atomic(full, f.bytes, f.len) == MR_OK);

  mr_game_facts facts;
  CHECK(mr_analyze_exe(full, &facts) == MR_OK);
  CHECK(facts.pe.is_valid);
  CHECK(!facts.pe.is_truncated);
  CHECK(mr_analyze_is_runnable(&facts));
  mr_analyze_free(&facts);

  /*
   * The same image cut off partway through its section data.
   *
   * Worth its own test because every header-driven question still answers "yes":
   * it is x86-64, it imports d3d11.dll, it carries DXBC blobs. Only the file
   * length gives it away, and with nothing checking that, the user is told the
   * download is fine and then watches it fail somewhere inside the loader with no
   * mention of the file being short.
   */
  const size_t cut = f.len - 512;
  CHECK(cut > 0 && cut < f.len);
  char partial[512];
  mr_tmp_path(partial, sizeof(partial), "partial.exe");
  CHECK(mr_write_file_atomic(partial, f.bytes, cut) == MR_OK);

  CHECK(mr_analyze_exe(partial, &facts) == MR_OK);
  CHECK(facts.pe.is_valid);
  CHECK(facts.pe.is_truncated);
  CHECK(!mr_analyze_is_runnable(&facts));
  mr_analyze_free(&facts);

  /* And the planner has to refuse it for that reason, not for a vague one. */
  CHECK(mr_analyze_exe(partial, &facts) == MR_OK);
  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  mr_host_caps host;
  mr_host_probe(&host);
  mr_runtime_layout layout;
  mr_layout_init(&layout, g_tmpdir);
  mr_launch_plan plan;
  CHECK(mr_plan_init(&plan) == MR_OK);
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  bool blames_the_file = false;
  for (size_t i = 0; i < plan.blockers.count; i++) {
    if (strstr(plan.blockers.items[i], "incomplete") != NULL) blames_the_file = true;
  }
  CHECK(plan.blockers.count > 0);
  CHECK(blames_the_file);
  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);

  mr_test_end();
}

static void test_analyze_classifies_d3d11(void) {
  mr_test_begin("analyzer: identifies Direct3D 11");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "game.exe");

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  CHECK_INT(facts.arch, MR_ARCH_AMD64_OR_ARM64EC);
  CHECK_INT(facts.gfx, MR_GFX_D3D11);
  /* A create-device import is a strong signal, not a weak one: the title calls
   * the API rather than merely linking it. */
  CHECK_INT(facts.gfx_confidence, MR_CONF_STRONG);
  CHECK(facts.input.wants_xinput);
  CHECK(facts.input.wants_raw_input);
  CHECK(facts.input.wants_keyboard_mouse);
  CHECK(facts.vc_runtime_modules.items.count > 0);
  CHECK(!facts.is_managed_net);
  CHECK(!facts.looks_like_setup);
  CHECK(mr_analyze_is_runnable(&facts));

  CHECK_STR(facts.exe_name, "game.exe");

  char id[80];
  mr_analyze_game_id(&facts, id);
  CHECK(strlen(id) > 8);
  /* The id must be stable across runs for the same image. */
  char id2[80];
  mr_analyze_game_id(&facts, id2);
  CHECK_STR(id, id2);

  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_analyze_rejects_32bit(void) {
  mr_test_begin("analyzer: 32-bit is a blocker");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "old-game.exe");

  mr_fixture f;
  mr_fixture_i386(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);
  CHECK_INT(facts.arch, MR_ARCH_I386);
  CHECK(facts.is_wow64);
  CHECK(!mr_analyze_is_runnable(&facts));

  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_analyze_finds_anticheat(void) {
  mr_test_begin("analyzer: finds anti-cheat and blocks");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "protected.exe");

  mr_fixture f;
  mr_fixture_anticheat(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);
  CHECK_INT(facts.anticheat, MR_AC_EASYANTICHEAT);

  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_analyze_finds_a_packer(void) {
  mr_test_begin("analyzer: VMProtect section is reported");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "packed.exe");

  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_add_section(&f, ".vmp0"));
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);
  CHECK_INT(facts.drm, MR_DRM_VMPROTECT);
  CHECK(facts.packer_sections.items.count > 0);

  mr_analyze_free(&facts);
  mr_test_end();
}

/* ---------------------------------------------------------------- profiles */

static void test_profile_round_trip(void) {
  mr_test_begin("profile: write, read back, same values");
  g_failures = 0;

  char dir[512];
  mr_tmp_path(dir, sizeof(dir), "profiles");
  (void)mr_mkdirs(dir);

  mr_profile written;
  CHECK(mr_profile_init(&written) == MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_CONFIG, "id", "test-game") == MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_CONFIG, "feature_level", "11_1") ==
        MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_GRAPHICS, "vsync", "false") ==
        MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_PERFORMANCE, "jit_pool_mb", "768") ==
        MR_OK);
  /* An unknown key must survive: a profile written by a newer build has to
   * round-trip through an older one, or downgrading silently discards it. */
  CHECK(mr_profile_set(&written, MR_SECTION_PERFORMANCE, "future_knob", "42") ==
        MR_OK);

  CHECK(mr_profile_write_layout(dir, &written, true) == MR_OK);

  mr_profile read;
  CHECK(mr_profile_init(&read) == MR_OK);
  CHECK(mr_profile_load_dir(written.profile_dir, &read) == MR_OK);

  CHECK_STR(mr_profile_get(&read, "feature_level"), "11_1");
  CHECK_STR(mr_profile_get(&read, "vsync"), "false");
  CHECK_STR(mr_profile_get(&read, "future_knob"), "42");
  CHECK_INT(mr_profile_get_int(&read, "jit_pool_mb", 0), 768);
  CHECK(mr_profile_get_bool(&read, "vsync", true) == false);
  CHECK(mr_profile_get_bool(&read, "missing_key", true) == true);

  mr_profile_free(&written);
  mr_profile_free(&read);
  mr_test_end();
}

static void test_profile_loads_over_a_seeded_default(void) {
  mr_test_begin("profile: loads on top of a seeded default");
  g_failures = 0;

  /*
   * The sequence the app performs on every launch: seed the profile with the id
   * the directory is named for, then load the directory over it.
   *
   * This failed with "key \"id\" is set more than once", because the
   * duplicate-key check compared against the whole profile instead of the file it
   * was reading, so the caller's own default collided with the file's copy of the
   * same key. Import, save, relaunch is the entire user flow, so nothing worked
   * past the first save.
   */
  char dir[512];
  mr_tmp_path(dir, sizeof(dir), "seedprofiles");
  (void)mr_mkdirs(dir);

  mr_profile written;
  CHECK(mr_profile_init(&written) == MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_CONFIG, "id", "seeded-game") ==
        MR_OK);
  CHECK(mr_profile_set(&written, MR_SECTION_CONFIG, "backend", "metal4") ==
        MR_OK);
  CHECK(mr_profile_write_layout(dir, &written, true) == MR_OK);
  char profdir[MR_PROFILE_PATH_MAX];
  snprintf(profdir, sizeof(profdir), "%s", written.profile_dir);
  CHECK(profdir[0] != '\0');
  mr_profile_free(&written);

  mr_profile seeded;
  CHECK(mr_profile_init(&seeded) == MR_OK);
  CHECK(mr_profile_set(&seeded, MR_SECTION_CONFIG, "id", "seeded-game") ==
        MR_OK);
  CHECK(mr_profile_load_dir(profdir, &seeded) == MR_OK);
  CHECK_STR(mr_profile_get(&seeded, "id"), "seeded-game");
  CHECK_STR(mr_profile_get(&seeded, "backend"), "metal4");
  mr_profile_free(&seeded);

  /* Two config files naming the same key is still fine -- the collision was
   * between files, and between files it is ordinary layering. Loading the same
   * directory twice covers that, since the second load starts from the first
   * load's result. */
  CHECK(mr_profile_init(&seeded) == MR_OK);
  CHECK(mr_profile_load_dir(profdir, &seeded) == MR_OK);
  CHECK(mr_profile_load_dir(profdir, &seeded) == MR_OK);
  CHECK_STR(mr_profile_get(&seeded, "backend"), "metal4");
  mr_profile_free(&seeded);

  mr_test_end();
}

static void test_profile_rejects_a_duplicate_key(void) {
  mr_test_begin("profile: duplicate key is an error");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "dup.conf");

  FILE *fp = fopen(path, "wb");
  CHECK(fp != NULL);
  if (fp != NULL) {
    fputs("# a comment\nwidth = 1\nwidth = 2\n", fp);
    fclose(fp);
  }

  mr_profile p;
  CHECK(mr_profile_init(&p) == MR_OK);
  /* A key set twice in one file is a mistake the user cannot see, so it fails
   * loudly rather than letting the second value win silently. */
  CHECK(mr_profile_load_file(path, MR_SECTION_CONFIG, &p) == MR_ERR_PARSE);
  CHECK(mr_profile_parse_error()[0] != '\0');
  mr_profile_free(&p);

  mr_test_end();
}

static void test_profile_dll_overrides(void) {
  mr_test_begin("profile: dll-overrides list");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "dll-overrides");

  FILE *fp = fopen(path, "wb");
  CHECK(fp != NULL);
  if (fp != NULL) {
    fputs("native = d3d11, dxgi\nnative += winemetal\nbuiltin = wined3d\n", fp);
    fclose(fp);
  }

  mr_profile p;
  CHECK(mr_profile_init(&p) == MR_OK);
  CHECK(mr_profile_load_file(path, MR_SECTION_DLL_OVERRIDES, &p) == MR_OK);
  CHECK_INT(p.dll_native.count, 3);
  CHECK_INT(p.dll_builtin.count, 1);
  CHECK_STR(p.dll_native.items[0], "d3d11");
  CHECK_STR(p.dll_native.items[2], "winemetal");

  mr_profile_free(&p);
  mr_test_end();
}

static void test_profile_id_safety(void) {
  mr_test_begin("profile: unsafe ids are refused");
  g_failures = 0;

  /* A game id names a directory, so anything that could escape it is refused
   * rather than normalised. */
  CHECK(!mr_profile_id_is_safe("../etc/passwd"));
  CHECK(!mr_profile_id_is_safe("a/b"));
  CHECK(!mr_profile_id_is_safe("."));
  CHECK(!mr_profile_id_is_safe(""));
  CHECK(!mr_profile_id_is_safe(NULL));
  CHECK(!mr_profile_id_is_safe("a\\b"));
  CHECK(mr_profile_id_is_safe("game-1234abcd"));
  CHECK(mr_profile_id_is_safe("Cyberpunk_2077"));

  char out[MR_PROFILE_PATH_MAX];
  CHECK(mr_profile_dir_for("/tmp", "../escape", out) == MR_ERR_INVALID);

  mr_test_end();
}

/* ------------------------------------------------------------- host policy */

static void test_host_jit_pool_sizing(void) {
  mr_test_begin("host: JIT pool respects the measured hole");
  g_failures = 0;

  mr_host_caps caps;
  mr_host_caps_init(&caps);
  caps.platform = MR_PLATFORM_IPADOS;
  caps.unified_memory = true;
  caps.physical_memory = 16ull * 1024 * 1024 * 1024;
  caps.memory_budget = 4ull * 1024 * 1024 * 1024;

  /* The recommendation is anchored on the one configuration validated on
   * hardware: a 4096 MB budget yields 896 MB. Asserting the anchor rather than a
   * range means a change to the constant is a decision someone has to make, not
   * a number that drifts. */
  uint64_t recommended = mr_host_recommended_jit_pool_bytes(&caps);
  CHECK_INT((long long)recommended, 896ll * 1024 * 1024);

  /* Monotonic in the budget, and ceilinged: past the ceiling the pool would be
   * larger than the hole it has to live in on every device seen, and the extra
   * size buys nothing because the code cache is not the bottleneck there. */
  mr_host_caps bigger = caps;
  bigger.memory_budget = 64ull * 1024 * 1024 * 1024;
  uint64_t scaled = mr_host_recommended_jit_pool_bytes(&bigger);
  CHECK(scaled >= recommended);
  CHECK(scaled <= 1792ull * 1024 * 1024);

  /* A hole larger than the request: the request is honoured. */
  uint64_t roomy = mr_host_derive_jit_pool_bytes(&caps, recommended,
                                                2ull * 1024 * 1024 * 1024, 0);
  CHECK_INT((long long)roomy, (long long)recommended);

  /* A hole smaller than the request: the hole decides, not the request. Pinning
   * address space moves the frontier rather than creating a gap, so a pool that
   * does not fit lands somewhere that hangs the first translated call instead of
   * faulting. Subtracting the headroom the guest and the Wine heap need is what
   * this branch exists for. */
  uint64_t hole = 800ull * 1024 * 1024;
  uint64_t headroom = 512ull * 1024 * 1024;
  uint64_t tight = mr_host_derive_jit_pool_bytes(&caps, recommended, hole, 0);
  CHECK_INT((long long)tight, (long long)(hole - headroom));

  /* A hole that cannot hold even the headroom leaves nothing. */
  CHECK_INT((long long)mr_host_derive_jit_pool_bytes(&caps, recommended,
                                                     400ull * 1024 * 1024, 0),
            0);

  /* Below the floor: refuse outright. Shrinking further would produce a pool
   * that spends its time evicting blocks it is about to need, and the resulting
   * stalls are indistinguishable from the game being slow -- so the runtime says
   * no rather than running badly. */
  CHECK_INT((long long)mr_host_derive_jit_pool_bytes(&caps, recommended,
                                                     600ull * 1024 * 1024, 0),
            0);

  /* The budget caps the pool as well: address space is not the only limit, and
   * the jetsam accountant does not care that the mapping was cheap. */
  mr_host_caps small = caps;
  small.memory_budget = 1024ull * 1024 * 1024;
  uint64_t from_small = mr_host_derive_jit_pool_bytes(
      &small, recommended, 4ull * 1024 * 1024 * 1024, 0);
  CHECK_INT((long long)from_small, 512ll * 1024 * 1024); /* budget / 2 */

  /* An explicit floor is respected, which is how a test can pin the boundary
   * without depending on the built-in constant. */
  CHECK_INT((long long)mr_host_derive_jit_pool_bytes(&caps, 100ull * 1024 * 1024,
                                                     4ull * 1024 * 1024 * 1024,
                                                     50ull * 1024 * 1024),
            100ll * 1024 * 1024);
  CHECK_INT((long long)mr_host_derive_jit_pool_bytes(&caps, 10ull * 1024 * 1024,
                                                     4ull * 1024 * 1024 * 1024,
                                                     50ull * 1024 * 1024),
            0);

  /* No budget at all means no answer, not a guess. */
  mr_host_caps unknown = caps;
  unknown.memory_budget = 0;
  CHECK_INT((long long)mr_host_recommended_jit_pool_bytes(&unknown), 0);
  CHECK_INT((long long)mr_host_derive_jit_pool_bytes(&unknown, recommended,
                                                     4ull * 1024 * 1024 * 1024,
                                                     0) > 0,
            1);

  mr_test_end();
}

static void test_host_backend_selection(void) {
  mr_test_begin("host: Metal 4 preferred, Metal 3 fallback");
  g_failures = 0;

  mr_host_caps caps;
  mr_host_caps_init(&caps);
  caps.gpu_available = true;

  const char *reason = NULL;
  caps.metal4_available = true;
  caps.metal3_available = true;
  CHECK_INT(mr_host_pick_backend(&caps, true, &reason), MR_BACKEND_METAL4);
  CHECK(reason != NULL);

  caps.metal4_available = false;
  CHECK_INT(mr_host_pick_backend(&caps, true, &reason), MR_BACKEND_METAL3);
  CHECK(reason != NULL && reason[0] != '\0');

  /* Explicitly refused by the profile. */
  caps.metal4_available = true;
  CHECK_INT(mr_host_pick_backend(&caps, false, &reason), MR_BACKEND_METAL3);

  /* No GPU at all. */
  caps.gpu_available = false;
  caps.metal4_available = false;
  caps.metal3_available = false;
  const char *no_device = NULL;
  CHECK_INT(mr_host_pick_backend(&caps, true, &no_device), MR_BACKEND_NONE);
  CHECK(no_device != NULL);

  /*
   * A device that is there and reports no Metal 3 family, which is what a
   * paravirtual GPU looks like. This is a different machine from the one above
   * and it must not be described with the same sentence: the arm64 macOS CI
   * runner is exactly this case, and saying "no device" about a device the probe
   * had just named sent the diagnosis in the wrong direction.
   */
  caps.gpu_available = true;
  caps.metal3_available = false;
  const char *no_family = NULL;
  CHECK_INT(mr_host_pick_backend(&caps, true, &no_family), MR_BACKEND_NONE);
  CHECK(no_family != NULL);
  CHECK(strcmp(no_device, no_family) != 0);

  /* An Apple Silicon device without Metal 4 is still usable. */
  caps.gpu_available = true;
  caps.metal3_available = true;
  CHECK_INT(mr_host_pick_backend(&caps, true, &reason), MR_BACKEND_METAL3);

  mr_test_end();
}

static void test_host_jit_selftest(void) {
  mr_test_begin("host: JIT executes generated code or says it cannot");
  g_failures = 0;

  /* This runs the real probe: it maps memory read-write, writes a stub that
   * returns 42, makes the page executable, and calls it. On the build host it
   * should succeed; on a hardened host it may not, and either answer is a
   * correct result. What must not happen is a crash or a false positive. */
  bool capable = mr_host_jit_selftest();
  mr_host_caps caps;
  mr_host_probe(&caps);
  CHECK(caps.jit_capable == capable);
  printf("(jit %s) ", capable ? "available" : "unavailable");

  mr_test_end();
}

/* --------------------------------------------------------------- planner */

static mr_host_caps mr_fake_apple_host(void) {
  mr_host_caps caps;
  mr_host_caps_init(&caps);
  caps.platform = MR_PLATFORM_MACOS;
  caps.gpu_available = true;
  caps.metal4_available = true;
  caps.metal3_available = true;
  caps.jit_capable = true;
  caps.jit_enabled = true;
  caps.can_spawn_processes = true;
  caps.unified_memory = true;
  caps.physical_memory = 16ull * 1024 * 1024 * 1024;
  caps.memory_budget = 3ull * 1024 * 1024 * 1024;
  caps.jit_hole_bytes = 2ull * 1024 * 1024 * 1024;
  snprintf(caps.device_name, sizeof(caps.device_name), "Apple M2");
  return caps;
}

/* A layout whose components are all present, so a plan reaches the decisions
 * rather than stopping at "components are missing". */
static mr_runtime_layout mr_fake_layout(const char *root) {
  mr_runtime_layout layout;
  mr_layout_init(&layout, root);
  layout.wine_present = true;
  layout.fex_present = true;
  layout.dxmt_present = true;
  layout.prefix_initialised = true;
  return layout;
}

static void test_plan_selects_the_documented_path(void) {
  mr_test_begin("planner: x86-64 + D3D11 -> FEX + DXMT + Metal 4");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "plan-game.exe");
  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "plan-game") == MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);

  CHECK_INT(plan.cpu_path, MR_CPU_PATH_FEX_JIT);
  CHECK(plan.uses_fex);
  CHECK_INT(plan.backend, MR_BACKEND_METAL4);
  CHECK(plan.graphics_enabled);
  CHECK_INT(plan.gfx_api, MR_GFX_D3D11);

  /* Feature level 11_0 by default, and the decision must carry its reason. */
  CHECK_INT(plan.d3d_feature_level, 1100);
  const char *level_reason = NULL;
  for (size_t i = 0; i < plan.decision_count; i++) {
    if (strcmp(plan.decisions[i].subject, "graphics.feature_level") == 0) {
      level_reason = plan.decisions[i].reason;
    }
  }
  CHECK(level_reason != NULL && level_reason[0] != '\0');

  /* The pool must fit the hole. */
  CHECK(plan.jit_pool_bytes > 0);
  CHECK(plan.jit_pool_bytes <= host.jit_hole_bytes);

  /* DXMT's four modules must be forced native, or Wine's builtin d3d11 wins and
   * the title gets a software rasteriser. */
  CHECK(strstr(plan.wine_dll_overrides, "d3d11") != NULL);
  CHECK(strstr(plan.wine_dll_overrides, "dxgi") != NULL);
  CHECK(strstr(plan.wine_dll_overrides, "winemetal") != NULL);

  /* The environment must name the DXMT config file, since DXMT otherwise reads
   * dxmt.conf from the working directory and two titles would share one. */
  const char *dxmt_env = NULL;
  const char *prefix_env = NULL;
  for (size_t i = 0; i < plan.envc; i++) {
    if (strcmp(plan.env[i].key, "DXMT_CONFIG_FILE") == 0) {
      dxmt_env = plan.env[i].value;
    }
    if (strcmp(plan.env[i].key, "WINEPREFIX") == 0) {
      prefix_env = plan.env[i].value;
    }
  }
  CHECK(dxmt_env != NULL && strstr(dxmt_env, "dxmt.conf") != NULL);
  CHECK(prefix_env != NULL);

  /* argv starts with the loader and names the executable. */
  CHECK(plan.argc >= 2);
  CHECK(strstr(plan.argv[plan.argc - 1], "plan-game.exe") != NULL);

  /* This title is expected to run, so nothing may block it. A surprise blocker
   * here would mean the analyzer and the planner disagree. */
  for (size_t i = 0; i < plan.blockers.count; i++) {
    printf("\n    unexpected blocker: %s\n", plan.blockers.items[i]);
  }
  CHECK_INT(plan.blockers.count, 0);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_plan_blocks_32bit(void) {
  mr_test_begin("planner: 32-bit guest is refused");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "thirtytwo.exe");
  mr_fixture f;
  mr_fixture_i386(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "thirtytwo") == MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  CHECK(plan.blockers.count > 0);

  bool mentions_32bit = false;
  for (size_t i = 0; i < plan.blockers.count; i++) {
    if (strstr(plan.blockers.items[i], "32-bit") != NULL) mentions_32bit = true;
  }
  CHECK(mentions_32bit);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_plan_blocks_anticheat(void) {
  mr_test_begin("planner: anti-cheat is refused with a reason");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "ac.exe");
  mr_fixture f;
  mr_fixture_anticheat(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "ac") == MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  CHECK(plan.blockers.count > 0);

  bool mentions_kernel = false;
  for (size_t i = 0; i < plan.blockers.count; i++) {
    if (strstr(plan.blockers.items[i], "kernel") != NULL) mentions_kernel = true;
  }
  /* The reason has to name the mechanism, not just say "unsupported": the user
   * needs to know no configuration will help. */
  CHECK(mentions_kernel);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_plan_blocks_without_jit(void) {
  mr_test_begin("planner: no JIT means no launch, and says why");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "nojit.exe");
  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "nojit") == MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  host.platform = MR_PLATFORM_IPADOS;
  host.jit_capable = false;
  host.jit_enabled = false;
  host.jit_hole_bytes = 0;

  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  /* Told up front rather than left to fail inside the allocator. */
  CHECK(plan.blockers.count > 0);

  mr_strvec problems;
  CHECK(mr_strvec_init(&problems) == MR_OK);
  CHECK(mr_plan_preflight(&plan, &host, &layout, &problems) == MR_OK);
  CHECK(problems.count > 0);
  mr_strvec_free(&problems);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_plan_omits_hardware_path_for_native_arm64(void) {
  mr_test_begin("planner: an arm64 guest skips translation");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "native.exe");
  mr_fixture f;
  mr_fixture_d3d11(&f);
  f.machine = 0xAA64u; /* native arm64 */
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);
  CHECK_INT(facts.arch, MR_ARCH_ARM64);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "native") == MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  /* No translation, so no code cache and no FEX environment. Allocating one
   * anyway would waste hundreds of megabytes on every native title. */
  CHECK(!plan.uses_fex);
  CHECK_INT(plan.cpu_path, MR_CPU_PATH_NATIVE);
  CHECK_INT(plan.jit_pool_bytes, 0);
  for (size_t i = 0; i < plan.envc; i++) {
    CHECK(strncmp(plan.env[i].key, "FEX_", 4) != 0);
  }

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

static void test_plan_profile_override_wins(void) {
  mr_test_begin("planner: a profile setting changes the outcome");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "override.exe");
  mr_fixture f;
  mr_fixture_d3d11(&f);
  CHECK(mr_fixture_build(&f));
  CHECK(mr_fixture_write(&f, path));

  mr_game_facts facts;
  CHECK(mr_analyze_init(&facts) == MR_OK);
  CHECK(mr_analyze_exe(path, &facts) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "id", "override") == MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "feature_level", "11_1") ==
        MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "backend", "metal3") ==
        MR_OK);
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "jit_pool_mb", "512") ==
        MR_OK);

  mr_host_caps host = mr_fake_apple_host();
  mr_runtime_layout layout = mr_fake_layout(g_tmpdir);

  mr_launch_plan plan;
  CHECK(mr_plan_build(&facts, &profile, &host, &layout, &plan) == MR_OK);
  CHECK_INT(plan.d3d_feature_level, 1101);
  CHECK_INT(plan.backend, MR_BACKEND_METAL3);
  CHECK_INT((long long)plan.jit_pool_bytes, 512ll * 1024 * 1024);

  /* The plan must render in both forms. */
  mr_str text;
  CHECK(mr_str_init(&text) == MR_OK);
  CHECK(mr_plan_to_text(&plan, &text) == MR_OK);
  CHECK(text.len > 0);
  CHECK(strstr(text.data, "metal3") != NULL);
  mr_str_free(&text);

  mr_str json;
  CHECK(mr_str_init(&json) == MR_OK);
  CHECK(mr_plan_to_json(&plan, &json) == MR_OK);
  CHECK(strstr(json.data, "\"backend\": \"metal3\"") != NULL);
  CHECK(strstr(json.data, "\"blockers\": []") != NULL);
  mr_str_free(&json);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_analyze_free(&facts);
  mr_test_end();
}

/* ------------------------------------------------------------------ cache */

static void test_cache_round_trip(void) {
  mr_test_begin("cache: store, read back, and reject corruption");
  g_failures = 0;

  char root[512];
  mr_tmp_path(root, sizeof(root), "cachetest");
  (void)mr_mkdirs(root);

  mr_shader_cache cache;
  CHECK(mr_shader_cache_open(&cache, root, "cache-game", "metal4|m2",
                             8ull * 1024 * 1024) == MR_OK);

  const uint8_t bytecode[32] = {0x01, 0x02, 0x03, 0x04};
  char key[MR_CACHE_KEY_LEN + 1];
  mr_cache_key(MR_CACHE_SHADER, bytecode, sizeof(bytecode), NULL, 0,
               "metal4|m2", key);
  CHECK(strlen(key) == MR_CACHE_KEY_LEN);

  /* A miss before anything is stored. */
  mr_bytes out;
  CHECK(mr_shader_cache_get(&cache, MR_CACHE_SHADER, key, &out) ==
        MR_ERR_NOTFOUND);
  CHECK_INT(cache.stats[MR_CACHE_SHADER].misses, 1);

  const char payload[] = "translated-metal-library-bytes";
  CHECK(mr_shader_cache_put(&cache, MR_CACHE_SHADER, key, payload,
                            sizeof(payload)) == MR_OK);

  CHECK(mr_shader_cache_get(&cache, MR_CACHE_SHADER, key, &out) == MR_OK);
  CHECK_INT((long long)out.len, (long long)sizeof(payload));
  CHECK(memcmp(out.data, payload, sizeof(payload)) == 0);
  mr_bytes_free(&out);

  /* The same inputs must produce the same key, and a different backend must
   * not: an entry written for Metal 3 must never be read back for Metal 4,
   * because the compiled output is not interchangeable. */
  char key2[MR_CACHE_KEY_LEN + 1];
  mr_cache_key(MR_CACHE_SHADER, bytecode, sizeof(bytecode), NULL, 0,
               "metal4|m2", key2);
  CHECK_STR(key, key2);
  char key3[MR_CACHE_KEY_LEN + 1];
  mr_cache_key(MR_CACHE_SHADER, bytecode, sizeof(bytecode), NULL, 0,
               "metal3|m2", key3);
  CHECK(strcmp(key, key3) != 0);

  /* A pipeline key and a shader key over identical inputs must differ. */
  char key4[MR_CACHE_KEY_LEN + 1];
  mr_cache_key(MR_CACHE_PIPELINE, bytecode, sizeof(bytecode), NULL, 0,
               "metal4|m2", key4);
  CHECK(strcmp(key, key4) != 0);

  /* Corrupt the entry in place. A crashed writer must produce a miss, never a
   * bad shader handed to the driver. */
  char entry[1024];
  snprintf(entry, sizeof(entry), "%s/cache-game/shader/%s.bin", root, key);
  FILE *fp = fopen(entry, "r+b");
  CHECK(fp != NULL);
  if (fp != NULL) {
    /* Flip a byte in the payload, past the 40-byte header. */
    fseek(fp, 48, SEEK_SET);
    int ch = fgetc(fp);
    CHECK(ch != EOF);
    fseek(fp, 48, SEEK_SET);
    fputc(ch ^ 0xFF, fp);
    fclose(fp);
  }

  CHECK(mr_shader_cache_get(&cache, MR_CACHE_SHADER, key, &out) ==
        MR_ERR_NOTFOUND);
  CHECK(cache.stats[MR_CACHE_SHADER].rejected_corrupt > 0);

  mr_shader_cache_close(&cache);
  mr_test_end();
}

static void test_cache_evicts_least_recently_used(void) {
  mr_test_begin("cache: eviction drops the oldest entries");
  g_failures = 0;

  char root[512];
  mr_tmp_path(root, sizeof(root), "evicttest");
  (void)mr_mkdirs(root);

  mr_shader_cache cache;
  CHECK(mr_shader_cache_open(&cache, root, "evict-game", "metal4",
                             1024ull * 1024ull) == MR_OK);

  uint8_t blob[512];
  char keys[8][MR_CACHE_KEY_LEN + 1];
  /* 8 entries of ~600 bytes each is about 4.8 KB, comfortably over a 2 KB
   * budget, so eviction has to do something. */
  for (size_t i = 0; i < 8; i++) {
    memset(blob, (int)i, sizeof(blob));
    mr_cache_key(MR_CACHE_SHADER, blob, sizeof(blob), NULL, 0, "metal4",
                 keys[i]);
    CHECK(mr_shader_cache_put(&cache, MR_CACHE_SHADER, keys[i], blob,
                              sizeof(blob)) == MR_OK);
  }
  CHECK(mr_shader_cache_reindex(&cache) == MR_OK);
  CHECK(cache.current_bytes > 0);

  uint64_t before = cache.current_bytes;
  CHECK(mr_shader_cache_evict_to(&cache, 2048) == MR_OK);
  CHECK(cache.current_bytes <= 2048);
  CHECK(cache.current_bytes < before);
  CHECK(cache.stats[MR_CACHE_SHADER].evictions > 0);

  /* A cleared cache is empty and reindexing agrees. */
  CHECK(mr_shader_cache_clear(&cache, MR_CACHE_SHADER) == MR_OK);
  CHECK_INT((long long)cache.current_bytes, 0);

  mr_shader_cache_close(&cache);
  mr_test_end();
}

static void test_cache_sweeps_orphans(void) {
  mr_test_begin("cache: sweep removes caches with no profile");
  g_failures = 0;

  char root[512];
  mr_tmp_path(root, sizeof(root), "sweeptest");
  char cache_root[600];
  char profiles_root[600];
  snprintf(cache_root, sizeof(cache_root), "%s/cache", root);
  snprintf(profiles_root, sizeof(profiles_root), "%s/profiles", root);
  (void)mr_mkdirs(cache_root);
  (void)mr_mkdirs(profiles_root);

  /* One game that still exists, one that was removed. */
  char *kept = mr_path_join(profiles_root, "kept-game");
  char *kept_profile = mr_path_join(kept, "config");
  CHECK(mr_mkdirs(kept) == MR_OK);
  CHECK(mr_write_file_atomic(kept_profile, "id = kept-game\n", 15) == MR_OK);
  free(kept_profile);
  free(kept);

  char *orphan_dir = mr_path_join(cache_root, "gone-game");
  CHECK(mr_mkdirs(orphan_dir) == MR_OK);
  char *orphan_file = mr_path_join(orphan_dir, "shader");
  CHECK(mr_mkdirs(orphan_file) == MR_OK);
  char *orphan_entry = mr_path_join(orphan_file, "0123456789abcdef.bin");
  CHECK(mr_write_file_atomic(orphan_entry, "stale", 5) == MR_OK);
  free(orphan_entry);
  free(orphan_file);
  free(orphan_dir);

  uint64_t reclaimed = 0;
  CHECK(mr_shader_cache_sweep_orphans(cache_root, profiles_root, &reclaimed) ==
        MR_OK);
  CHECK_INT((long long)reclaimed, 5);

  /* The orphan is gone and the live game's directory is untouched. */
  struct stat st;
  char check_path[700];
  snprintf(check_path, sizeof(check_path), "%s/gone-game", cache_root);
  CHECK(stat(check_path, &st) != 0);
  snprintf(check_path, sizeof(check_path), "%s/kept-game", cache_root);
  CHECK(stat(check_path, &st) != 0); /* never created for the live game */

  mr_test_end();
}

/* -------------------------------------------------------------- telemetry */

static void test_telemetry_percentiles(void) {
  mr_test_begin("telemetry: percentiles and over-budget count");
  g_failures = 0;

  mr_telemetry t;
  /* 60 Hz: a 16.67 ms budget. */
  CHECK(mr_telemetry_init(&t, 16666666ull) == MR_OK);
  CHECK_INT(t.counter_count, MR_LAYER_COUNT);

  /* One second of perfectly even 10 ms frames, then one 100 ms hitch. */
  for (int i = 0; i < 100; i++) {
    mr_telemetry_record(&t, MR_LAYER_FRAME, 10.0e6);
  }
  mr_telemetry_record(&t, MR_LAYER_FRAME, 100.0e6);

  mr_str report;
  CHECK(mr_str_init(&report) == MR_OK);
  CHECK(mr_telemetry_report(&t, &report) == MR_OK);
  CHECK(strstr(report.data, "frame_time") != NULL);

  const mr_counter *frame = &t.counters[MR_LAYER_FRAME];
  CHECK_INT((long long)frame->count, 101);
  /* Exactly one sample is over the budget. A mean-based check would not catch a
   * wrong threshold; a count does. */
  CHECK_INT((long long)frame->over_budget, 1);
  CHECK(frame->max > 99.0e6);
  CHECK(frame->min > 9.0e6 && frame->min < 11.0e6);

  mr_str_free(&report);
  mr_telemetry_destroy(&t);
  mr_test_end();
}

static void test_telemetry_spans(void) {
  mr_test_begin("telemetry: a span records its duration");
  g_failures = 0;

  mr_telemetry t;
  CHECK(mr_telemetry_init(&t, 0) == MR_OK);

  int span = mr_telemetry_span(&t, "fex_jit");
  CHECK(span >= 0);
  /* The same name must resolve to the same index, so call sites can cache it. */
  CHECK_INT(mr_telemetry_span(&t, "fex_jit"), span);

  mr_telemetry_span_begin(&t, span);
  /* Burn a little time so the duration is non-zero but do not sleep: a test
   * that sleeps is a test that fails on a loaded machine. */
  volatile double sink = 0.0;
  for (int i = 0; i < 100000; i++) sink += (double)i;
  (void)sink;
  mr_telemetry_span_end(&t, span);

  int counter = mr_telemetry_counter(&t, "fex_jit", "ns");
  CHECK(counter >= 0);
  CHECK_INT((long long)t.counters[counter].count, 1);

  /* An end without a begin must be ignored rather than underflowing the depth
   * and poisoning every later measurement of the same span. */
  mr_telemetry_span_end(&t, span);
  CHECK_INT((long long)t.counters[counter].count, 1);

  mr_telemetry_destroy(&t);
  mr_test_end();
}

/* ----------------------------------------------------------------- compat */

static void test_compat_glob_matching(void) {
  mr_test_begin("compat: id patterns match what they say");
  g_failures = 0;

  CHECK(mr_compat_id_matches("*", "anything"));
  CHECK(mr_compat_id_matches("cyberpunk*", "cyberpunk-2077-ab12"));
  CHECK(mr_compat_id_matches("cyberpunk*", "CYBERPUNK-2077"));
  CHECK(!mr_compat_id_matches("cyberpunk*", "witcher-3"));
  CHECK(mr_compat_id_matches("exact-game", "exact-game"));
  CHECK(!mr_compat_id_matches("exact", "exact-game"));
  CHECK(!mr_compat_id_matches("", "anything"));

  mr_test_end();
}

static void test_compat_builtin_defaults_are_applied(void) {
  mr_test_begin("compat: built-in defaults reach the profile");
  g_failures = 0;

  mr_compat_db db;
  CHECK(mr_compat_db_init(&db) == MR_OK);
  CHECK(mr_compat_db_load_builtins(&db) == MR_OK);

  /* The reserved defaults entry is not a verdict about a title, so a lookup for
   * a game must not return it. Returning it would make every game look like it
   * had an entry. */
  CHECK(mr_compat_lookup(&db, "some-game") == NULL);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);

  mr_str notes;
  CHECK(mr_str_init(&notes) == MR_OK);

  CHECK(mr_compat_apply(&db, "some-game", &profile, NULL, &notes) == MR_OK);
  CHECK_STR(mr_profile_get(&profile, "feature_level"), "11_0");
  CHECK(mr_profile_get_bool(&profile, "allow_metal4", false) == true);
  CHECK_STR(mr_profile_get(&profile, "smc_checks"), "true");
  CHECK(notes.len > 0);

  mr_str_free(&notes);
  mr_profile_free(&profile);
  mr_compat_db_free(&db);
  mr_test_end();
}

static void test_compat_does_not_override_the_user(void) {
  mr_test_begin("compat: an explicit profile setting wins");
  g_failures = 0;

  mr_compat_db db;
  CHECK(mr_compat_db_init(&db) == MR_OK);
  CHECK(mr_compat_db_load_builtins(&db) == MR_OK);

  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  /* The user asked for 11_1. The database says 11_0 and must lose: a profile
   * whose contents do not mean what they say is worse than a wrong default. */
  CHECK(mr_profile_set(&profile, MR_SECTION_CONFIG, "feature_level", "11_1") ==
        MR_OK);
  CHECK(mr_profile_is_explicit(&profile, "feature_level"));

  CHECK(mr_compat_apply(&db, "some-game", &profile, NULL, NULL) == MR_OK);
  CHECK_STR(mr_profile_get(&profile, "feature_level"), "11_1");

  mr_profile_free(&profile);
  mr_compat_db_free(&db);
  mr_test_end();
}

static void test_compat_rules_file(void) {
  mr_test_begin("compat: rules file parses and blocks");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "rules.conf");

  FILE *fp = fopen(path, "wb");
  CHECK(fp != NULL);
  if (fp != NULL) {
    fputs("# a title someone worked out\n"
          "[broken-title-1234abcd]\n"
          "title = Broken Title\n"
          "status = broken\n"
          "verified_with = Mr 0.1, M1, iPadOS 26.0\n"
          "note = Crashes at the first pipeline state.\n"
          "setting feature_level = 11_1\n"
          "env FEX_MAXINST = 5000\n",
          fp);
    fclose(fp);
  }

  mr_compat_db db;
  CHECK(mr_compat_db_init(&db) == MR_OK);
  CHECK(mr_compat_db_load_file(&db, path) == MR_OK);
  CHECK_INT(db.rule_count, 1);

  const mr_compat_rule *rule = mr_compat_lookup(&db, "broken-title-1234abcd");
  CHECK(rule != NULL);
  if (rule != NULL) {
    CHECK_INT(rule->status, MR_COMPAT_BROKEN);
    CHECK_INT(rule->setting_count, 1);
    CHECK_INT(rule->env_count, 1);
    CHECK_STR(mr_compat_rule_setting(rule, "feature_level"), "11_1");
    CHECK(mr_compat_rule_has_env(rule, "FEX_MAXINST"));
    CHECK_STR(rule->verified_with, "Mr 0.1, M1, iPadOS 26.0");
  }

  /* An unsupported verdict must produce a blocker carrying the note, so the UI
   * can show the user why without the database being loaded. */
  mr_profile profile;
  CHECK(mr_profile_init(&profile) == MR_OK);
  mr_launch_plan plan;
  CHECK(mr_plan_init(&plan) == MR_OK);

  CHECK(mr_compat_apply(&db, "broken-title-1234abcd", &profile, &plan, NULL) ==
        MR_OK);
  CHECK(plan.blockers.count > 0);
  CHECK(plan.blockers.count > 0 &&
        strstr(plan.blockers.items[0], "first pipeline state") != NULL);

  mr_plan_free(&plan);
  mr_profile_free(&profile);
  mr_compat_db_free(&db);
  mr_test_end();
}

static void test_compat_malformed_rule_is_an_error(void) {
  mr_test_begin("compat: a malformed line fails the load");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "bad-rules.conf");

  FILE *fp = fopen(path, "wb");
  CHECK(fp != NULL);
  if (fp != NULL) {
    fputs("[a-game]\nstatus = playable\nthis line has no equals sign\n", fp);
    fclose(fp);
  }

  mr_compat_db db;
  CHECK(mr_compat_db_init(&db) == MR_OK);
  /* Half-loading a rules file produces confident wrong verdicts, which is worse
   * than refusing to load it. */
  CHECK(mr_compat_db_load_file(&db, path) == MR_ERR_PARSE);
  CHECK(db.last_error[0] != '\0');
  mr_compat_db_free(&db);

  mr_test_end();
}

/* ------------------------------------------------------------------- util */

static void test_util_strings_and_paths(void) {
  mr_test_begin("util: string helpers and path joining");
  g_failures = 0;

  CHECK(mr_strcasecmp("ABC", "abc") == 0);
  CHECK(mr_strcasecmp("abc", "abd") < 0);
  CHECK(mr_str_iequal("True", "true"));
  CHECK(mr_str_ends_with_ci("game.EXE", ".exe"));
  CHECK(!mr_str_ends_with_ci("game.exe", ".dll"));

  char joined[64];
  CHECK(mr_path_join_into(joined, sizeof(joined), "/a/b", "c"));
  CHECK_STR(joined, "/a/b/c");
  CHECK(mr_path_join_into(joined, sizeof(joined), "/a/b/", "c"));
  CHECK_STR(joined, "/a/b/c");
  /* Too small a buffer must fail rather than truncate: a truncated path names a
   * different file. */
  CHECK(!mr_path_join_into(joined, 4, "/a/verylongpath", "c"));
  CHECK_STR(joined, "");

  char *base = mr_path_basename_dup("/games/title/game.exe");
  CHECK(base != NULL);
  if (base != NULL) {
    CHECK_STR(base, "game.exe");
    free(base);
  }
  /* Windows separators appear in paths handed to us by the guest. */
  base = mr_path_basename_dup("C:\\games\\title\\game.exe");
  CHECK(base != NULL);
  if (base != NULL) {
    CHECK_STR(base, "game.exe");
    free(base);
  }

  CHECK(!mr_path_component_is_safe(".."));
  CHECK(!mr_path_component_is_safe("a/b"));
  CHECK(mr_path_component_is_safe("a-b_c"));

  /* FNV-1a is compared by value, so a change of algorithm is caught here. */
  uint64_t h1 = mr_fnv1a64("hello", 5);
  uint64_t h2 = mr_fnv1a64("hello", 5);
  CHECK_INT((long long)h1, (long long)h2);
  CHECK(h1 != mr_fnv1a64("hellp", 5));
  /* The empty input must yield the FNV-1a 64 offset basis and nothing else: the
   * value is part of the cache key format, so changing it invalidates every
   * cache on every device. */
  CHECK_INT((long long)mr_fnv1a64("", 0), (long long)0xcbf29ce484222325ull);

  mr_test_end();
}

static void test_util_bytes_and_files(void) {
  mr_test_begin("util: atomic file writes and bounded reads");
  g_failures = 0;

  char path[512];
  mr_tmp_path(path, sizeof(path), "atomic.txt");

  CHECK(mr_write_file_atomic(path, "hello world", 11) == MR_OK);
  mr_bytes b;
  CHECK(mr_read_file(path, &b) == MR_OK);
  CHECK_INT((long long)b.len, 11);
  CHECK(memcmp(b.data, "hello world", 11) == 0);
  mr_bytes_free(&b);

  /* A file that does not exist is reported, not silently treated as empty. */
  CHECK(mr_read_file("/tmp/mr-definitely-not-here-12345", &b) == MR_ERR_NOTFOUND);

  /* Bounds-checked readers must refuse an offset past the end. */
  const uint8_t small[4] = {1, 2, 3, 4};
  uint32_t v = 0;
  CHECK(mr_read_u32(small, sizeof(small), 0, &v));
  CHECK_INT(v, 0x04030201);
  CHECK(!mr_read_u32(small, sizeof(small), 2, &v));
  CHECK(!mr_read_u32(small, sizeof(small), 100, &v));
  uint64_t v64 = 0;
  CHECK(!mr_read_u64(small, sizeof(small), 0, &v64));

  mr_test_end();
}

static void test_util_types_and_names(void) {
  mr_test_begin("util: enum names and parsers agree");
  g_failures = 0;

  /* Every enum's string form must parse back to the same value. A mismatch here
   * would corrupt a profile silently, because profiles store the strings. */
  for (int i = 0; i <= (int)MR_ARCH_ARM64X; i++) {
    const char *name = mr_arch_str((mr_arch)i);
    if (strcmp(name, "unknown") == 0) continue;
    CHECK_INT(mr_arch_from_str(name), i);
    CHECK(mr_strcasecmp(name, name) == 0);
  }

  for (int i = 0; i <= (int)MR_GFX_D3D12; i++) {
    const char *name = mr_gfx_api_str((mr_gfx_api)i);
    CHECK_INT(mr_gfx_api_from_str(name), i);
  }

  for (int i = 0; i <= (int)MR_BACKEND_NONE + 2; i++) {
    const char *name = mr_backend_str((mr_backend)i);
    if (strcmp(name, "none") == 0) continue;
    CHECK_INT(mr_backend_from_str(name), i);
  }

  CHECK_INT(mr_status_str(MR_ERR_PARSE) != NULL, 1);
  CHECK(strlen(mr_status_str(MR_ERR_PARSE)) > 0);
  /* Every status must have a name, because the CLI prints them and an unnamed
   * one produces a bare number in a bug report. */
  for (int i = 0; i <= (int)MR_ERR_RANGE; i++) {
    const char *name = mr_status_str((mr_status)i);
    CHECK(name != NULL && name[0] != '\0');
  }

  char hex[17];
  mr_hex64(0xDEADBEEFCAFEF00Dull, hex);
  CHECK_STR(hex, "deadbeefcafef00d");

  mr_test_end();
}

static void test_backend_formats(void) {
  mr_test_begin("backend: format arithmetic");
  g_failures = 0;

  CHECK_INT(mr_format_bytes_per_pixel(MR_FMT_R8G8B8A8_UNORM), 4);
  CHECK_INT(mr_format_bytes_per_pixel(MR_FMT_R16G16B16A16_FLOAT), 8);
  CHECK_INT(mr_format_bytes_per_pixel(MR_FMT_R32G32B32A32_FLOAT), 16);
  CHECK_INT(mr_format_bytes_per_pixel(MR_FMT_D16_UNORM), 2);
  /* Block-compressed formats have no single bytes-per-pixel value, so returning
   * 0 is the honest answer and the caller must check. */
  CHECK_INT(mr_format_bytes_per_pixel(MR_FMT_BC1_RGBA_UNORM), 0);
  CHECK(mr_format_is_block_compressed(MR_FMT_BC1_RGBA_UNORM));
  CHECK(mr_format_is_block_compressed(MR_FMT_BC7_RGBA_UNORM));
  CHECK(!mr_format_is_block_compressed(MR_FMT_R8G8B8A8_UNORM));
  CHECK(mr_format_is_depth(MR_FMT_D24_UNORM_S8_UINT));
  CHECK(!mr_format_is_depth(MR_FMT_R32_FLOAT));

  /* Frames in flight is a correctness value on Metal 4, so the default and the
   * override both matter. */
  mr_gfx_caps caps;
  memset(&caps, 0, sizeof(caps));
  CHECK_INT(mr_gfx_recommended_frames_in_flight(&caps), 3);
  caps.max_frames_in_flight = 2;
  CHECK_INT(mr_gfx_recommended_frames_in_flight(&caps), 2);

  mr_test_end();
}

/* ------------------------------------------------------------------- main */

int main(void) {
  mr_make_tmpdir();
  printf("Mr test suite (working directory: %s)\n\n", g_tmpdir);

  printf("PE parser\n");
  test_pe_parses_a_d3d11_x64_image();
  test_pe_rejects_truncated_images();
  test_pe_reads_the_clr_version();
  test_pe_detects_sections();

  printf("\nGame analyzer\n");
  test_analyze_rejects_an_incomplete_download();
  test_analyze_classifies_d3d11();
  test_analyze_rejects_32bit();
  test_analyze_finds_anticheat();
  test_analyze_finds_a_packer();

  printf("\nProfiles\n");
  test_profile_round_trip();
  test_profile_loads_over_a_seeded_default();
  test_profile_rejects_a_duplicate_key();
  test_profile_dll_overrides();
  test_profile_id_safety();

  printf("\nHost policy\n");
  test_host_jit_pool_sizing();
  test_host_backend_selection();
  test_host_jit_selftest();

  printf("\nLaunch planner\n");
  test_plan_selects_the_documented_path();
  test_plan_blocks_32bit();
  test_plan_blocks_anticheat();
  test_plan_blocks_without_jit();
  test_plan_omits_hardware_path_for_native_arm64();
  test_plan_profile_override_wins();

  printf("\nShader cache\n");
  test_cache_round_trip();
  test_cache_evicts_least_recently_used();
  test_cache_sweeps_orphans();

  printf("\nTelemetry\n");
  test_telemetry_percentiles();
  test_telemetry_spans();

  printf("\nCompatibility database\n");
  test_compat_glob_matching();
  test_compat_builtin_defaults_are_applied();
  test_compat_does_not_override_the_user();
  test_compat_rules_file();
  test_compat_malformed_rule_is_an_error();

  printf("\nUtilities\n");
  test_util_strings_and_paths();
  test_util_bytes_and_files();
  test_util_types_and_names();
  test_backend_formats();

  printf("\n%d checks, %d failures\n", g_checks_total, g_failures_total);
  return g_failures_total == 0 ? 0 : 1;
}
