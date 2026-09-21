/*
 * The Game Analyzer.
 *
 * Reads an executable and works out what it will need. Two rules shape the
 * whole file:
 *
 *   Nothing here executes the analysed binary, and nothing here trusts it.
 *   Launching a title to find out what it imports would run its anti-cheat and
 *   its DRM before the user has agreed to trust them. The analysis is static,
 *   from the image and the names of its neighbours.
 *
 *   Every conclusion is scored and explained. A boolean cannot distinguish
 *   "this links d3d11.dll" from "this links d3d11.dll, calls
 *   D3D11CreateDeviceAndSwapChain, and carries 412 DXBC blobs", and those two
 *   deserve different answers from the profile builder. The evidence list is
 *   what the UI shows and what a bug report contains.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE /* readdir, dirent */
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_analyze.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --------------------------------------------------------------- constants */

/* Graphics modules and what importing each one suggests. */
typedef struct {
  const char *module;
  mr_gfx_api api;
  mr_confidence confidence;
  const char *why;
} mr_gfx_module_rule;

static const mr_gfx_module_rule k_gfx_modules[] = {
    {"d3d11.dll", MR_GFX_D3D11, MR_CONF_MODERATE,
     "imports d3d11.dll, the Direct3D 11 runtime"},
    {"d3d12.dll", MR_GFX_D3D12, MR_CONF_MODERATE,
     "imports d3d12.dll, the Direct3D 12 runtime"},
    {"d3d10.dll", MR_GFX_D3D10, MR_CONF_MODERATE, "imports d3d10.dll"},
    {"d3d10_1.dll", MR_GFX_D3D10, MR_CONF_MODERATE, "imports d3d10_1.dll"},
    {"d3d10core.dll", MR_GFX_D3D10, MR_CONF_MODERATE, "imports d3d10core.dll"},
    {"d3d9.dll", MR_GFX_D3D9, MR_CONF_MODERATE, "imports d3d9.dll"},
    {"d3d8.dll", MR_GFX_D3D9, MR_CONF_MODERATE,
     "imports d3d8.dll, a Direct3D 8 title"},
    {"ddraw.dll", MR_GFX_D3D9, MR_CONF_WEAK,
     "imports ddraw.dll, pre-Direct3D 9"},
    {"opengl32.dll", MR_GFX_OPENGL, MR_CONF_MODERATE, "imports opengl32.dll"},
    {"vulkan-1.dll", MR_GFX_VULKAN, MR_CONF_MODERATE, "imports vulkan-1.dll"},
};

/* Vendor drivers that only appear in a Direct3D title. */
static const char *const k_d3d_vendor_modules[] = {
    "nvapi64.dll", "nvapi.dll", "nvcuda.dll", "amd_ags_x64.dll",
    "amdxc64.dll", "atiadlxx.dll", "amdvlk64.dll", "nvngx_dlss.dll",
};

/* Direct3D entry points. The symbol is the strongest static signal available,
 * because it means the title actually creates a device rather than merely
 * linking a runtime it never calls. */
static const struct {
  const char *module;
  const char *symbol;
  mr_gfx_api api;
  const char *why;
} k_gfx_entry_points[] = {
    {"d3d11.dll", "d3d11createdevice", MR_GFX_D3D11,
     "imports d3d11.dll!D3D11CreateDevice"},
    {"d3d11.dll", "d3d11createdeviceandswapchain", MR_GFX_D3D11,
     "imports d3d11.dll!D3D11CreateDeviceAndSwapChain"},
    {"d3d11.dll", "d3d11createdeferredcontext", MR_GFX_D3D11,
     "imports d3d11.dll!D3D11CreateDeferredContext"},
    {"d3d12.dll", "d3d12createdevice", MR_GFX_D3D12,
     "imports d3d12.dll!D3D12CreateDevice"},
    {"d3d10.dll", "d3d10createdeviceandswapchain", MR_GFX_D3D10,
     "imports d3d10.dll!D3D10CreateDeviceAndSwapChain"},
    {"d3d9.dll", "direct3dcreate9", MR_GFX_D3D9,
     "imports d3d9.dll!Direct3DCreate9"},
    {"vulkan-1.dll", "vkcreateinstance", MR_GFX_VULKAN,
     "imports vulkan-1.dll!vkCreateInstance"},
};

/* Shader compilers shipped with the title: evidence of Direct3D, not proof of
 * which version, so they score weak. */
static const char *const k_shader_compilers[] = {
    "d3dcompiler_43.dll", "d3dcompiler_46.dll", "d3dcompiler_47.dll",
    "dxcompiler.dll", "fxc.exe",
};

/* Visual C++ runtime modules, which is also how the redistributable version is
 * pinned down: the module name encodes the toolset that produced the title. */
static const struct {
  const char *prefix;
  const char *runtime;
} k_vc_modules[] = {
    {"msvcp140", "Visual C++ 2015-2022 (vcruntime140)"},
    {"vcruntime140", "Visual C++ 2015-2022 (vcruntime140)"},
    {"vcruntime140_1", "Visual C++ 2015-2022 (vcruntime140)"},
    {"concrt140", "Visual C++ 2015-2022 (concurrency runtime)"},
    {"vccorlib140", "Visual C++ 2015-2022 (WinRT component support)"},
    {"msvcp120", "Visual C++ 2013"},
    {"msvcr120", "Visual C++ 2013"},
    {"msvcp110", "Visual C++ 2012"},
    {"msvcr110", "Visual C++ 2012"},
    {"msvcp100", "Visual C++ 2010"},
    {"msvcr100", "Visual C++ 2010"},
    {"msvcp90", "Visual C++ 2008"},
    {"msvcr90", "Visual C++ 2008"},
    {"msvcp80", "Visual C++ 2005"},
    {"msvcr80", "Visual C++ 2005"},
    {"msvcp71", "Visual C++ 2003"},
    {"msvcr71", "Visual C++ 2003"},
    {"mfc140", "MFC 2015-2022"},
    {"mfc120", "MFC 2013"},
    {"mfc100", "MFC 2010"},
    {"api-ms-win-crt-runtime", "Universal CRT (api-ms-win-crt-*)"},
    {"ucrtbase", "Universal CRT (ucrtbase)"},
};

