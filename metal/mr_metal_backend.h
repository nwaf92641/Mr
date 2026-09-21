/*
 * The Metal backend's own interface, as opposed to the vtable in mr_backend.h.
 *
 * There is exactly one reason this file exists separately: the ABI has no way to
 * hand a backend a drawable. mr_gfx_backend_create() takes host capabilities and
 * nothing else, and mr_gfx_backend_present() takes a texture, so a backend that
 * has to present into a CAMetalLayer has nowhere to be told which layer.
 *
 * Madeira answers this question in its own way -- DXMT's winemetal looks up
 * `macdrv_functions` with dlsym(RTLD_DEFAULT) and asks Wine's display driver for
 * a CAMetalLayer belonging to an HWND -- but that route only exists when Wine is
 * in the picture. The Metal backend has to be testable with no Wine at all, which
 * is the point of building it first.
 *
 * So the layer is set through one additive call, declared here rather than in
 * mr_backend.h, because adding an entry to the vtable would change every backend
 * and every caller to serve one. The type is void * so that this header stays
 * valid C: CAMetalLayer is an Objective-C object and cannot be named in a header
 * that the portable core includes.
 */
#ifndef MR_METAL_BACKEND_H
#define MR_METAL_BACKEND_H

#include "mr/mr_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the layer that mr_gfx_backend_present() draws into. `layer` is a
 * CAMetalLayer, borrowed: the backend retains it, so the caller may release its
 * own reference. Passing NULL clears it, after which present() reports that no
 * drawable is available instead of failing somewhere inside Metal.
 *
 * Returns MR_ERR_UNSUPPORTED when `backend` is not a Metal backend.
 */
mr_status mr_metal_set_drawable_layer(mr_gfx_backend *backend, void *layer);

/*
 * Why the last call that returned MR_ERR_UNSUPPORTED did so, for diagnostics and
 * for tests. Never NULL. The string is owned by the backend and stays valid
 * until the next call on it.
 */
const char *mr_metal_last_reason(const mr_gfx_backend *backend);

/*
 * A one-line human description of the most recent command buffer completion,
 * for tests: whether it completed, whether it errored, and the error text when
 * Metal reported one. Empty before the first commit.
 */
const char *mr_metal_last_commit_status(const mr_gfx_backend *backend);

/*
 * Blocks until every command buffer this backend has committed has completed,
 * and returns MR_ERR_STATE if any of them finished with an error.
 *
 * This exists because the ABI has no way to await a committed command buffer.
 * commit() is asynchronous, present() returns before the frame lands, and the
 * fence entries that could express the wait return MR_ERR_UNSUPPORTED. Something
 * has to be able to say "the GPU is done", or a caller cannot read a render
 * target back and a test cannot assert what was drawn. Fences are the right
 * answer for the renderer; this is the blunt one, and it belongs in the tests
 * and in teardown.
 */
mr_status mr_metal_wait_idle(mr_gfx_backend *backend);

/*
 * Copies mip 0 of a 2D texture into `out`, `out_len` bytes, one row of
 * `bytes_per_row`. Returns MR_ERR_RANGE when the buffer is too small for the
 * texture's height, and MR_ERR_UNSUPPORTED for a private texture, which has no
 * host address to read from.
 *
 * The ABI has copy_into_texture and nothing in the other direction, so without
 * this a test can show that a draw returned no error but not that it drew
 * anything. "The command buffer completed" and "the triangle is in the pixels"
 * are different claims, and only the second one is worth a green tick.
 */
mr_status mr_metal_read_texture(mr_gfx_backend *backend, mr_gfx_handle texture,
                                void *out, size_t out_len,
                                uint32_t bytes_per_row);

/*
 * The entry point mr_gfx_backend_create() falls through to on Apple platforms,
 * defined in mr_backend_metal3.m. Declared here rather than in mr_backend.h
 * because only one caller should ever choose a backend, and a second caller
 * choosing independently is how the two get out of step. It is declared at all
 * because a C function without a prototype is an error under this project's
 * warning set.
 */
mr_gfx_backend *mr_gfx_backend_create_apple(const mr_host_caps *host,
                                            const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* MR_METAL_BACKEND_H */
