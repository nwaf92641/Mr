/*
 * Host policy: sizing, backend selection, and the pure rules behind both.
 *
 * The platform probe that fills mr_host_caps lives beside this file under
 * platform/, because it is the only part that talks to the OS. Everything that
 * *decides* something from those caps is here, where it can be tested against a
 * made-up device.
 */
#if !defined(_WIN32)
/* Must precede every system header; see the note in mr_util.c. */
#define _POSIX_C_SOURCE 200809L

/*
 * ...and Darwin hides its BSD extensions when that macro is in force, which
 * includes MAP_ANON. _DARWIN_C_SOURCE is Apple's documented way to ask for them
 * back, and it has to be in the same place as the macro above: before the first
 * system header.
 *
 * Two CI runs were spent learning this. The first stopped at
 *
 *   mr_host.c:219:34: error: use of undeclared identifier 'MAP_ANONYMOUS'
 *
 * and the obvious fix -- fall back to MAP_ANON, which is the BSD spelling -- got
 * exactly the same error, because under _POSIX_C_SOURCE this SDK defines
 * neither. Falling back between two names that are both hidden is not a fix.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stdio.h>

#include "mr/mr_host.h"

#include <string.h>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>

/*
 * Darwin calls it MAP_ANON; Linux, and the rest of the Unix world, call it
 * MAP_ANONYMOUS. The name only matters to the compiler, and this file has to
 * compile on both, which it did not: the first arm64 macOS CI run stopped here
 * with "use of undeclared identifier 'MAP_ANONYMOUS'" before anything else in
 * the project was reached.
 */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

const char *mr_platform_str(mr_platform p) {
  switch (p) {
    case MR_PLATFORM_MACOS: return "macos";
    case MR_PLATFORM_IPADOS: return "ipados";
    case MR_PLATFORM_IOS: return "ios";
    case MR_PLATFORM_LINUX: return "linux";
    case MR_PLATFORM_UNKNOWN: break;
  }
  return "unknown";
}

void mr_host_caps_init(mr_host_caps *caps) {
  if (caps == NULL) return;
  memset(caps, 0, sizeof(*caps));
  caps->platform = MR_PLATFORM_UNKNOWN;
  caps->unified_memory = true; /* every Apple Silicon device is unified */
  caps->max_color_attachments = 8;
  caps->max_textures_per_stage = 128;
  caps->max_threads_per_threadgroup = 1024;
}

/*
 * The floor is not a tuning knob. Below about 256 MB the translator spends its
 * time evicting blocks it is about to need again, and the resulting stalls are
 * indistinguishable to the user from the game being slow. It is better to fail
 * the launch with a clear reason than to run in that state.
 */
#define MR_JIT_POOL_FLOOR_BYTES (256ull * 1024ull * 1024ull)

/*
 * Leave the guest and the Wine heap room to exist. The translated code, the
 * guest image, the Wine allocations and the GPU resources all share one address
 * space, and a pool that consumes the hole the guest needed is a launch that
 * hangs instead of one that reports a size.
 */
#define MR_JIT_POOL_HEADROOM_BYTES (512ull * 1024ull * 1024ull)

uint64_t mr_host_derive_jit_pool_bytes(const mr_host_caps *caps,
                                       uint64_t requested, uint64_t hole,
                                       uint64_t floor_bytes) {
  if (caps == NULL) return 0;

  uint64_t floor = floor_bytes != 0 ? floor_bytes : MR_JIT_POOL_FLOOR_BYTES;

  /* Never ask for more than the measured hole will hold. This is the whole
   * point of the function: pinning address space moves the frontier, it does
   * not create a gap, and a pool that does not fit lands somewhere that makes
   * the first translated call hang rather than fault. */
  uint64_t usable = hole;
  if (usable > MR_JIT_POOL_HEADROOM_BYTES) {
    usable -= MR_JIT_POOL_HEADROOM_BYTES;
  } else {
    usable = 0;
  }

  uint64_t chosen = requested < usable ? requested : usable;

  /* Also cap by the memory budget itself: address space is not the only limit,
   * and the jetsam accountant does not care that the mapping was cheap. */
  if (caps->memory_budget != 0) {
    uint64_t from_budget = caps->memory_budget / 2;
    if (chosen > from_budget) chosen = from_budget;
  }

  if (chosen < floor) {
    /* Refuse rather than shrink further. A caller that gets 0 knows to report
     * "this device cannot host the translator" instead of running badly. */
    return 0;
  }
  return chosen;
}