/* Anti-cheat, worst first: the order decides which is reported when a title
 * ships two. */
static const struct {
  const char *needle; /* matched case-insensitively against module names */
  mr_anticheat ac;
} k_anticheat_modules[] = {
    {"easyanticheat", MR_AC_EASYANTICHEAT},
    {"easyanticheat_eos", MR_AC_EASYANTICHEAT},
    {"beclient", MR_AC_BATTLEYE},
    {"beservice", MR_AC_BATTLEYE},
    {"bedaisy", MR_AC_BATTLEYE},
    {"equ8", MR_AC_EQU8},
    {"xigncode", MR_AC_XIGNCODE},
    {"x3.xem", MR_AC_XIGNCODE},
    {"vgk.sys", MR_AC_VANGUARD},
    {"vgc.exe", MR_AC_VANGUARD},
};

/* DRM. Steam and the store clients are not obstacles, so they are recorded
 * separately from the ones that are. */
static const struct {
  const char *needle;
  mr_drm drm;
} k_drm_modules[] = {
    {"steam_api64", MR_DRM_STEAM},
    {"steam_api", MR_DRM_STEAM},
    {"steamclient64", MR_DRM_STEAM},
    {"galaxy64", MR_DRM_GOG},
    {"goggame", MR_DRM_GOG},
    {"eossdk", MR_DRM_EPIC},
    {"epic_online", MR_DRM_EPIC},
    {"denuvo", MR_DRM_DENUVO},
    {"securom", MR_DRM_SECUROM},
    {"vmprotect", MR_DRM_VMPROTECT},
};

/* Section names that identify a packer or a protector. A packed image hides its
 * imports from us, so this has to be reported rather than ignored: a title with
 * two imports and a VMProtect section is not a title with two dependencies. */
static const struct {
  const char *section;
  mr_drm drm;
  const char *name;
} k_packer_sections[] = {
    {".vmp0", MR_DRM_VMPROTECT, "VMProtect"},
    {".vmp1", MR_DRM_VMPROTECT, "VMProtect"},
    {".themida", MR_DRM_UNKNOWN_PACKER, "Themida"},
    {".winlice", MR_DRM_UNKNOWN_PACKER, "Themida/WinLicense"},
    {"UPX0", MR_DRM_UNKNOWN_PACKER, "UPX"},
    {"UPX1", MR_DRM_UNKNOWN_PACKER, "UPX"},
    {"UPX2", MR_DRM_UNKNOWN_PACKER, "UPX"},
    {".aspack", MR_DRM_UNKNOWN_PACKER, "ASPack"},
    {".adata", MR_DRM_UNKNOWN_PACKER, "ASPack"},
    {".nsp0", MR_DRM_UNKNOWN_PACKER, "NsPack"},
    {".enigma1", MR_DRM_UNKNOWN_PACKER, "Enigma Protector"},
    {".enigma2", MR_DRM_UNKNOWN_PACKER, "Enigma Protector"},
    {".petite", MR_DRM_UNKNOWN_PACKER, "Petite"},
    {".mpress1", MR_DRM_UNKNOWN_PACKER, "MPRESS"},
    {".mpress2", MR_DRM_UNKNOWN_PACKER, "MPRESS"},
    {".taz", MR_DRM_UNKNOWN_PACKER, "PESpin"},
    {".bind", MR_DRM_STEAM, "Steam DRM stub"},
    {".sdata", MR_DRM_STEAM, "Steam DRM stub"},
    {".bsdiff", MR_DRM_STEAM, "Steam DRM stub"},
};

/* Config files worth looking for, in the order we prefer to read them. */
static const char *const k_config_candidates[] = {
    "config.ini", "settings.ini", "game.ini", "user.ini",
    "video.ini", "videosettings.ini", "graphics.ini", "display.ini",
    "defaultengine.ini", "engine.ini", "gameusersettings.ini",
    "usersettings.ini", "config.cfg", "settings.json", "prefs.json",
    "dxmt.conf", "dxvk.conf",
};

/* Installation markers. A shipped installer is not a game, and telling the user
 * so before they run it is the difference between an error message and a
 * half-installed prefix. Only unambiguous strings are listed: a short token like
 * "SFX" appears in ordinary resources and would flag innocent titles. */
static const char *const k_installer_markers[] = {
    "Inno Setup", "Nullsoft Install", "NSIS Error", "InstallShield",
    "7-Zip SFX", "WiX Toolset", "This installation is corrupted",
};

/* ---------------------------------------------------------------- lifecycle */

typedef struct {
  mr_strvec *lists[8];
  size_t count;
} mr_list_registry;

static void mr_register_lists(mr_game_facts *f, mr_list_registry *reg) {
  reg->lists[0] = &f->required_dlls.items;
  reg->lists[1] = &f->vc_runtime_modules.items;
  reg->lists[2] = &f->net_frame_versions.items;
  reg->lists[3] = &f->config_files.items;
  reg->lists[4] = &f->launcher_candidates.items;
  reg->lists[5] = &f->sibling_executables.items;
  reg->lists[6] = &f->packer_sections.items;
  reg->lists[7] = &f->gfx_modules.items;
  reg->count = 8;
}

mr_status mr_analyze_init(mr_game_facts *facts) {
  if (facts == NULL) return MR_ERR_INVALID;
  memset(facts, 0, sizeof(*facts));

  mr_status st = mr_pe_init(&facts->pe);
  if (st != MR_OK) return st;

  mr_list_registry reg;
  mr_register_lists(facts, &reg);
  for (size_t i = 0; i < reg.count; i++) {
    st = mr_strvec_init(reg.lists[i]);
    if (st != MR_OK) return st;
  }
  return MR_OK;
}

