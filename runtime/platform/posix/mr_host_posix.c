/*
 * Host probe for the non-Apple build.
 *
 * This exists so the portable core, the analyzer and the test suite can run on
 * a Linux development machine. It deliberately reports gpu_available = false:
 * the planner then refuses to produce a runnable plan, which is the honest
 * outcome. A probe that fabricated plausible Metal capabilities here would let a
 * test pass that proves nothing about the device.
 */
#if defined(__linux__)
#define _DEFAULT_SOURCE /* sysinfo */
#endif
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* sysconf, statvfs */
#endif

#include "mr/mr_host.h"

#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

/* Bounded copy that cannot trip -Wformat-truncation on a fixed-size field. */
static void mr_copy_field(char *dst, size_t dst_cap, const char *src) {
  if (dst == NULL || dst_cap == 0) return;
  size_t n = src != NULL ? strlen(src) : 0;
  if (n >= dst_cap) n = dst_cap - 1;
  if (n != 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

void mr_host_probe(mr_host_caps *caps) {
  if (caps == NULL) return;
  mr_host_caps_init(caps);

  caps->platform = MR_PLATFORM_LINUX;
  caps->can_spawn_processes = true;
  caps->can_dlopen = true;
  caps->has_network = true;
  caps->sandboxed = false;
  caps->gpu_available = false;
  caps->metal3_available = false;
  caps->metal4_available = false;

  /*
   * Run the real W^X test rather than asserting a value. On a development host
   * it normally succeeds, which is what makes the self-test itself verifiable
   * here instead of only on a device.
   */
  caps->jit_capable = mr_host_jit_selftest();
  caps->jit_enabled = caps->jit_capable;

  struct utsname uts;
  if (uname(&uts) == 0) {
    mr_copy_field(caps->arch_name, sizeof(caps->arch_name), uts.machine);
  } else {
    mr_copy_field(caps->arch_name, sizeof(caps->arch_name), "unknown");
  }

#if defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    uint64_t unit = (uint64_t)info.mem_unit;
    uint64_t total = (uint64_t)info.totalram * unit;
    caps->physical_memory = total;
    /*
     * There is no jetsam accountant here, so the budget is physical memory.
     * The field earns its keep on Apple platforms; on Linux it exists so the
     * sizing maths has something coherent to work with.
     */
    caps->memory_budget = total;
  }
#endif

  /*
   * Report no usable JIT hole. The hole is a property of the guest address-space
   * layout below the 4 GB window, which only the Apple probe can measure;
   * claiming one here would let the planner size a pool that could never be
   * placed, and a badly placed pool hangs the first translated call.
   */
  caps->jit_hole_bytes = 0;

  struct statvfs vfs;
  if (statvfs(".", &vfs) == 0) {
    caps->free_disk_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
  }

  mr_copy_field(caps->device_name, sizeof(caps->device_name), "development host");
  mr_copy_field(caps->os_version, sizeof(caps->os_version), "unknown");
}
