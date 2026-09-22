/*
 * Host probe for Apple platforms.
 *
 * This is the file the header calls "the seam". Every adaptive decision in the
 * runtime -- pool size, backend choice, whether to launch at all -- reads
 * mr_host_caps, and this is the only place those values come from a real machine.
 *
 * It is written to be honest rather than flattering. A field the platform will
 * not report is left at zero or its conservative default, because the planner
 * treats a zero as a reason to refuse and a fabricated value as a reason to
 * proceed. The one measurement that cannot be approximated is the JIT hole: a
 * pool placed outside it does not fail, it hangs on the first translated call,
 * which is why the hole is measured by walking the address space rather than
 * inferred from the memory budget.
 *
 * The Metal questions are answered next door in mr_metal_probe.m, which is the
 * only Objective-C in the project. Keeping the split here means this file is
 * plain C -- sysctl, statfs and Mach -- and can be reasoned about without an
 * Objective-C runtime in the picture.
 */
#include "mr/mr_host.h"

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <string.h>

#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include "mr_metal_probe.h"
#include "mr_metalfx_probe.h"

/* Bounded copy that cannot trip -Wformat-truncation on a fixed-size field. */
static void mr_copy_field(char *dst, size_t dst_cap, const char *src) {
  if (dst == NULL || dst_cap == 0) return;
  size_t n = src != NULL ? strlen(src) : 0;
  if (n >= dst_cap) n = dst_cap - 1;
  if (n != 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

/* A sysctl by name, as a number. Returns 0 when the key does not exist. */
static uint64_t mr_sysctl_u64(const char *name) {
  uint64_t value = 0;
  size_t len = sizeof(value);
  if (sysctlbyname(name, &value, &len, NULL, 0) != 0) return 0;
  return value;
}

static void mr_sysctl_str(const char *name, char *dst, size_t cap) {
  char buf[256];
  size_t len = sizeof(buf);
  if (sysctlbyname(name, buf, &len, NULL, 0) != 0) return;
  /* A string sysctl reports its length including the terminator, and reports
   * nothing at all as zero. */
  if (len == 0 || len > sizeof(buf)) return;
  buf[len - 1] = '\0';
  mr_copy_field(dst, cap, buf);
}

/*
 * Measures the largest free region below the 4 GB guest window.
 *
 * Walks the task's own address space rather than asking for a number, because
 * there is no number to ask for: it is a property of how the guest image, the
 * runtime's own mappings and the system frameworks happen to be laid out this
 * launch. Regions the kernel reports as unallocated are the candidates, and the
 * largest of them is what a JIT pool can actually be pinned into.
 *
 * This is the one field in the probe that the runtime cannot recover from
 * getting wrong -- see mr_host_derive_jit_pool_bytes, which spends this value --
 * so it is also the field to verify first on a new device.
 */
static uint64_t mr_measure_jit_hole(void) {
  const mach_vm_address_t limit = (mach_vm_address_t)0x100000000ull; /* 4 GB */
  mach_vm_address_t address = 0;
  uint64_t largest = 0;
  size_t regions = 0;

  /*
   * The walk is bounded by the region count as well as by the limit. A
   * pathological address space should cost a bad pool size, not a launch that
   * never finishes probing.
   */
  const size_t k_max_regions = 100000;

  while (address < limit && regions < k_max_regions) {
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;

    kern_return_t kr =
        mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &count, &object_name);
    if (object_name != MACH_PORT_NULL) {
      mach_port_deallocate(mach_task_self(), object_name);
    }
    if (kr != KERN_SUCCESS || size == 0) break;

    regions++;

    /* An unallocated region is the only place a new mapping can be pinned, and
     * only if it stops before the guest window does. */
    if (info.protection == VM_PROT_NONE) {
      mach_vm_address_t end = address + size;
      if (end > limit) end = limit;
      if (end > address) {
        uint64_t gap = (uint64_t)(end - address);
        if (gap > largest) largest = gap;
      }
    }

    mach_vm_address_t next = address + size;
    if (next <= address) break; /* wrapped: stop rather than loop */
    address = next;
  }

  return largest;
}

void mr_host_probe(mr_host_caps *caps) {
  if (caps == NULL) return;
  mr_host_caps_init(caps);

#if TARGET_OS_OSX
  caps->platform = MR_PLATFORM_MACOS;
#else
  /*
   * iPadOS and iOS share a kernel and an SDK; the difference is the device.
   * hw.machine is the documented way to tell them apart without linking UIKit
   * into a runtime that has no business displaying anything: iPad models report
   * "iPad13,4" and phones report "iPhone14,2".
   */
  char machine[128];
  machine[0] = '\0';
  mr_sysctl_str("hw.machine", machine, sizeof(machine));
  caps->platform = strncmp(machine, "iPad", 4) == 0 ? MR_PLATFORM_IPADOS
                                                    : MR_PLATFORM_IOS;
#endif

  mr_sysctl_str("kern.osproductversion", caps->os_version,
                sizeof(caps->os_version));
  if (caps->os_version[0] == '\0') {
    mr_copy_field(caps->os_version, sizeof(caps->os_version), "unknown");
  }

  struct utsname uts;
  if (uname(&uts) == 0) {
    mr_copy_field(caps->arch_name, sizeof(caps->arch_name), uts.machine);
  } else {
    mr_copy_field(caps->arch_name, sizeof(caps->arch_name), "arm64");
  }

  caps->physical_memory = mr_sysctl_u64("hw.memsize");

  /*
   * The budget is what this process may hold, not what the machine has. On
   * iPadOS the jetsam limit is well under physical memory and it is the binding
   * constraint on the translation cache: a pool sized from hw.memsize is a pool
   * the process cannot keep, and the launch dies later for a reason that has
   * nothing to do with the number that was wrong.
   *
   * On macOS there is no equivalent limit, so physical memory is the answer.
   */
#if TARGET_OS_IPHONE
  uint64_t available = (uint64_t)os_proc_available_memory();
  caps->memory_budget = available != 0 ? available : caps->physical_memory;
#else
  caps->memory_budget = caps->physical_memory;
#endif

  caps->jit_hole_bytes = mr_measure_jit_hole();

  /*
   * Ask the machine whether it will run code this process just wrote, rather
   * than reading a flag. See mr_host_jit_selftest: on iPadOS the flag can be set
   * with no debugger attached, and the mapping then returns zero and fails
   * somewhere else entirely.
   */
  caps->jit_capable = mr_host_jit_selftest();
  caps->jit_enabled = caps->jit_capable;

#if TARGET_OS_OSX
  caps->can_spawn_processes = true;
  caps->sandboxed = false;
#else
  /* fork/exec is unavailable to a third-party app on iPadOS, with or without a
   * jailbreak, and the runtime is built to not need it: wineserver runs as a
   * thread of this process. */
  caps->can_spawn_processes = false;
  caps->sandboxed = true;
#endif

  /* dlopen reaches libraries inside the app bundle and the system ones. It does
   * not reach arbitrary paths on iPadOS, which is why Wine and FEX ship inside
   * the bundle rather than being installed next to it. */
  caps->can_dlopen = true;
  caps->has_network = true;

  const char *home = getenv("HOME");
  struct statfs fs;
  if (statfs(home != NULL ? home : "/", &fs) == 0) {
    caps->free_disk_bytes = (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
  }

  mr_metal_probe_result gpu;
  mr_metal_probe(&gpu);

  caps->gpu_available = gpu.device_present;
  caps->gpu_family = gpu.gpu_family;
  caps->unified_memory = gpu.device_present ? gpu.unified_memory : true;
  caps->supports_bc_textures = gpu.supports_bc_textures;
  caps->supports_mesh_shaders = gpu.supports_mesh_shaders;
  caps->supports_hardware_raytracing = gpu.supports_hardware_raytracing;
  caps->supports_argument_buffers_tier2 = gpu.argument_buffers_tier2;
  caps->metal3_available = gpu.metal3_available;
  caps->metal4_available = gpu.metal4_available;

  /*
   * Real hardware or a paravirtual device. A real Apple GPU answers yes to one
   * of the MTLGPUFamilyApple families; the device the CI runner exposes answers
   * no to all of them while still being a Metal device.
   */
  caps->real_apple_gpu = gpu.device_present && gpu.gpu_family != 0;

  mr_metalfx_caps fx;
  mr_metalfx_probe(&fx);
  caps->metalfx_spatial = fx.spatial;
  caps->metalfx_temporal = fx.temporal;
  caps->metalfx_denoise = fx.denoise;

  if (gpu.max_buffer_length != 0) {
    caps->max_buffer_length = gpu.max_buffer_length;
  }
  if (gpu.max_threadgroup_memory != 0) {
    caps->max_threadgroup_memory = gpu.max_threadgroup_memory;
  }
  if (gpu.max_threads_per_threadgroup != 0) {
    caps->max_threads_per_threadgroup = gpu.max_threads_per_threadgroup;
  }

  /* The device name is what the user sees in the import screen, so an absent GPU
   * is named as such rather than reported as an empty string. */
  if (caps->gpu_available) {
    mr_copy_field(caps->device_name, sizeof(caps->device_name), gpu.device_name);
  } else if (caps->device_name[0] == '\0') {
    mr_copy_field(caps->device_name, sizeof(caps->device_name),
                  "no Metal device");
  }
}