void mr_analyze_free(mr_game_facts *facts) {
  if (facts == NULL) return;
  mr_pe_free(&facts->pe);

  mr_list_registry reg;
  mr_register_lists(facts, &reg);
  for (size_t i = 0; i < reg.count; i++) mr_strvec_free(reg.lists[i]);

  memset(facts, 0, sizeof(*facts));
}

const char *mr_confidence_str(mr_confidence c) {
  switch (c) {
    case MR_CONF_NONE: return "none";
    case MR_CONF_WEAK: return "weak";
    case MR_CONF_MODERATE: return "moderate";
    case MR_CONF_STRONG: return "strong";
    case MR_CONF_CERTAIN: return "certain";
  }
  return "none";
}

const char *mr_anticheat_str(mr_anticheat ac) {
  switch (ac) {
    case MR_AC_NONE: return "none";
    case MR_AC_EASYANTICHEAT: return "Easy Anti-Cheat";
    case MR_AC_BATTLEYE: return "BattlEye";
    case MR_AC_EQU8: return "EQU8";
    case MR_AC_RICOCHET: return "Ricochet";
    case MR_AC_XIGNCODE: return "XIGNCODE3";
    case MR_AC_VANGUARD: return "Riot Vanguard";
    case MR_AC_UNKNOWN_KERNEL_DRIVER: return "unrecognised kernel driver";
  }
  return "none";
}

const char *mr_drm_str(mr_drm drm) {
  switch (drm) {
    case MR_DRM_NONE: return "none";
    case MR_DRM_STEAM: return "Steam";
    case MR_DRM_GOG: return "GOG Galaxy";
    case MR_DRM_EPIC: return "Epic Online Services";
    case MR_DRM_DENUVO: return "Denuvo";
    case MR_DRM_SECUROM: return "SecuROM";
    case MR_DRM_TAGES: return "TAGES";
    case MR_DRM_VMPROTECT: return "VMProtect";
    case MR_DRM_UNKNOWN_PACKER: return "an unidentified packer";
  }
  return "none";
}

const char *mr_engine_str(mr_engine e) {
  switch (e) {
    case MR_ENGINE_UNKNOWN: return "unknown";
    case MR_ENGINE_UNITY: return "Unity";
    case MR_ENGINE_UNREAL: return "Unreal Engine";
    case MR_ENGINE_GODOT: return "Godot";
    case MR_ENGINE_SOURCE: return "Source";
    case MR_ENGINE_SOURCE2: return "Source 2";
    case MR_ENGINE_CRYENGINE: return "CryEngine";
    case MR_ENGINE_GAMEMAKER: return "GameMaker";
    case MR_ENGINE_MONOGAME: return "MonoGame";
    case MR_ENGINE_CUSTOM: return "custom or in-house";
  }
  return "unknown";
}

void mr_analyze_add_evidence(mr_game_facts *facts, mr_confidence confidence,
                             const char *claim, const char *fmt, ...) {
  if (facts == NULL || claim == NULL) return;
  if (facts->evidence_count >= MR_MAX_EVIDENCE) return;

  mr_evidence *e = &facts->evidence[facts->evidence_count];
  memset(e, 0, sizeof(*e));
  snprintf(e->claim, sizeof(e->claim), "%s", claim);
  e->confidence = confidence;

  if (fmt != NULL) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(e->evidence, sizeof(e->evidence), fmt, args);
    va_end(args);
  }
  facts->evidence_count++;
}

/* ------------------------------------------------------------------ helpers */

static void mr_facts_add(mr_strvec *v, const char *s) {
  if (s == NULL || v == NULL) return;
  if (mr_strvec_contains(v, s)) return;
  (void)mr_strvec_push(v, s);
}

/* Case-insensitive substring search over a bounded buffer. Not a general
 * matcher: it exists for marker strings in PE payloads. */
static bool mr_buffer_contains_ci(const uint8_t *data, size_t len,
                                  const char *needle) {
  size_t n = strlen(needle);
  if (n == 0 || len < n) return false;
  for (size_t i = 0; i + n <= len; i++) {
    size_t j = 0;
    while (j < n) {
      unsigned char a = data[i + j];
      unsigned char b = (unsigned char)needle[j];
      if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
      if (a != b) break;
      j++;
    }
    if (j == n) return true;
  }
  return false;
}