uint64_t mr_host_recommended_jit_pool_bytes(const mr_host_caps *caps) {
  if (caps == NULL) return 0;
  if (caps->memory_budget == 0) return 0;

  /*
   * Anchored on the only configuration that has been validated on hardware:
   * a 4096 MB budget yields 896 MB. Scaling is sublinear because the guest and
   * the Wine heap grow with the same memory, so the pool cannot take the whole
   * increment. Devices at or below the anchor get exactly the anchor value.
   */
  const uint64_t anchor_budget = 4096ull * 1024ull * 1024ull;
  const uint64_t anchor_pool = 896ull * 1024ull * 1024ull;

  if (caps->memory_budget <= anchor_budget) return anchor_pool;

  uint64_t extra = caps->memory_budget - anchor_budget;
  uint64_t scaled = anchor_pool + extra / 4; /* a quarter of the surplus */

  /* Hard ceiling: past this the pool is larger than the hole it must live in on
   * every device seen so far, and the extra size buys nothing because the cache
   * is not the bottleneck at that point. */
  const uint64_t ceiling = 1792ull * 1024ull * 1024ull;
  return scaled > ceiling ? ceiling : scaled;
}

bool mr_host_can_translate_x86(const mr_host_caps *caps) {
  if (caps == NULL) return false;
  if (!caps->jit_capable) return false;
  if (!caps->jit_enabled) return false;
  /* Translation needs to mmap, write, then execute from the same pages, which
   * iPadOS permits only while a debugger is attached with the JIT entitlement.
   * A host that cannot do it is not a degraded host, it is an unusable one. */
  return true;
}

/*
 * Metal 4 or nothing.
 *
 * This function used to prefer Metal 4 and fall back to Metal 3, and the fallback
 * is gone on purpose. Mr is an Apple Silicon runtime whose graphics target is
 * Metal 4: Metal 3 is not a runtime path here, it is the conformance harness in
 * runtime/metal/ and a comparison point. A fallback that quietly gave a Metal 3
 * frame path would hide exactly the thing this project needs to see -- that Metal 4
 * is not working on this device, or on this OS, or in this build -- and the failure
 * would then be reported as success by the layer above.
 *
 * So every path out of here either selects Metal 4 or names the reason there is no
 * target. The reasons are the deliverable: "UNSUPPORTED TARGET" with the fact that
 * decided it, never a backend the caller did not ask for.
 */
mr_backend mr_host_pick_backend(const mr_host_caps *caps, bool allow_metal4,
                                const char **reason) {
  static const char *k_no_gpu = "no Metal device is available to this process";
  static const char *k_paravirtual =
      "the only Metal device is a paravirtual device, which is a device and not "
      "an Apple GPU: this machine can build the graphics path and cannot verify "
      "it, so REQUIRES REAL APPLE GPU VALIDATION";
  static const char *k_metal4_disabled =
      "Metal 4 is disabled in this profile, and it is the only graphics target "
      "this runtime has: there is no Metal 3 path to fall back to, so nothing was "
      "selected. UNSUPPORTED TARGET";
  static const char *k_os =
      "the operating system predates Metal 4, which needs macOS or iPadOS 26 or "
      "later. Metal 3 is not a runtime path for this project, so nothing was "
      "selected. UNSUPPORTED TARGET";
  static const char *k_probe =
      "the device did not return an MTL4 command queue, so this GPU or OS "
      "combination has no Metal 4 support. Metal 3 is not a runtime path for this "
      "project, so nothing was selected. UNSUPPORTED TARGET";
  static const char *k_chosen = "this device and OS support Metal 4";

  if (reason != NULL) *reason = NULL;
  if (caps == NULL) {
    if (reason != NULL) *reason = k_no_gpu;
    return MR_BACKEND_NONE;
  }
  if (!caps->gpu_available) {
    if (reason != NULL) *reason = k_no_gpu;
    return MR_BACKEND_NONE;
  }

  /*
   * A paravirtual device before anything else. It reports no GPU family and no
   * Metal 4 queue, so the Metal 4 checks below would describe it as a device that
   * cannot do Metal 4, which is true and not the useful sentence. The useful one is
   * that this machine has no GPU to verify against.
   */
  if (!caps->real_apple_gpu) {
    if (reason != NULL) *reason = k_paravirtual;
    return MR_BACKEND_NONE;
  }

  /*
   * The device and OS facts come before the profile setting, because a profile
   * cannot enable something the hardware does not have, and answering a hardware
   * question with "you turned it off" sends the reader to the wrong place.
   *
   * "Too old" and "present in the SDK but refused here" are kept apart: only one of
   * them is worth updating a machine over.
   */
  if (!caps->metal4_available) {
    if (reason != NULL) {
      int major = 0;
      if (caps->os_version[0] >= '0' && caps->os_version[0] <= '9') {
        major = caps->os_version[0] - '0';
        if (caps->os_version[1] >= '0' && caps->os_version[1] <= '9') {
          major = major * 10 + (caps->os_version[1] - '0');
        }
      }
      *reason = (major > 0 && major < 26) ? k_os : k_probe;
    }
    return MR_BACKEND_NONE;
  }

  if (!allow_metal4) {
    if (reason != NULL) *reason = k_metal4_disabled;
    return MR_BACKEND_NONE;
  }

  if (reason != NULL) *reason = k_chosen;
  return MR_BACKEND_METAL4;
}

