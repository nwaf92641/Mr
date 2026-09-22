/*
 * Does this machine have a Metal device the D3D11 probe could be verified on?
 *
 * It exists so the workflow does not have to infer that answer from how the
 * probe died. A GitHub runner's GPU is paravirtual and DXMT does not tolerate it,
 * so without this check the probe reaching D3D11CreateDevice would be reported as
 * a failure of the probe, which is the wrong conclusion from the right
 * observation. Asking the device directly keeps the two apart:
 *
 *   exit 0   a Metal device with an Apple GPU family is present; run the probe
 *   exit 77  no usable device, so nothing about the graphics path can be
 *            verified here and the caller reports
 *            REQUIRES REAL APPLE GPU VALIDATION
 *
 * The device questions are asked through mr_metal_probe(), the same function the
 * Metal conformance test uses, rather than asking them again here. One place
 * decides what counts as a usable device.
 *
 * A device that exists but reports no Apple GPU family is treated as unusable
 * because that is what a paravirtual device does: it answers that it is a Metal
 * device and then reports family 0, which is the observation this repository
 * already recorded in the CI notes.
 */

#include <stdio.h>

#include "mr_metal_probe.h"
#include "mr_metalfx_probe.h"

int main(void) {
  mr_metal_probe_result r;
  mr_metal_probe(&r);

  printf("device_present=%d\n", r.device_present ? 1 : 0);
  printf("device_name=%s\n", r.device_present ? r.device_name : "(none)");
  printf("gpu_family=%d\n", r.gpu_family);
  printf("unified_memory=%d\n", r.unified_memory ? 1 : 0);
  printf("metal3_available=%d\n", r.metal3_available ? 1 : 0);
  printf("metal4_available=%d\n", r.metal4_available ? 1 : 0);
  /* Real hardware or a paravirtual device. Same rule as mr_host_apple.c: a real
   * Apple GPU matches a family in MTLGPUFamilyApple*, a paravirtual device
   * matches none. Derived here rather than probed twice. */
  printf("real_apple_gpu=%d\n",
         (r.device_present && r.gpu_family != 0) ? 1 : 0);

  mr_metalfx_caps fx;
  mr_metalfx_probe(&fx);
  printf("metalfx_spatial=%d\n", fx.spatial ? 1 : 0);
  printf("metalfx_temporal=%d\n", fx.temporal ? 1 : 0);
  printf("metalfx_denoise=%d\n", fx.denoise ? 1 : 0);

  if (!r.device_present) {
    printf("\nno Metal device at all\n\nREQUIRES REAL APPLE GPU VALIDATION\n");
    return 77;
  }
  if (r.gpu_family == 0) {
    printf(
        "\nMetal device \"%s\" reports no Apple GPU family, which is how a "
        "paravirtual device answers\n\nREQUIRES REAL APPLE GPU VALIDATION\n",
        r.device_name);
    return 77;
  }

  printf("\nusable Apple GPU: %s, family %d\n", r.device_name, r.gpu_family);
  return 0;
}