static bool mr_starts_with_ci(const char *s, const char *prefix) {
  if (s == NULL || prefix == NULL) return false;
  if (prefix[0] == '\0') return false;
  return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static bool mr_contains_ci(const char *haystack, const char *needle) {
  if (haystack == NULL || needle == NULL) return false;
  size_t n = strlen(needle);
  if (n == 0) return false;
  size_t hlen = strlen(haystack);
  if (hlen < n) return false;
  for (size_t i = 0; i + n <= hlen; i++) {
    size_t j = 0;
    while (j < n) {
      char a = haystack[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
      j++;
    }
    if (j == n) return true;
  }
  return false;
}

/* --------------------------------------------------------------- directory */

typedef enum { MR_ENTRY_EXE = 0, MR_ENTRY_ANY } mr_entry_filter;

typedef struct {
  char name[512];
  uint64_t size;
} mr_dir_entry;

/* Lists at most `max` entries. Returns the count. Sorted by name so the output
 * is stable across filesystems, which matters because it lands in profiles and
 * in bug reports. */
static size_t mr_list_dir(const char *dir, mr_entry_filter filter,
                          mr_dir_entry *out, size_t max) {
  DIR *d = opendir(dir);
  if (d == NULL) return 0;

  size_t count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL && count < max) {
    if (ent->d_name[0] == '.') continue;

    bool is_exe = mr_str_ends_with_ci(ent->d_name, ".exe");
    if (filter == MR_ENTRY_EXE && !is_exe) continue;

    /* Skip directories: a sibling folder is not a sibling executable. */
    char *full = mr_path_join(dir, ent->d_name);
    if (full == NULL) continue;
    struct stat st;
    bool ok = stat(full, &st) == 0 && S_ISREG(st.st_mode);
    uint64_t size = ok ? (uint64_t)st.st_size : 0;
    free(full);
    if (!ok) continue;

    snprintf(out[count].name, sizeof(out[count].name), "%s", ent->d_name);
    out[count].size = size;
    count++;
  }
  (void)closedir(d);

  for (size_t i = 0; i + 1 < count; i++) {
    for (size_t j = i + 1; j < count; j++) {
      if (mr_strcasecmp(out[i].name, out[j].name) > 0) {
        mr_dir_entry tmp = out[i];
        out[i] = out[j];
        out[j] = tmp;
      }
    }
  }
  return count;
}

/* ---------------------------------------------------------------- .NET/CLR */

/*
 * Reads the metadata version string out of a CLR header.
 *
 * The version string is how "does this need a CLR, and which one" becomes a
 * fact rather than a guess. The path is: CLR header -> metadata root RVA ->
 * "BSJB" signature -> VersionLength -> Version.
 */
static void mr_read_clr_version(const mr_pe *pe, const uint8_t *data, size_t len,
                               uint32_t clr_rva, mr_game_facts *facts) {
  size_t clr = mr_pe_rva_to_offset(pe, clr_rva);
  if (clr == 0 || clr + 16 > len) return;

  uint32_t metadata_rva = 0;
  if (!mr_read_u32(data, len, clr + 8, &metadata_rva)) return;
  if (metadata_rva == 0) return;

  size_t root = mr_pe_rva_to_offset(pe, metadata_rva);
  if (root == 0 || root + 16 > len) return;

  uint32_t signature = 0;
  if (!mr_read_u32(data, len, root, &signature)) return;
  if (signature != 0x424A5342u) return; /* "BSJB" */

  uint32_t version_len = 0;
  if (!mr_read_u32(data, len, root + 12, &version_len)) return;
  if (version_len == 0 || version_len > 256) return;

  char version[260];
  mr_read_ascii(data, len, root + 16, version_len, version, sizeof(version));
  if (version[0] != '\0') mr_facts_add(&facts->net_frame_versions.items, version);
}

/* --------------------------------------------------------------- detection */

static void mr_detect_arch(mr_game_facts *facts) {
  const mr_pe *pe = &facts->pe;
  facts->arch = pe->arch;

  switch (pe->arch) {
    case MR_ARCH_AMD64_OR_ARM64EC:
      mr_analyze_add_evidence(
          facts, MR_CONF_CERTAIN, "64-bit guest",
          "PE machine 0x8664: x86-64, or an ARM64EC hybrid which reports the "
          "same machine word");
      break;
    case MR_ARCH_I386:
      facts->is_wow64 = true;
      mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "32-bit guest",
                              "PE machine 0x14C: this needs WoW64 on top of "
                              "the x86-64 path, which this release does not "
                              "cover");
      break;
    case MR_ARCH_ARM64:
      mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "native arm64 guest",
                              "PE machine 0xAA64: runs without CPU "
                              "translation");
      break;
    case MR_ARCH_ARM64X:
      mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "ARM64X hybrid guest",
                              "PE machine 0xA64E: the image contains both "
                              "ARM64EC and native arm64 code");
      break;
    default:
      mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "unsupported guest",
                              "PE machine 0x%04X is not a supported guest "
                              "architecture",
                              (unsigned)pe->machine);
      break;
  }

  if (pe->is_arm64ec) {
    mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "ARM64EC hybrid",
                            "PE machine 0xA64E confirms ARM64EC code is "
                            "present");
  }
}

static void mr_detect_graphics(mr_game_facts *facts, const uint8_t *raw,
                               size_t raw_len) {
  const mr_pe *pe = &facts->pe;
  mr_gfx_api best = MR_GFX_NONE;
  mr_confidence best_conf = MR_CONF_NONE;

  for (size_t i = 0; i < sizeof(k_gfx_modules) / sizeof(k_gfx_modules[0]); i++) {
    const mr_gfx_module_rule *r = &k_gfx_modules[i];
    if (!mr_pe_imports_module_family(pe, r->module)) continue;
    mr_facts_add(&facts->gfx_modules.items, r->module);
    if (r->confidence > best_conf) {
      best = r->api;
      best_conf = r->confidence;
    }
  }

  /* A create-device import outranks a bare link, so it can only raise the
   * verdict, never lower it. */
  for (size_t i = 0;
       i < sizeof(k_gfx_entry_points) / sizeof(k_gfx_entry_points[0]); i++) {
    if (!mr_pe_imports_symbol(pe, k_gfx_entry_points[i].module,
                              k_gfx_entry_points[i].symbol)) {
      continue;
    }
    if (k_gfx_entry_points[i].api == best || best == MR_GFX_NONE) {
      best = k_gfx_entry_points[i].api;
      if (best_conf < MR_CONF_STRONG) best_conf = MR_CONF_STRONG;
    }
    mr_analyze_add_evidence(facts, MR_CONF_STRONG,
                            mr_gfx_api_str(k_gfx_entry_points[i].api),
                            "%s", k_gfx_entry_points[i].why);
  }

  for (size_t i = 0;
       i < sizeof(k_shader_compilers) / sizeof(k_shader_compilers[0]); i++) {
    if (!mr_pe_imports_module_family(pe, k_shader_compilers[i])) continue;
    mr_facts_add(&facts->gfx_modules.items, k_shader_compilers[i]);
    if (best_conf < MR_CONF_WEAK) best_conf = MR_CONF_WEAK;
    mr_analyze_add_evidence(facts, MR_CONF_WEAK, "compiles shaders at runtime",
                            "imports %s", k_shader_compilers[i]);
  }

  for (size_t i = 0;
       i < sizeof(k_d3d_vendor_modules) / sizeof(k_d3d_vendor_modules[0]); i++) {
    if (!mr_pe_imports_module_family(pe, k_d3d_vendor_modules[i])) continue;
    mr_facts_add(&facts->gfx_modules.items, k_d3d_vendor_modules[i]);
    if (best_conf < MR_CONF_WEAK) best_conf = MR_CONF_WEAK;
    mr_analyze_add_evidence(facts, MR_CONF_WEAK, "uses a vendor GPU driver",
                            "imports %s", k_d3d_vendor_modules[i]);
  }

  /* Shipped bytecode: the data the API operates on. Weak on its own (a blob
   * could be dead weight) but together with a link to the runtime it upgrades
   * the answer, because there is no other reason to embed D3D bytecode. */
  if (pe->dxbc_blob_count > 0) {
    if (best == MR_GFX_NONE || best == MR_GFX_D3D11 || best == MR_GFX_D3D10) {
      best = best == MR_GFX_NONE ? MR_GFX_D3D11 : best;
      if (best_conf < MR_CONF_STRONG) best_conf = MR_CONF_STRONG;
    }
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "Direct3D bytecode present",
                            "%u DXBC shader containers embedded in the image",
                            (unsigned)pe->dxbc_blob_count);
  }

  if (pe->dxil_blob_count > 0) {
    facts->needs_shader_model_6 = true;
    mr_analyze_add_evidence(
        facts, MR_CONF_MODERATE, "shader model 6 bytecode",
        "%u DXIL containers: shader model 6, which in practice means Direct3D "
        "12 even when d3d12.dll is not imported directly",
        (unsigned)pe->dxil_blob_count);
  }

  /* A last resort for packed titles, where the import table is a lie. */
  if (best == MR_GFX_NONE && raw != NULL) {
    if (mr_buffer_contains_ci(raw, raw_len, "d3d11")) {
      best = MR_GFX_D3D11;
      best_conf = MR_CONF_WEAK;
      mr_analyze_add_evidence(
          facts, MR_CONF_WEAK, "Direct3D 11",
          "no import table evidence, but the literal string \"d3d11\" appears "
          "in the image; the import table is probably packed");
    }
  }

  facts->gfx = best;
  facts->gfx_confidence = best_conf;
}

