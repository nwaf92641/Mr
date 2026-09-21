/*
 * The Game Analyzer's derived view of a title.
 *
 * The analyzer does not answer questions with booleans. A boolean cannot say
 * how sure it is or why, and the user's next action depends on both: "the EXE
 * imports d3d11.dll and contains 412 DXBC blobs" is actionable, "D3D11: yes" is
 * not. Every conclusion is therefore a scored claim with the evidence that
 * produced it, and the profile builder is free to prefer lower-scoring evidence
 * when a higher-scoring claim was contradicted.
 */
#ifndef MR_ANALYZE_H
#define MR_ANALYZE_H

#include "mr/mr_pe.h"
#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MR_MAX_EVIDENCE 24
#define MR_MAX_SIBLINGS 64

typedef enum {
  MR_CONF_NONE = 0,
  MR_CONF_WEAK = 1,      /* a plausible coincidence */
  MR_CONF_MODERATE = 2,  /* a link to the API, but no proof of use */
  MR_CONF_STRONG = 3,    /* the API plus the data it operates on */
  MR_CONF_CERTAIN = 4,   /* the entry point or an explicit version marker */
} mr_confidence;

const char *mr_confidence_str(mr_confidence c);

typedef struct {
  char claim[128];     /* what was concluded */
  char evidence[256];  /* the observation that produced it */
  mr_confidence confidence;
} mr_evidence;

typedef struct {
  mr_strvec items;
} mr_strlist;

/* Anti-cheat and DRM are reported separately from ordinary dependencies,
 * because their presence changes the verdict rather than the configuration. */
typedef enum {
  MR_AC_NONE = 0,
  MR_AC_EASYANTICHEAT,
  MR_AC_BATTLEYE,
  MR_AC_EQU8,
  MR_AC_RICOCHET,
  MR_AC_XIGNCODE,
  MR_AC_VANGUARD,
  MR_AC_UNKNOWN_KERNEL_DRIVER,
} mr_anticheat;

typedef enum {
  MR_DRM_NONE = 0,
  MR_DRM_STEAM,
  MR_DRM_GOG,
  MR_DRM_EPIC,
  MR_DRM_DENUVO,
  MR_DRM_SECUROM,
  MR_DRM_TAGES,
  MR_DRM_VMPROTECT,
  MR_DRM_UNKNOWN_PACKER,
} mr_drm;

typedef enum {
  MR_ENGINE_UNKNOWN = 0,
  MR_ENGINE_UNITY,
  MR_ENGINE_UNREAL,
  MR_ENGINE_GODOT,
  MR_ENGINE_SOURCE,
  MR_ENGINE_SOURCE2,
  MR_ENGINE_CRYENGINE,
  MR_ENGINE_GAMEMAKER,
  MR_ENGINE_MONOGAME,
  MR_ENGINE_CUSTOM,
} mr_engine;

const char *mr_anticheat_str(mr_anticheat ac);
const char *mr_drm_str(mr_drm drm);
const char *mr_engine_str(mr_engine e);

/* Input devices the title asks for. A game that never imports XInput or
 * DirectInput honestly reports "no controller"; it does not get one. */
typedef struct {
  bool wants_xinput;
  bool wants_directinput;
  bool wants_raw_input;
  bool wants_keyboard_mouse;
  bool ships_sdl;
} mr_input_needs;

typedef struct {
  mr_pe pe;

  char exe_path[1024];
  char exe_name[256];
  char game_dir[1024];

  mr_arch arch;
  mr_gfx_api gfx;
  mr_confidence gfx_confidence;
  mr_engine engine;
  mr_anticheat anticheat;
  mr_drm drm;

  bool is_wow64;          /* 32-bit on a 64-bit host */
  bool is_managed_net;    /* needs a CLR, which Mr does not provide */
  bool looks_like_setup;  /* installer rather than a game */
  bool has_launcher_files;
  bool requires_elevation;

  mr_strlist required_dlls;      /* direct imports */
  mr_strlist vc_runtime_modules;
  mr_strlist net_frame_versions; /* from the CLR metadata version string */
  mr_strlist config_files;       /* found next to the EXE */
  mr_strlist launcher_candidates;
  mr_strlist sibling_executables;
  mr_strlist packer_sections; /* .vmp0, UPX0, .themida ... */
  mr_strlist gfx_modules;

  mr_input_needs input;

  uint32_t dxbc_blob_count;
  uint32_t dxil_blob_count;
  bool needs_shader_model_6;

  int default_width;
  int default_height;

  mr_evidence evidence[MR_MAX_EVIDENCE];
  size_t evidence_count;
} mr_game_facts;

mr_status mr_analyze_init(mr_game_facts *facts);
void mr_analyze_free(mr_game_facts *facts);

/*
 * Analyses a single executable. Uses only the file itself plus the names of
 * its siblings: Mr never executes the imported binary to discover what it
 * needs, because doing so would run unvetted anti-cheat and DRM payloads
 * before the user has decided to trust them.
 */
mr_status mr_analyze_exe(const char *exe_path, mr_game_facts *facts);

/* Records a claim. Silently drops the claim once MR_MAX_EVIDENCE is reached;
 * evidence is diagnostic, so exhausting the budget is not an error. */
void mr_analyze_add_evidence(mr_game_facts *facts, mr_confidence confidence,
                             const char *claim, const char *fmt, ...);

/* Derives a stable, filesystem-safe id for the title, e.g. "thumper-1f3a9c22".
 * The hash covers the EXE name and the PE timestamp/size-of-image, so two
 * different builds of the same title do not silently share a profile. */
void mr_analyze_game_id(const mr_game_facts *facts, char out[80]);

/* True when the evidence supports running the title at all on this runtime. */
bool mr_analyze_is_runnable(const mr_game_facts *facts);

#ifdef __cplusplus
}
#endif

#endif /* MR_ANALYZE_H */