/* ------------------------------------------------------------ jit self-test */

/*
 * A stub that returns 42, in the host's own instruction set.
 *
 * Emitted as bytes rather than written as a function so that the test cannot be
 * folded into a constant by the compiler, and so the same source works on the
 * development host and on the device.
 *
 *   arm64   movz w0, #42 ; ret    -> 40 05 80 52  c0 03 5f d6
 *   x86-64  mov eax, 42 ; ret     -> b8 2a 00 00 00  c3
 */
#if defined(__aarch64__)
static const unsigned char k_jit_stub[] = {0x40, 0x05, 0x80, 0x52,
                                           0xc0, 0x03, 0x5f, 0xd6};
#elif defined(__x86_64__) || defined(__i386__)
static const unsigned char k_jit_stub[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
#else
static const unsigned char k_jit_stub[] = {0x00};
#endif

bool mr_host_jit_selftest(void) {
#if defined(_WIN32)
  return false;
#else
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return false;
  size_t len = (size_t)page;

  /*
   * Map writable and not executable, then flip. Asking for PROT_WRITE|PROT_EXEC
   * in the first call would defeat the test: the platforms that matter refuse it
   * precisely because that is the W^X bypass, and the ones that allow it are not
   * the ones we are trying to detect.
   */
  void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return false;

  memcpy(mem, k_jit_stub, sizeof(k_jit_stub));

  bool ok = false;
  if (mprotect(mem, len, PROT_READ | PROT_EXEC) == 0) {
    /* The store above landed in the data cache; on arm64 the instruction fetch
     * may otherwise see stale bytes. */
#if defined(__aarch64__)
    __builtin___clear_cache((char *)mem, (char *)mem + sizeof(k_jit_stub));
#endif
    /*
     * ISO C does not allow casting an object pointer to a function pointer, and
     * -Wpedantic is worth keeping on. A union performs the same conversion
     * without the cast; the standard leaves the result implementation-defined,
     * which is exactly the latitude mmap+mprotect already relies on.
     */
    union {
      void *obj;
      int (*fn)(void);
    } caster;
    caster.obj = mem;
    ok = (caster.fn() == 42);
  }

  (void)munmap(mem, len);
  return ok;
#endif
}


size_t mr_host_describe_graphics(const mr_host_caps *caps, char *out,
                                 size_t cap) {
  if (out == NULL || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (caps == NULL) {
    return (size_t)snprintf(out, cap, "graphics: no capability record\n");
  }

  const char *reason = NULL;
  mr_backend chosen = mr_host_pick_backend(caps, true, &reason);
  int n = snprintf(out, cap,
                   "graphics:\n"
                   "  Metal available ..... %s\n"
                   "  Metal 3 ............ %s\n"
                   "  Metal 4 ............ %s\n"
                   "  real Apple GPU ..... %s\n"
                   "  GPU family ......... %d\n"
                   "  unified memory ..... %s\n"
                   "  MetalFX ............ spatial %s, temporal %s, denoise %s\n"
                   "  target ............. Metal 4, and there is no Metal 3 runtime "
                   "path\n"
                   "  recommended path ... %s\n"
                   "  because ............ %s\n",
                   caps->gpu_available ? "yes" : "no",
                   caps->metal3_available ? "yes" : "no",
                   caps->metal4_available ? "yes" : "no",
                   caps->real_apple_gpu
                       ? "yes"
                       : "no, so no graphics path can be verified here",
                   caps->gpu_family, caps->unified_memory ? "yes" : "no",
                   caps->metalfx_spatial ? "yes" : "no",
                   caps->metalfx_temporal ? "yes" : "no",
                   caps->metalfx_denoise ? "yes" : "no",
                   mr_backend_str(chosen), reason == NULL ? "" : reason);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1u;
}