static void mr_detect_gfx_modules_present(mr_game_facts *facts) {
  const mr_pe *pe = &facts->pe;

  for (size_t i = 0;
       i < sizeof(k_anticheat_modules) / sizeof(k_anticheat_modules[0]); i++) {
    if (!mr_pe_imports_module_family(pe, k_anticheat_modules[i].needle)) continue;
    if (facts->anticheat == MR_AC_NONE) {
      facts->anticheat = k_anticheat_modules[i].ac;
    }
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "anti-cheat present",
                            "imports %s (%s)", k_anticheat_modules[i].needle,
                            mr_anticheat_str(k_anticheat_modules[i].ac));
  }

  for (size_t i = 0; i < sizeof(k_drm_modules) / sizeof(k_drm_modules[0]); i++) {
    if (!mr_pe_imports_module_family(pe, k_drm_modules[i].needle)) continue;
    /*
     * Store clients are not obstacles in the way a protector is, so the two are
     * ranked rather than overwritten: a Steam-linked title that also carries
     * Denuvo must report Denuvo, and the order they appear in the import table
     * must not decide that.
     */
    bool replace = facts->drm == MR_DRM_NONE || k_drm_modules[i].drm != MR_DRM_STEAM;
    if (replace) facts->drm = k_drm_modules[i].drm;
    mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "store or DRM layer",
                            "imports %s (%s)", k_drm_modules[i].needle,
                            mr_drm_str(k_drm_modules[i].drm));
  }

  for (size_t i = 0;
       i < sizeof(k_packer_sections) / sizeof(k_packer_sections[0]); i++) {
    if (!mr_pe_has_section(pe, k_packer_sections[i].section)) continue;
    mr_facts_add(&facts->packer_sections.items, k_packer_sections[i].name);
    if (facts->drm == MR_DRM_NONE || k_packer_sections[i].drm == MR_DRM_VMPROTECT) {
      facts->drm = k_packer_sections[i].drm;
    }
    mr_analyze_add_evidence(
        facts, MR_CONF_STRONG, "packed or protected image",
        "section %s identifies %s; the import table may be incomplete",
        k_packer_sections[i].section, k_packer_sections[i].name);
  }

  for (size_t i = 0; i < pe->imported_modules.count; i++) {
    const char *mod = pe->imported_modules.items[i];
    for (size_t j = 0;
         j < sizeof(k_vc_modules) / sizeof(k_vc_modules[0]); j++) {
      if (!mr_starts_with_ci(mod, k_vc_modules[j].prefix)) continue;
      mr_facts_add(&facts->vc_runtime_modules.items, k_vc_modules[j].runtime);
    }
  }
  if (facts->vc_runtime_modules.items.count > 0) {
    mr_analyze_add_evidence(facts, MR_CONF_STRONG,
                            "needs a Visual C++ runtime",
                            "%zu runtime module families imported",
                            facts->vc_runtime_modules.items.count);
  }

  for (size_t i = 0; i < pe->imported_modules.count; i++) {
    mr_facts_add(&facts->required_dlls.items, pe->imported_modules.items[i]);
  }
}

