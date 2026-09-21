/*
 * Every layer in Mr reports failure through mr_status. Nothing in the core
 * aborts or exits: a runtime that runs games cannot afford to convert "this
 * optional feature is unavailable" into "the whole process is gone", because
 * the layers below us (D3D11, DXGI) have documented refusal results that titles
 * are already written to cope with.
 */
#ifndef MR_TYPES_H
#define MR_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MR_OK = 0,
  MR_ERR_INVALID = -1,
  MR_ERR_NOMEM = -2,
  MR_ERR_IO = -3,
  MR_ERR_PARSE = -4,
  MR_ERR_TRUNCATED = -5,
  MR_ERR_UNSUPPORTED = -6,
  MR_ERR_NOTFOUND = -7,
  MR_ERR_RANGE = -8,
  MR_ERR_STATE = -9,
  MR_ERR_EXISTS = -10,
} mr_status;

/* Stable, human-readable name. Never returns NULL. */
const char *mr_status_str(mr_status status);

/*
 * Guest CPU architecture of an imported binary.
 *
 * MR_ARCH_AMD64_OR_ARM64EC covers PE machine 0x8664, which is what both a plain
 * x86-64 image and an ARM64EC hybrid report. Disambiguating the two needs the
 * ARM64EC metadata, not the machine word, so callers that care must ask
 * mr_pe_is_arm64ec().
 */
typedef enum {
  MR_ARCH_UNKNOWN = 0,
  MR_ARCH_I386,
  MR_ARCH_AMD64_OR_ARM64EC,
  MR_ARCH_ARM64,
  /*
   * ARM64X (machine 0xA64E) is the one final-image machine word that guarantees
   * ARM64EC code is present. A pure ARM64EC image reports 0x8664 and is
   * indistinguishable from x86-64 at the machine word alone, so it stays under
   * MR_ARCH_AMD64_OR_ARM64EC rather than getting a guess of its own.
   */
  MR_ARCH_ARM64X,
  MR_ARCH_ARM,
  MR_ARCH_ARMNT,
  MR_ARCH_IA64,
} mr_arch;

/* Graphics API the title is built against, strongest evidence wins. */
typedef enum {
  MR_GFX_NONE = 0,
  MR_GFX_OPENGL,
  MR_GFX_VULKAN,
  MR_GFX_D3D9,
  MR_GFX_D3D10,
  MR_GFX_D3D11,
  MR_GFX_D3D12,
} mr_gfx_api;

/* How guest instructions reach the host CPU. */
typedef enum {
  MR_CPU_PATH_NATIVE = 0, /* arm64 guest: no translation */
  MR_CPU_PATH_FEX_JIT,    /* x86-64 guest translated at run time */
  MR_CPU_PATH_FEX_AOT,    /* x86-64 guest pre-translated into the code cache */
} mr_cpu_path;

/* Which Metal generation the graphics backend drives. */
typedef enum {
  MR_BACKEND_NONE = 0,
  MR_BACKEND_METAL3, /* MTLDevice / MTLCommandQueue, the compatibility path */
  MR_BACKEND_METAL4, /* MTL4CommandQueue + explicit allocators, the fast path */
} mr_backend;

const char *mr_arch_str(mr_arch arch);
const char *mr_gfx_api_str(mr_gfx_api api);
const char *mr_cpu_path_str(mr_cpu_path path);
const char *mr_backend_str(mr_backend backend);

mr_arch mr_arch_from_str(const char *s);
mr_gfx_api mr_gfx_api_from_str(const char *s);
mr_cpu_path mr_cpu_path_from_str(const char *s);
mr_backend mr_backend_from_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* MR_TYPES_H */
