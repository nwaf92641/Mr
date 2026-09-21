/*
 * The portable half of the graphics backend.
 *
 * Format arithmetic and the frame-in-flight policy live here so they can be
 * tested without a GPU. The vtable itself is implemented per platform: the Apple
 * implementation is Objective-C over Metal, and it is the only file in the
 * project that needs the Metal SDK.
 */
#include "mr/mr_backend.h"

#include <string.h>

uint32_t mr_format_bytes_per_pixel(mr_format f) {
  switch (f) {
    case MR_FMT_R8_UNORM: return 1;
    case MR_FMT_R8G8_UNORM: return 2;
    case MR_FMT_R8G8B8A8_UNORM:
    case MR_FMT_R8G8B8A8_UNORM_SRGB:
    case MR_FMT_B8G8R8A8_UNORM:
    case MR_FMT_B8G8R8A8_UNORM_SRGB:
    case MR_FMT_R16_FLOAT:
    case MR_FMT_R16G16_FLOAT:
    case MR_FMT_R32_FLOAT:
    case MR_FMT_R32_UINT:
    case MR_FMT_R32_SINT:
    case MR_FMT_D32_FLOAT:
    case MR_FMT_D24_UNORM_S8_UINT:
      return 4;
    case MR_FMT_R16G16B16A16_FLOAT:
    case MR_FMT_R32G32_FLOAT:
      return 8;
    case MR_FMT_R32G32B32A32_FLOAT:
      return 16;
    case MR_FMT_D16_UNORM:
      return 2;
    case MR_FMT_UNKNOWN:
    case MR_FMT_COUNT:
    case MR_FMT_BC1_RGBA_UNORM: /* block-compressed: see the function below */
    case MR_FMT_BC2_RGBA_UNORM:
    case MR_FMT_BC3_RGBA_UNORM:
    case MR_FMT_BC4_R_UNORM:
    case MR_FMT_BC5_RG_UNORM:
    case MR_FMT_BC6H_RGB_UFLOAT:
    case MR_FMT_BC7_RGBA_UNORM:
      return 0;
  }
  return 0;
}

bool mr_format_is_depth(mr_format f) {
  return f == MR_FMT_D16_UNORM || f == MR_FMT_D32_FLOAT ||
         f == MR_FMT_D24_UNORM_S8_UINT;
}

bool mr_format_is_block_compressed(mr_format f) {
  switch (f) {
    case MR_FMT_BC1_RGBA_UNORM:
    case MR_FMT_BC2_RGBA_UNORM:
    case MR_FMT_BC3_RGBA_UNORM:
    case MR_FMT_BC4_R_UNORM:
    case MR_FMT_BC5_RG_UNORM:
    case MR_FMT_BC6H_RGB_UFLOAT:
    case MR_FMT_BC7_RGBA_UNORM:
      return true;
    default:
      return false;
  }
}

/*
 * The number of frames to keep in flight.
 *
 * Three. Two is enough to overlap the CPU encoding a frame with the GPU drawing
 * the previous one, and the third covers the case where the GPU is a whole frame
 * behind, which is normal on a title that is GPU-bound. Beyond three, extra
 * frames add input latency without adding throughput: there is one GPU, and a
 * deeper queue only means the CPU runs further ahead of what the user sees.
 *
 * The value is not a tunable because a wrong value is a correctness bug rather
 * than a performance one on Metal 4: an MTL4CommandAllocator cannot be reset
 * while the commands encoded from it are still in flight, so the allocator ring
 * and this number have to agree.
 */
uint32_t mr_gfx_recommended_frames_in_flight(const mr_gfx_caps *caps) {
  if (caps == NULL) return 3;
  if (caps->max_frames_in_flight != 0) return caps->max_frames_in_flight;
  return 3;
}

/*
 * The platform implementations. Declared here rather than in a header because
 * only this function should ever choose between them, and a second caller
 * choosing independently is how the two get out of step.
 */
#if defined(__APPLE__) && !defined(MR_NO_APPLE_GFX_BACKEND)
/* metal/mr_backend_metal3.m, plus metal4/mr_backend_metal4.m behind it. */
mr_gfx_backend *mr_gfx_backend_create_apple(const mr_host_caps *host,
                                            const char **reason);
#endif

mr_gfx_backend *mr_gfx_backend_create(const mr_host_caps *host,
                                      const char **reason) {
  if (reason != NULL) *reason = "not attempted";

  if (host == NULL) {
    if (reason != NULL) *reason = "no host capabilities were provided";
    return NULL;
  }

  if (!host->gpu_available) {
    if (reason != NULL) {
      *reason = "no Metal device is visible to this process";
    }
    return NULL;
  }

#if defined(__APPLE__) && !defined(MR_NO_APPLE_GFX_BACKEND)
  return mr_gfx_backend_create_apple(host, reason);
#else
  /*
   * Saying so explicitly is the point. This build can analyse a title and plan a
   * launch, but it cannot draw anything, and a silently working stub here would
   * let a graphics test pass on a machine with no GPU behind it.
   *
   * The two reasons are kept apart because they need different answers: one is
   * "you built on the wrong machine", the other is "nobody has written this
   * yet", and only the first is something the user can fix.
   */
  if (reason != NULL) {
#if defined(__APPLE__)
    *reason =
        "this build has no graphics backend: the Metal backends under metal/ and "
        "metal4/ are not implemented yet";
#else
    *reason = "this build has no Metal backend (built without the Apple SDK)";
#endif
  }
  return NULL;
#endif
}
