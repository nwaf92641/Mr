/*
 * MetalFX availability, in its own translation unit.
 *
 * MetalFX is an optional enhancement and never a condition for running a frame.
 * A DX11 game does not know it exists, so nothing in the frame path may depend on
 * it, and a device that supports none of it must still render. What this file
 * answers is only "which of these effects could be offered here", so that an
 * enhancement elsewhere can be skipped where the hardware has none.
 *
 * It is separate from mr_metal_probe because it is a different question asked of
 * a different framework, and because a MetalFX that is missing or weak-linked
 * must not be able to affect the answer about the device itself.
 *
 * C signature on purpose: the caller is mr_host_apple.c, which is plain C and
 * must never see an Objective-C type.
 */
#ifndef MR_METALFX_PROBE_H
#define MR_METALFX_PROBE_H

#include <stdbool.h>

typedef struct {
  bool spatial;  /* MTLFXSpatialScaler, from iOS/iPadOS 16, macOS 13 */
  bool temporal; /* MTLFXTemporalScaler, same floor */
  bool denoise;  /* MTLFXTemporalDenoisedScaler */
} mr_metalfx_caps;

/*
 * Fills `out` from the system default MTLDevice. Never fails: no device, no
 * framework and no support all leave the fields false, because all three mean
 * the same thing to the caller -- do not offer the effect.
 */
void mr_metalfx_probe(mr_metalfx_caps *out);

#endif /* MR_METALFX_PROBE_H */
