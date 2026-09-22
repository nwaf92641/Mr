/*
 * What the machine we are running on can actually do.
 *
 * Every adaptive decision in the runtime reads from one of these, and none of
 * them is ever guessed from a marketing name. "M4" does not imply "Metal 4":
 * the backend is chosen because `[device respondsToSelector:@selector(newCommandQueue)]`
 * on the MTL4 path returned an object, and because the OS reported a version
 * that has the MTL4 headers. A device table would need updating for every new
 * chip; a capability probe does not.
 *
 * This struct is also the seam that makes the planner testable off-device. A
 * test constructs an mr_host_caps for an M1 on iPadOS 26 and another for an A15
 * on iOS 26 and asserts that the two produce different plans, without either
 * device being present.
 */
#ifndef MR_HOST_H
#define MR_HOST_H

#include "mr/mr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MR_PLATFORM_UNKNOWN = 0,
  MR_PLATFORM_MACOS,
  MR_PLATFORM_IPADOS,
  MR_PLATFORM_IOS,
  MR_PLATFORM_LINUX, /* host-only: builds and runs the portable core */
} mr_platform;

const char *mr_platform_str(mr_platform p);

typedef struct {
  mr_platform platform;
  char device_name[128];  /* MTLDevice.name, or a stand-in on Linux */
  char os_version[64];    /* "26.0", "18.4" */
  char arch_name[32];     /* "arm64" */

  uint64_t physical_memory;
  /*
   * The largest allocation the OS will let this process hold. On iPadOS this
   * is the jetsam limit and it is the binding constraint on the FEX code
   * cache; on macOS it is effectively physical memory.
   */
  uint64_t memory_budget;
  /*
   * Bytes of *contiguous* address space below the 4 GB guest window. The JIT
   * pool must land in this hole, and it is frequently smaller than the budget
   * suggests -- on a 7 GB device the budget allows a 1760 MB pool while the
   * usable hole is about 1 GB. A pool placed above the guest window hangs on
   * the first translated call, so this value, not memory_budget, sizes it.
   */
  uint64_t jit_hole_bytes;

  /* GPU */
  bool gpu_available;
  int gpu_family; /* MTLGPUFamily raw value; 0 when unknown */
  bool unified_memory;
  bool supports_bc_textures;
  bool supports_mesh_shaders;
  bool supports_hardware_raytracing;
  bool supports_argument_buffers_tier2;
  uint64_t max_buffer_length;
  uint64_t max_threadgroup_memory;
  size_t max_threads_per_threadgroup;
  int max_color_attachments;
  int max_textures_per_stage;

  /* Backends */
  bool metal3_available;
  bool metal4_available; /* MTL4CommandQueue et al. usable */

  /*
   * A Metal device that is not an Apple GPU.
   *
   * The arm64 macOS runner exposes a device called "Apple Paravirtual device":
   * it answers every Metal query and reports no Apple GPU family. It is a Metal
   * device and it is not hardware, and the difference is the difference between
   * "this path ran" and "this path has never run anywhere". Kept as its own
   * field rather than inferred from metal3_available == false, because the two
   * facts belong to different questions and a report that conflated them would
   * let a VM claim REAL APPLE GPU VERIFIED.
   *
   * No Apple API answers this by name. It is derived from the one fact Apple
   * does report: a real Apple GPU matches a family in MTLGPUFamilyApple*, and a
   * paravirtual device matches none.
   */
  bool real_apple_gpu;

  /*
   * MetalFX, optional. Recorded so an enhancement can be offered where the
   * device supports it, never so that the frame path can require it.
   */
  bool metalfx_spatial;
  bool metalfx_temporal;
  bool metalfx_denoise;

  /* JIT / process model */
  bool jit_capable;   /* the CPU can execute freshly written code at all */
  bool jit_enabled;   /* it is enabled right now, not merely permitted */
  bool can_spawn_processes;
  bool can_dlopen;
  bool sandboxed;
  bool has_network;

  /* Storage */
  uint64_t free_disk_bytes;
} mr_host_caps;

/*
 * Fills `caps` from the running machine. Never fails: a field the platform
 * cannot report is left at a conservative default, so a partial probe degrades
 * the plan instead of aborting the launch.
 */
void mr_host_probe(mr_host_caps *caps);

/* Zeroes and conservatively defaults every field. */
void mr_host_caps_init(mr_host_caps *caps);

/*
 * Chooses the JIT translation-cache size.
 *
 * `requested` is what the device's memory budget suggests. `hole` is what was
 * actually measured as contiguous and usable (see jit_hole_bytes). The result
 * is min(requested, hole) clamped to [floor, requestable], where `requestable`
 * is the largest size that still leaves room for the guest and the Wine heap.
 *
 * This is a pure function on purpose: the sizing rule is the one policy in the
 * runtime that has already caused a hard hang in the field, and a pure function
 * is the only form in which it can be regression-tested without a device.
 */
uint64_t mr_host_derive_jit_pool_bytes(const mr_host_caps *caps,
                                       uint64_t requested, uint64_t hole,
                                       uint64_t floor_bytes);

/* Maps a device memory budget to the pool size to ask for. */
uint64_t mr_host_recommended_jit_pool_bytes(const mr_host_caps *caps);

/* True when this host can run guest x86-64 code at all. */
bool mr_host_can_translate_x86(const mr_host_caps *caps);

/*
 * Answers "can this process execute code it just wrote?" by doing it.
 *
 * Capability flags are not evidence. On iPadOS, CS_DEBUGGED can be set with no
 * debugger attached, the allocation then returns zero, and the failure surfaces
 * far away as a placement complaint that has nothing to do with the cause. The
 * only reliable test is to map a page writable, write a stub that returns 42,
 * remap it executable, call it, and check the answer.
 *
 * Returns true only when the stub actually ran and returned 42. Any failure --
 * no W^X permission, a hardened runtime without the JIT entitlement, a
 * sandbox that refuses PROT_EXEC on anonymous memory -- returns false.
 */
bool mr_host_jit_selftest(void);

/*
 * Picks the graphics backend. Prefers Metal 4 and falls back to Metal 3, and
 * records why in `reason` (which may be NULL).
 */
mr_backend mr_host_pick_backend(const mr_host_caps *caps, bool allow_metal4,
                                const char **reason);

/*
 * Writes the graphics capability report: what Metal is there, which GPU family,
 * whether the GPU is real hardware, which MetalFX effects exist, and which
 * backend the runtime would choose and why.
 *
 * One function so the same sentences appear in the launch log, the test output
 * and a bug report, and so the recommendation can be read without re-deriving
 * it. Returns the number of characters written, excluding the terminator.
 */
size_t mr_host_describe_graphics(const mr_host_caps *caps, char *out,
                                 size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MR_HOST_H */
