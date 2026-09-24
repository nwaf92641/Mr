/* Mr — Metal 4 milestone harness.
 *
 * Walks the bring-up ladder and reports each step, so "it compiled" is never
 * the answer to "does Metal 4 work". Every step before the render actually
 * executes Metal: a device is created, a buffer and a texture are allocated, an
 * argument table is bound, a command buffer is committed and waited on, a
 * pipeline is built from Metal source, a triangle is encoded and drawn, and the
 * render target is read back and the non-black pixels counted. A clear alone
 * would pass with an empty draw, so the check is that the drawn triangle put
 * pixels in the target.
 *
 * Runs headless. The only step it cannot do without the app is presentation,
 * which needs the CAMetalLayer the Swift side owns; pass a drawable to
 * mr_mtl4_milestone_run to include it, or pass NULL and it reports SKIP for
 * that step alone.
 *
 * Result codes: 0 all executed steps passed, 1 a step failed, 2 Metal 4 is not
 * available on this device (not a failure -- the layer is meant to decline).
 */
#ifndef MR_METAL4_MILESTONE_H
#define MR_METAL4_MILESTONE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mtl_drawable is an id<CAMetalDrawable> or NULL. out may be NULL for stderr. */
int mr_mtl4_milestone_run(FILE *out, void *mtl_drawable);

#ifdef __cplusplus
}
#endif

#endif /* MR_METAL4_MILESTONE_H */
