#include "mr/mr_types.h"

#include <string.h>

const char *mr_status_str(mr_status status) {
  switch (status) {
    case MR_OK: return "ok";
    case MR_ERR_INVALID: return "invalid argument";
    case MR_ERR_NOMEM: return "out of memory";
    case MR_ERR_IO: return "i/o error";
    case MR_ERR_PARSE: return "malformed input";
    case MR_ERR_TRUNCATED: return "truncated input";
    case MR_ERR_UNSUPPORTED: return "unsupported";
    case MR_ERR_NOTFOUND: return "not found";
    case MR_ERR_RANGE: return "out of range";
    case MR_ERR_STATE: return "wrong state";
    case MR_ERR_EXISTS: return "already exists";
  }
  return "unknown error";
}

const char *mr_arch_str(mr_arch arch) {
  switch (arch) {
    case MR_ARCH_I386: return "i386";
    case MR_ARCH_AMD64_OR_ARM64EC: return "x86-64";
    case MR_ARCH_ARM64: return "arm64";
    case MR_ARCH_ARM64X: return "arm64x";
    case MR_ARCH_ARM: return "arm";
    case MR_ARCH_ARMNT: return "armnt";
    case MR_ARCH_IA64: return "ia64";
    case MR_ARCH_UNKNOWN: break;
  }
  return "unknown";
}

const char *mr_gfx_api_str(mr_gfx_api api) {
  switch (api) {
    case MR_GFX_NONE: return "none";
    case MR_GFX_OPENGL: return "opengl";
    case MR_GFX_VULKAN: return "vulkan";
    case MR_GFX_D3D9: return "directx-9";
    case MR_GFX_D3D10: return "directx-10";
    case MR_GFX_D3D11: return "directx-11";
    case MR_GFX_D3D12: return "directx-12";
  }
  return "unknown";
}

const char *mr_cpu_path_str(mr_cpu_path path) {
  switch (path) {
    case MR_CPU_PATH_NATIVE: return "native";
    case MR_CPU_PATH_FEX_JIT: return "fex-jit";
    case MR_CPU_PATH_FEX_AOT: return "fex-aot";
  }
  return "unknown";
}

const char *mr_backend_str(mr_backend backend) {
  switch (backend) {
    case MR_BACKEND_NONE: return "none";
    case MR_BACKEND_METAL3: return "metal3";
    case MR_BACKEND_METAL4: return "metal4";
  }
  return "unknown";
}

mr_arch mr_arch_from_str(const char *s) {
  if (s == NULL) return MR_ARCH_UNKNOWN;
  if (strcmp(s, "i386") == 0) return MR_ARCH_I386;
  if (strcmp(s, "x86-64") == 0 || strcmp(s, "amd64") == 0 ||
      strcmp(s, "arm64ec") == 0) return MR_ARCH_AMD64_OR_ARM64EC;
  if (strcmp(s, "arm64") == 0 || strcmp(s, "aarch64") == 0) return MR_ARCH_ARM64;
  if (strcmp(s, "arm64x") == 0) return MR_ARCH_ARM64X;
  if (strcmp(s, "arm") == 0) return MR_ARCH_ARM;
  if (strcmp(s, "armnt") == 0) return MR_ARCH_ARMNT;
  if (strcmp(s, "ia64") == 0) return MR_ARCH_IA64;
  return MR_ARCH_UNKNOWN;
}

mr_gfx_api mr_gfx_api_from_str(const char *s) {
  if (s == NULL) return MR_GFX_NONE;
  if (strcmp(s, "none") == 0) return MR_GFX_NONE;
  if (strcmp(s, "opengl") == 0) return MR_GFX_OPENGL;
  if (strcmp(s, "vulkan") == 0) return MR_GFX_VULKAN;
  if (strcmp(s, "directx-9") == 0 || strcmp(s, "d3d9") == 0) return MR_GFX_D3D9;
  if (strcmp(s, "directx-10") == 0 || strcmp(s, "d3d10") == 0) return MR_GFX_D3D10;
  if (strcmp(s, "directx-11") == 0 || strcmp(s, "d3d11") == 0) return MR_GFX_D3D11;
  if (strcmp(s, "directx-12") == 0 || strcmp(s, "d3d12") == 0) return MR_GFX_D3D12;
  return MR_GFX_NONE;
}

mr_cpu_path mr_cpu_path_from_str(const char *s) {
  if (s == NULL) return MR_CPU_PATH_NATIVE;
  if (strcmp(s, "fex-jit") == 0 || strcmp(s, "jit") == 0) return MR_CPU_PATH_FEX_JIT;
  if (strcmp(s, "fex-aot") == 0 || strcmp(s, "aot") == 0) return MR_CPU_PATH_FEX_AOT;
  return MR_CPU_PATH_NATIVE;
}

mr_backend mr_backend_from_str(const char *s) {
  if (s == NULL) return MR_BACKEND_NONE;
  if (strcmp(s, "metal4") == 0) return MR_BACKEND_METAL4;
  if (strcmp(s, "metal3") == 0 || strcmp(s, "metal") == 0) return MR_BACKEND_METAL3;
  return MR_BACKEND_NONE;
}