static void mr_detect_input(mr_game_facts *facts) {
  const mr_pe *pe = &facts->pe;

  static const char *const k_xinput[] = {"xinput1_1.dll", "xinput1_2.dll",
                                         "xinput1_3.dll", "xinput1_4.dll",
                                         "xinput9_1_0.dll"};
  for (size_t i = 0; i < sizeof(k_xinput) / sizeof(k_xinput[0]); i++) {
    if (mr_pe_imports_module_family(pe, k_xinput[i])) {
      facts->input.wants_xinput = true;
      mr_analyze_add_evidence(facts, MR_CONF_STRONG, "wants a game controller",
                              "imports %s", k_xinput[i]);
      break;
    }
  }

  if (mr_pe_imports_module_family(pe, "dinput8.dll") ||
      mr_pe_imports_module_family(pe, "dinput.dll")) {
    facts->input.wants_directinput = true;
    mr_analyze_add_evidence(facts, MR_CONF_MODERATE,
                            "uses DirectInput for input",
                            "imports dinput8.dll or dinput.dll");
  }

  if (mr_pe_imports_symbol(pe, "user32.dll", "getrawinputdata") ||
      mr_pe_imports_symbol(pe, "user32.dll", "registerrawinputdevices")) {
    facts->input.wants_raw_input = true;
  }
  if (mr_pe_imports_symbol(pe, "user32.dll", "getasynckeystate") ||
      mr_pe_imports_symbol(pe, "user32.dll", "getcursorpos") ||
      mr_pe_imports_symbol(pe, "user32.dll", "setcursorpos")) {
    facts->input.wants_keyboard_mouse = true;
  }

  if (mr_pe_imports_module_family(pe, "sdl2.dll") ||
      mr_pe_imports_module_family(pe, "sdl3.dll")) {
    facts->input.ships_sdl = true;
    mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "input through SDL",
                            "imports SDL; input mapping is the title's own");
  }

  /* A title that asks for nothing is a title that reads no input. Saying so is
   * more useful than assuming a controller will work. */
  if (!facts->input.wants_xinput && !facts->input.wants_directinput &&
      !facts->input.wants_raw_input && !facts->input.wants_keyboard_mouse &&
      !facts->input.ships_sdl) {
    mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "no input API imported",
                            "the import table names no XInput, DirectInput or "
                            "user32 input entry point; the image is probably "
                            "packed or the real code lives in a sibling DLL");
  }
}

static void mr_detect_engine(mr_game_facts *facts, const uint8_t *raw,
                            size_t raw_len) {
  const mr_pe *pe = &facts->pe;

  if (mr_pe_imports_module_family(pe, "unityplayer.dll") ||
      mr_pe_imports_module_family(pe, "mono-2.0-bdwgc.dll") ||
      mr_pe_imports_module_family(pe, "il2cpp.dll")) {
    facts->engine = MR_ENGINE_UNITY;
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "Unity",
                            "imports UnityPlayer.dll or the IL2CPP/Mono "
                            "runtime that Unity ships");
    return;
  }

  /* Unreal's launcher stubs are uniformly named, and the name is reliable
   * because the packaging tool produces it. */
  if (mr_contains_ci(facts->exe_name, "-win64-shipping") ||
      mr_contains_ci(facts->exe_name, "-win64-test") ||
      mr_pe_imports_module_family(pe, "ue4game") ||
      mr_pe_imports_module_family(pe, "ue5game")) {
    facts->engine = MR_ENGINE_UNREAL;
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "Unreal Engine",
                            "executable name or imports match Unreal's "
                            "packaged layout");
    return;
  }

  if (mr_pe_imports_module_family(pe, "tier0.dll") ||
      mr_pe_imports_module_family(pe, "vstdlib.dll") ||
      mr_pe_imports_module_family(pe, "tier0_s.dll")) {
    /* Source 2 also ships these, so check for the newer engine's marker. */
    if (raw != NULL && mr_buffer_contains_ci(raw, raw_len, "source2")) {
      facts->engine = MR_ENGINE_SOURCE2;
      mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "Source 2",
                              "imports tier0/vstdlib and carries a Source 2 "
                              "marker");
    } else {
      facts->engine = MR_ENGINE_SOURCE;
      mr_analyze_add_evidence(facts, MR_CONF_STRONG, "Source",
                              "imports tier0.dll and vstdlib.dll");
    }
    return;
  }

  if (mr_pe_imports_module_family(pe, "crysystem.dll") ||
      mr_pe_imports_module_family(pe, "cryrenderd3d11.dll")) {
    facts->engine = MR_ENGINE_CRYENGINE;
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "CryEngine",
                            "imports CrySystem.dll or CryRenderD3D11.dll");
    return;
  }

  if (mr_pe_imports_module_family(pe, "monogame.framework.dll") ||
      mr_pe_imports_module_family(pe, "monogame.framework")) {
    facts->engine = MR_ENGINE_MONOGAME;
    mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "MonoGame",
                            "imports MonoGame.Framework");
    return;
  }

  if (raw != NULL) {
    if (mr_buffer_contains_ci(raw, raw_len, "godot engine")) {
      facts->engine = MR_ENGINE_GODOT;
      mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "Godot",
                              "the image contains the string \"Godot Engine\"");
      return;
    }
    if (mr_buffer_contains_ci(raw, raw_len, "gamemaker")) {
      facts->engine = MR_ENGINE_GAMEMAKER;
      mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "GameMaker",
                              "the image contains a GameMaker marker");
      return;
    }
  }

  if (facts->packer_sections.items.count == 0) {
    facts->engine = MR_ENGINE_CUSTOM;
    mr_analyze_add_evidence(facts, MR_CONF_WEAK, "no known engine",
                            "nothing in the image identifies a middleware "
                            "engine; this is either an in-house engine or a "
                            "statically linked one");
  }
}

