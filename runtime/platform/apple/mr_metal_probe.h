/*
 * Metal capability probe: the only Objective-C in the project.
 *
 * It is Objective-C because MTLDevice is an Objective-C protocol, and the
 * alternative -- calling objc_msgSend by hand -- trades every compiler type
 * check for a hand-written cast. Keeping it as one small translation unit also
 * keeps the Objective-C surface out of the portable core, which has to compile
 * and be tested on a machine with no Apple SDK at all.
 *
 * The file answers questions and encodes nothing: no command buffers, no
 * resources, no pipelines. Drawing lives in the backend targets, which only
 * exist when an Apple SDK is present.
 *
 * Only documented MTLDevice members are read, and a capability that Metal offers
 * no documented query for is reported false rather than derived from a chip
 * name. A guess would be worse than a false: the planner treats these as facts,
 * and a wrong fact selects a code path the GPU does not have.
 */
#ifndef MR_METAL_PROBE_H
#define MR_METAL_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool device_present;
  char device_name[128];
  /* Raw MTLGPUFamily value of the highest Apple family the device reports, so
   * the number means what Apple's tables mean. 0 when nothing matched. */
  int gpu_family;
  bool unified_memory;
  bool supports_bc_textures;
  bool supports_mesh_shaders;
  bool supports_hardware_raytracing;
  bool argument_buffers_tier2;
  uint64_t max_buffer_length;
  uint64_t max_threadgroup_memory;
  size_t max_threads_per_threadgroup;
  bool metal3_available;
  bool metal4_available;
} mr_metal_probe_result;

/*
 * Fills `out` from the system default MTLDevice. Never fails: a device that
 * cannot be created leaves device_present false and everything else false, which
 * is the truth on a machine without a GPU rather than a reason to abort.
 */
void mr_metal_probe(mr_metal_probe_result *out);

#endif /* MR_METAL_PROBE_H */