static void mr_detect_installer(mr_game_facts *facts, const uint8_t *raw,
                               size_t raw_len) {
  if (mr_contains_ci(facts->exe_name, "setup") ||
      mr_contains_ci(facts->exe_name, "install") ||
      mr_contains_ci(facts->exe_name, "uninstall")) {
    facts->looks_like_setup = true;
  }

  if (raw != NULL) {
    for (size_t i = 0;
         i < sizeof(k_installer_markers) / sizeof(k_installer_markers[0]); i++) {
      if (!mr_buffer_contains_ci(raw, raw_len, k_installer_markers[i])) continue;
      facts->looks_like_setup = true;
      mr_analyze_add_evidence(facts, MR_CONF_MODERATE, "installer rather than a game",
                              "the image contains the %s marker",
                              k_installer_markers[i]);
      break;
    }
  }

  if (facts->pe.subsystem == 3 && facts->gfx == MR_GFX_NONE) {
    facts->looks_like_setup = true;
  }

  /* A tiny windowless executable that imports the shell and networking is a
   * launcher, not a game. */
  if (facts->pe.size_of_image > 0 && facts->pe.size_of_image < 512u * 1024u &&
      facts->gfx == MR_GFX_NONE) {
    facts->has_launcher_files = true;
    mr_analyze_add_evidence(
        facts, MR_CONF_MODERATE, "probably a launcher",
        "the image is %u bytes and imports no graphics API; the game itself is "
        "likely in a sibling executable",
        (unsigned)facts->pe.size_of_image);
  }
}

/* ------------------------------------------------------------ config files */

/*
 * Looks for a resolution in a small INI-style config.
 *
 * Keys are tried in order of specificity, because a bare "width" in an
 * unrelated section is a common red herring and a wrong resolution silently
 * produces a letterboxed window with no explanation.
 */
static bool mr_scan_ini_resolution(const char *path, int *width, int *height) {
  static const char *const wkeys[] = {
      "screenmanager resolution width", "resolutionx", "screenwidth",
      "displaywidth", "fullscreenwidth", "renderwidth", "width"};
  static const char *const hkeys[] = {
      "screenmanager resolution height", "resolutiony", "screenheight",
      "displayheight", "fullscreenheight", "renderheight", "height"};

  mr_bytes bytes;
  if (mr_read_file(path, &bytes) != MR_OK) return false;
  /* Configs are small. A multi-megabyte file matched against these keys is a
   * false positive waiting to happen. */
  if (bytes.len == 0 || bytes.len > 256u * 1024u) {
    mr_bytes_free(&bytes);
    return false;
  }

  int found_w = 0;
  int found_h = 0;
  char line[512];
  size_t line_len = 0;

  for (size_t i = 0; i <= bytes.len; i++) {
    bool at_end = (i == bytes.len);
    char c = at_end ? '\n' : (char)bytes.data[i];
    if (c != '\n' && c != '\r') {
      if (line_len + 1 < sizeof(line)) line[line_len++] = c;
      continue;
    }
    line[line_len] = '\0';
    line_len = 0;

    char *eq = strchr(line, '=');
    if (eq == NULL) eq = strchr(line, ':');
    if (eq == NULL) continue;

    char key[256];
    size_t klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) klen--;
    size_t kstart = 0;
    while (kstart < klen && (line[kstart] == ' ' || line[kstart] == '\t')) kstart++;
    if (klen - kstart >= sizeof(key)) continue;
    memcpy(key, line + kstart, klen - kstart);
    key[klen - kstart] = '\0';

    long value = strtol(eq + 1, NULL, 10);
    if (value < 320 || value > 16384) continue;

    for (size_t k = 0; k < sizeof(wkeys) / sizeof(wkeys[0]); k++) {
      if (mr_str_iequal(key, wkeys[k])) {
        if (found_w == 0) found_w = (int)value;
        break;
      }
    }
    for (size_t k = 0; k < sizeof(hkeys) / sizeof(hkeys[0]); k++) {
      if (mr_str_iequal(key, hkeys[k])) {
        if (found_h == 0) found_h = (int)value;
        break;
      }
    }
  }

  mr_bytes_free(&bytes);

  if (found_w > 0 && found_h > 0) {
    *width = found_w;
    *height = found_h;
    return true;
  }
  return false;
}

static void mr_scan_directory(mr_game_facts *facts) {
  const char *dir = facts->game_dir;
  if (dir[0] == '\0') return;

  /* Config files: only the names we know, so an unrelated .ini in a game folder
   * cannot be mistaken for a settings file. */
  for (size_t i = 0;
       i < sizeof(k_config_candidates) / sizeof(k_config_candidates[0]); i++) {
    char *path = mr_path_join(dir, k_config_candidates[i]);
    if (path == NULL) continue;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      mr_facts_add(&facts->config_files.items, k_config_candidates[i]);
      if (facts->default_width == 0) {
        int w = 0, h = 0;
        if (mr_scan_ini_resolution(path, &w, &h)) {
          facts->default_width = w;
          facts->default_height = h;
          mr_analyze_add_evidence(facts, MR_CONF_MODERATE,
                                  "default resolution",
                                  "%dx%d, read from %s", w, h,
                                  k_config_candidates[i]);
        }
      }
    }
    free(path);
  }

  mr_dir_entry *entries =
      (mr_dir_entry *)calloc(MR_MAX_SIBLINGS, sizeof(*entries));
  if (entries == NULL) return;

  size_t count = mr_list_dir(dir, MR_ENTRY_EXE, entries, MR_MAX_SIBLINGS);
  for (size_t i = 0; i < count; i++) {
    mr_facts_add(&facts->sibling_executables.items, entries[i].name);
    if (mr_contains_ci(entries[i].name, "launch") ||
        mr_contains_ci(entries[i].name, "start") ||
        mr_contains_ci(entries[i].name, "play")) {
      mr_facts_add(&facts->launcher_candidates.items, entries[i].name);
    }
  }
  free(entries);

  if (facts->launcher_candidates.items.count > 0) {
    mr_analyze_add_evidence(
        facts, MR_CONF_MODERATE, "launcher present",
        "%zu sibling executable(s) look like launchers: %s",
        facts->launcher_candidates.items.count,
        facts->launcher_candidates.items.items[0]);
  }

  /* Unity keeps the game in a sibling folder named after the executable. */
  mr_str gamedata;
  if (mr_str_init(&gamedata) == MR_OK) {
    const char *base = facts->exe_name;
    size_t n = strlen(base);
    if (n > 4) n -= 4; /* strip ".exe" */
    if (mr_str_append(&gamedata, base, n) == MR_OK &&
        mr_str_appendz(&gamedata, "_Data") == MR_OK) {
      char *p = mr_path_join(dir, gamedata.data);
      if (p != NULL) {
        if (mr_path_is_dir(p)) {
          mr_facts_add(&facts->config_files.items, gamedata.data);
          if (facts->engine == MR_ENGINE_UNKNOWN) {
            facts->engine = MR_ENGINE_UNITY;
            mr_analyze_add_evidence(
                facts, MR_CONF_STRONG, "Unity",
                "the sibling folder %s is the layout Unity's build produces",
                gamedata.data);
          }
        }
        free(p);
      }
    }
    mr_str_free(&gamedata);
  }
}

/* ------------------------------------------------------------------- .NET */

static void mr_detect_dotnet(mr_game_facts *facts, const uint8_t *raw,
                            size_t raw_len) {
  const mr_pe *pe = &facts->pe;

  if (pe->has_clr_header) {
    facts->is_managed_net = true;
    mr_analyze_add_evidence(facts, MR_CONF_CERTAIN, "managed .NET assembly",
                            "the image carries a CLR header");
    mr_read_clr_version(pe, raw, raw_len, pe->clr_rva, facts);
    return;
  }

  if (mr_pe_imports_module_family(pe, "mscoree.dll")) {
    facts->is_managed_net = true;
    mr_analyze_add_evidence(facts, MR_CONF_STRONG, "managed .NET assembly",
                            "imports mscoree.dll, the CLR shim");
  }
}

/* ------------------------------------------------------------------- ids */

void mr_analyze_game_id(const mr_game_facts *facts, char out[80]) {
  if (facts == NULL || out == NULL) return;

  /* The id covers the image identity, not just the name: two builds of the
   * same title with different entry points should not silently share one
   * profile, because the settings that work for one may not for the other. */
  uint64_t h = mr_fnv1a64(facts->exe_name, strlen(facts->exe_name));
  h = mr_fnv1a64_update(h, &facts->pe.timestamp, sizeof(facts->pe.timestamp));
  h = mr_fnv1a64_update(h, &facts->pe.size_of_image,
                        sizeof(facts->pe.size_of_image));

  char slug[48];
  size_t n = 0;
  for (const char *p = facts->exe_name; *p != '\0' && n + 1 < sizeof(slug); p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!ok) {
      /* Collapse runs of punctuation into one separator. */
      if (n > 0 && slug[n - 1] != '-') slug[n++] = '-';
      continue;
    }
    slug[n++] = c;
  }
  while (n > 0 && slug[n - 1] == '-') n--;
  slug[n] = '\0';
  if (n == 0) snprintf(slug, sizeof(slug), "game");

  char hex[17];
  mr_hex64(h, hex);
  snprintf(out, 80, "%s-%.8s", slug, hex);
}

bool mr_analyze_is_runnable(const mr_game_facts *facts) {
  if (facts == NULL || !facts->pe.is_valid) return false;
  if (facts->pe.is_dll) return false;
  if (facts->pe.is_truncated) return false;
  if (facts->looks_like_setup) return false;
  if (facts->arch != MR_ARCH_AMD64_OR_ARM64EC &&
      facts->arch != MR_ARCH_ARM64) {
    return false;
  }
  if (facts->gfx != MR_GFX_D3D11) return false;
  return true;
}

/* ------------------------------------------------------------------ entry */

mr_status mr_analyze_exe(const char *exe_path, mr_game_facts *facts) {
  if (exe_path == NULL || facts == NULL) return MR_ERR_INVALID;

  mr_bytes raw;
  mr_status st = mr_read_file(exe_path, &raw);
  if (st != MR_OK) return st;

  st = mr_pe_parse(raw.data, raw.len, &facts->pe);
  if (st != MR_OK) {
    mr_bytes_free(&raw);
    return st;
  }

  snprintf(facts->exe_path, sizeof(facts->exe_path), "%s", exe_path);

  char *base = mr_path_basename_dup(exe_path);
  if (base != NULL) {
    snprintf(facts->exe_name, sizeof(facts->exe_name), "%s", base);
    free(base);
  }

  /* Directory: everything up to the last separator, or "." for a bare name. */
  const char *slash = NULL;
  for (const char *p = exe_path; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') slash = p;
  }
  if (slash != NULL && slash != exe_path) {
    size_t n = (size_t)(slash - exe_path);
    if (n >= sizeof(facts->game_dir)) n = sizeof(facts->game_dir) - 1;
    memcpy(facts->game_dir, exe_path, n);
    facts->game_dir[n] = '\0';
  } else {
    snprintf(facts->game_dir, sizeof(facts->game_dir), ".");
  }

  mr_detect_arch(facts);
  mr_detect_graphics(facts, raw.data, raw.len);
  mr_detect_gfx_modules_present(facts);
  mr_detect_input(facts);
  mr_detect_engine(facts, raw.data, raw.len);
  mr_detect_dotnet(facts, raw.data, raw.len);
  mr_detect_installer(facts, raw.data, raw.len);
  mr_scan_directory(facts);

  /*
   * Evidence, not just a flag. "runnable here: no" with nothing next to it is the
   * kind of output that sends the user looking for a setting to change, and there
   * is no setting that supplies the bytes a partial download is missing.
   */
  if (facts->pe.is_truncated) {
    mr_analyze_add_evidence(
        facts, MR_CONF_CERTAIN, "the file is incomplete",
        "the image ends before the data its section table describes, so this is "
        "part of a file: the header parsing above is reading a real Windows "
        "program that was not copied in full");
  }

  if (facts->pe.has_clr_header || facts->is_managed_net) {
    mr_analyze_add_evidence(
        facts, MR_CONF_STRONG, "managed code needs a CLR",
        "Mr supplies no CLR of its own; Wine can provide one, but managed "
        "titles are outside what this release claims to run");
  }

  mr_bytes_free(&raw);
  return MR_OK;
}
