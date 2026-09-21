/*
 * The Metal backend's real test.
 *
 * It answers the questions in order and refuses to answer any of them with "no
 * error was reported":
 *
 *   1  was a Metal device created
 *   2  was a command queue created
 *   3  did a command buffer execute
 *   4  was a render target created
 *   5  did a draw happen            <- proved by reading the pixels back
 *   6  did a frame reach the drawable
 *   ...
 *
 * Number 5 is the reason this file is worth writing. A command buffer that
 * completes proves the API calls were legal; it does not prove a triangle
 * reached the render target. The test clears to a known colour, draws three
 * vertices in three primary colours, reads the texture back, and checks that a
 * pixel outside the triangle is the clear colour and a pixel inside it is the
 * interpolated gradient. Neither assertion can pass unless rasterisation
 * happened with both shaders working.
 *
 * The FEX, Wine and DXMT questions are printed as UNVERIFIED, with the reason,
 * rather than omitted. They are the next stages and this file is where their
 * evidence will go.
 *
 * Usage: mr_metal_test <path-to-triangle.metallib>
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metal/mr_metal_backend.h"
#include "mr/mr_backend.h"
#include "mr/mr_host.h"

/* ------------------------------------------------------------------ harness */

static int g_failures;
static int g_passes;
static int g_unverified;

typedef enum { R_PASS, R_FAIL, R_SKIP } mr_result;

static void mr_report(int n, const char *question, mr_result r,
                      const char *detail) {
  const char *word = r == R_PASS ? "PASS" : (r == R_FAIL ? "FAIL" : "UNVERIFIED");
  printf("[%2d] %-46s %-10s %s\n", n, question, word,
         detail != NULL ? detail : "");
  fflush(stdout);
  if (r == R_PASS) g_passes++;
  if (r == R_FAIL) g_failures++;
  if (r == R_SKIP) g_unverified++;
}

/* ------------------------------------------------------------------ geometry */

#define MR_TEST_W 64
#define MR_TEST_H 64

typedef struct {
  float x, y, z;
  float r, g, b;
} mr_test_vertex;

/* The same three vertices in the same three colours as Madeira's D3D11
 * triangle, in NDC with y up, which is what both D3D11 and Metal use. */
static const mr_test_vertex k_vertices[3] = {
    {0.0f, 0.6f, 0.0f, 1.0f, 0.0f, 0.0f},
    {0.6f, -0.6f, 0.0f, 0.0f, 1.0f, 0.0f},
    {-0.6f, -0.6f, 0.0f, 0.0f, 0.0f, 1.0f},
};

static const float k_clear[4] = {0.1f, 0.15f, 0.2f, 1.0f};

static const unsigned char *mr_pixel(const unsigned char *pixels, int x, int y) {
  return pixels + ((size_t)y * MR_TEST_W + (size_t)x) * 4u;
}

static int mr_channel_close(unsigned char got, int want, int tolerance) {
  int diff = (int)got - want;
  if (diff < 0) diff = -diff;
  return diff <= tolerance;
}

/* ------------------------------------------------------------------- main */

static mr_status mr_run_frame(mr_gfx_backend *gfx, mr_gfx_handle library,
                              mr_gfx_handle *out_target,
                              char *why, size_t why_len);

#if TARGET_OS_OSX
/*
 * Attempts the drawable half of the frame path: a real CAMetalLayer in a real
 * NSWindow, presented through the backend's present().
 *
 * This is split out because it is the one part of the test that depends on the
 * machine having a window server. A headless CI runner can fail to give the
 * layer a drawable, and that is a fact about the environment rather than about
 * the runtime, so the caller reports it as UNVERIFIED with the reason instead of
 * failing the run.
 */
static mr_status mr_try_present(mr_gfx_backend *gfx, mr_gfx_handle target,
                                const char **why) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    NSRect rect = NSMakeRect(0, 0, MR_TEST_W, MR_TEST_H);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:rect
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (window == nil) {
      *why = "an NSWindow could not be created";
      return MR_ERR_STATE;
    }
    NSView *view = [[NSView alloc] initWithFrame:rect];
    CAMetalLayer *layer = [CAMetalLayer layer];
    /* No device set here: mr_metal_set_drawable_layer fills it in from the
     * device the backend is actually using. Asking for the system default
     * device again would be a second call whose result is not known to be the
     * same object the backend holds. */
    layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
    layer.drawableSize = CGSizeMake(MR_TEST_W, MR_TEST_H);
    layer.framebufferOnly = YES;
    view.layer = layer;
    view.wantsLayer = YES;
    window.contentView = view;
    [window orderFront:nil];

    mr_status set = mr_metal_set_drawable_layer(gfx, layer);
    if (set != MR_OK) {
      *why = "the backend refused the layer";
      return set;
    }

    mr_status st = gfx->present(gfx, target);
    if (st != MR_OK) {
      *why = mr_metal_last_reason(gfx);
    }
    [window close];
    return st;
  }
}
#endif

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("Mr Metal backend test\n\n");

  if (argc < 2) {
    fprintf(stderr, "usage: %s <triangle.metallib>\n", argv[0]);
    return 2;
  }

  /* Stage 1: the host probe reports the device. This exercises
   * mr_host_apple.c and mr_metal_probe.m, which is a real dependency of
   * everything below: a backend is only chosen because the probe said so.
   *
   * A machine with no Metal device stops the run here as UNVERIFIED rather than
   * FAIL. That distinction is the whole point: a CI runner or a VM without a GPU
   * cannot make a claim about a GPU backend either way, and reporting it as a
   * failure would send someone looking for a bug in code that never ran. */
  mr_host_caps host;
  mr_host_probe(&host);

  char detail[512];
  snprintf(detail, sizeof(detail), "%s, %s %s, os %s, %s, gpu family %d%s%s",
           host.arch_name, host.platform == MR_PLATFORM_MACOS ? "macOS" : "iOS",
           host.device_name, host.os_version,
           host.unified_memory ? "unified memory" : "discrete memory",
           host.gpu_family, host.metal4_available ? ", Metal 4 available" : "",
           host.jit_enabled ? ", JIT enabled" : ", JIT not enabled");
  mr_report(1, "a Metal device is visible to this process",
            host.gpu_available ? R_PASS : R_SKIP, detail);

  if (!host.gpu_available) {
    printf("\nThis machine exposes no Metal device, so nothing below can be\n"
           "verified here. That is an environment fact, not a result: the\n"
           "backend needs a Mac, an iPad, or a VM with GPU passthrough.\n\n");
    static const char *const k_no_device[] = {
        "backend created",
        "backend describes its own capabilities",
        "metallib loaded into a Metal library",
        "command buffer executed to completion",
        "render target created",
        "draw rasterised the triangle into the target",
        "frame reached the drawable",
        "unimplemented calls refuse and explain",
    };
    for (int i = 0; i < (int)(sizeof(k_no_device) / sizeof(k_no_device[0])); i++) {
      mr_report(2 + i, k_no_device[i], R_SKIP, "no Metal device on this machine");
    }
    mr_report(10, "FEX executed translated ARM64 code", R_SKIP,
              "FEX is not integrated yet; needs the arm64 FEXCore build and a JIT "
              "entitlement");
    mr_report(11, "Wine loaded a Windows executable", R_SKIP,
              "Wine is not integrated yet; needs the ARM64EC build and a prefix");
    mr_report(12, "DXMT created a D3D11 device and context", R_SKIP,
              "DXMT is not integrated yet; needs its iOS build and a guest PE to "
              "drive it");
    printf("\n%d passed, %d failed, %d unverified\n", g_passes, g_failures,
           g_unverified);
    return 0;
  }

  const char *reason = NULL;
  mr_gfx_backend *gfx = mr_gfx_backend_create(&host, &reason);

  snprintf(detail, sizeof(detail), "%s", reason != NULL ? reason : "");
  mr_report(2, "backend created", gfx != NULL ? R_PASS : R_FAIL, detail);
  if (gfx == NULL) {
    printf("\nNo backend: %d passed, %d failed, %d unverified\n", g_passes,
           g_failures, g_unverified);
    return 1;
  }

  /* Stage 2: the capabilities the backend reports about itself. */
  mr_gfx_caps caps;
  if (gfx->caps(gfx, &caps) != MR_OK) {
    printf("\nfatal: caps() failed\n");
    return 1;
  }
  snprintf(detail, sizeof(detail),
           "%s on %s, %u frames in flight, %s, limit %llu bytes", caps.name,
           caps.device_name, caps.max_frames_in_flight,
           caps.explicit_barriers ? "explicit barriers"
                                  : "implicit hazard tracking",
           (unsigned long long)caps.max_buffer_length);
  mr_report(3, "backend describes its own capabilities",
            caps.backend == MR_BACKEND_METAL3 ? R_PASS : R_FAIL, detail);

  /* Stage 3: a shader library, compiled to a metallib before this ran. Loading
   * a real metallib rather than compiling from source is the path DXMT uses:
   * its DXBC -> AIR translator produces one. */
  mr_status st = MR_ERR_STATE;
  mr_gfx_handle library = MR_GFX_HANDLE_NONE;
  long metallib_len = 0;
  {
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
      printf("\nfatal: cannot open %s\n", argv[1]);
      return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
      printf("\nfatal: %s is empty\n", argv[1]);
      fclose(f);
      return 2;
    }
    void *bytes = malloc((size_t)len);
    if (bytes == NULL || fread(bytes, 1, (size_t)len, f) != (size_t)len) {
      printf("\nfatal: cannot read %s\n", argv[1]);
      fclose(f);
      free(bytes);
      return 2;
    }
    fclose(f);
    metallib_len = len;
    st = gfx->library_load(gfx, bytes, (size_t)len, &library);
    free(bytes);
  }
  if (st == MR_OK) {
    snprintf(detail, sizeof(detail),
             "library_load accepted %ld bytes of compiled metallib",
             metallib_len);
  } else {
    snprintf(detail, sizeof(detail), "%ld bytes of metallib: %s", metallib_len,
             mr_metal_last_reason(gfx));
  }
  mr_report(4, "metallib loaded into a Metal library", st == MR_OK ? R_PASS : R_FAIL,
            detail);
  if (st != MR_OK) goto summary;

  /* Stages 4 to 7: the frame itself. */
  {
    mr_gfx_handle target = MR_GFX_HANDLE_NONE;
    char frame_why[256];
    frame_why[0] = '\0';
    mr_status frame = mr_run_frame(gfx, library, &target, frame_why,
                                   sizeof(frame_why));

    if (frame == MR_OK) {
      mr_report(5, "command buffer executed to completion", R_PASS,
                mr_metal_last_commit_status(gfx));
      mr_report(6, "render target created", R_PASS, "64x64 RGBA8, shared");
      mr_report(7, "draw rasterised the triangle into the target", R_PASS,
                frame_why);
    } else {
      mr_report(5, "command buffer executed to completion", R_FAIL,
                mr_metal_last_commit_status(gfx));
      mr_report(6, "render target created", R_FAIL, "the frame did not get far "
                                                   "enough to say");
      mr_report(7, "draw rasterised the triangle into the target", R_FAIL,
                frame_why);
    }

    /* Stage 8: presenting into a drawable, where the machine allows it. */
#if TARGET_OS_OSX
    if (target != MR_GFX_HANDLE_NONE) {
      const char *present_why = NULL;
      mr_status pst = mr_try_present(gfx, target, &present_why);
      if (pst == MR_OK) {
        mr_report(8, "frame reached the drawable", R_PASS,
                  "presented through a CAMetalLayer in an NSWindow");
      } else {
        mr_report(8, "frame reached the drawable", R_SKIP,
                  present_why != NULL ? present_why
                                      : "no drawable in this environment");
      }
    } else {
      mr_report(8, "frame reached the drawable", R_SKIP,
                "no render target to present");
    }
#else
    mr_report(8, "frame reached the drawable", R_SKIP,
              "needs an app host: on iPadOS a drawable comes from a UIWindow, "
              "so this belongs in the app-level test");
#endif
  }

  /* Stage 9: the backend must refuse what it has not implemented, and say so.
   * Without this check, "returns MR_ERR_UNSUPPORTED" is a claim about the source
   * rather than about the binary. */
  {
    mr_gfx_handle fence = MR_GFX_HANDLE_NONE;
    mr_status s1 = gfx->fence_create(gfx, &fence);
    const char *r1 = mr_metal_last_reason(gfx);

    mr_gfx_handle archive = MR_GFX_HANDLE_NONE;
    mr_status s2 = gfx->archive_open(gfx, "/tmp/mr-test-archive", true, &archive);
    const char *r2 = mr_metal_last_reason(gfx);

    bool refused = s1 == MR_ERR_UNSUPPORTED && s2 == MR_ERR_UNSUPPORTED &&
                   r1 != NULL && r1[0] != '\0' && r2 != NULL && r2[0] != '\0';
    snprintf(detail, sizeof(detail), "fences (%d: %s), archives (%d: %s)", s1,
             r1, s2, r2);
    mr_report(9, "unimplemented calls refuse and explain", refused ? R_PASS : R_FAIL,
              detail);
  }

summary:
  printf("\n");
  mr_report(10, "FEX executed translated ARM64 code", R_SKIP,
            "FEX is not integrated yet; needs the arm64 FEXCore build and a JIT "
            "entitlement");
  mr_report(11, "Wine loaded a Windows executable", R_SKIP,
            "Wine is not integrated yet; needs the ARM64EC build and a prefix");
  mr_report(12, "DXMT created a D3D11 device and context", R_SKIP,
            "DXMT is not integrated yet; needs its iOS build and a guest PE to "
            "drive it");

  if (gfx != NULL) {
    mr_metal_wait_idle(gfx);
    gfx->destroy(gfx);
  }

  printf("\n%d passed, %d failed, %d unverified\n", g_passes, g_failures,
         g_unverified);
  return g_failures == 0 ? 0 : 1;
}

/* --------------------------------------------------------------- the frame */

/*
 * Builds and runs one frame end to end, then reads the target back and checks
 * it. Returns MR_OK only when the pixels are what the geometry says they should
 * be; `why` carries the pixel evidence either way, so a passing run still prints
 * what it actually saw.
 */
static mr_status mr_run_frame(mr_gfx_backend *gfx, mr_gfx_handle library,
                              mr_gfx_handle *out_target,
                              char *why, size_t why_len) {
  mr_status st;
  mr_gfx_handle allocator = MR_GFX_HANDLE_NONE;
  mr_gfx_handle cmd = MR_GFX_HANDLE_NONE;
  mr_gfx_handle target = MR_GFX_HANDLE_NONE;
  mr_gfx_handle vbo = MR_GFX_HANDLE_NONE;
  mr_gfx_handle table = MR_GFX_HANDLE_NONE;
  mr_gfx_handle pso = MR_GFX_HANDLE_NONE;
  mr_gfx_handle encoder = MR_GFX_HANDLE_NONE;

  st = gfx->allocator_create(gfx, "test", &allocator);
  if (st != MR_OK) {
    snprintf(why, why_len, "allocator_create: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->command_buffer_create(gfx, "frame", &cmd);
  if (st != MR_OK) {
    snprintf(why, why_len, "command_buffer_create: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->begin_command_buffer(gfx, cmd, allocator);
  if (st != MR_OK) {
    snprintf(why, why_len, "begin_command_buffer: %s", mr_metal_last_reason(gfx));
    return st;
  }

  /* The render target. Shared storage so the test can read it back, which is
   * also what a swap chain's back buffer is on a unified-memory device. */
  mr_texture_desc td;
  memset(&td, 0, sizeof(td));
  td.format = MR_FMT_R8G8B8A8_UNORM;
  td.width = MR_TEST_W;
  td.height = MR_TEST_H;
  td.mip_levels = 1;
  td.array_length = 1;
  td.sample_count = 1;
  td.usage = MR_USAGE_RENDER_TARGET;
  td.shared = true;
  td.label = "test target";
  st = gfx->texture_create(gfx, &td, &target);
  if (st != MR_OK) {
    snprintf(why, why_len, "texture_create: %s", mr_metal_last_reason(gfx));
    return st;
  }

  /* The vertex data. Identical bytes to a D3D11 vertex buffer for the same
   * geometry, so the guest path can be compared against this run. */
  mr_buffer_desc bd;
  memset(&bd, 0, sizeof(bd));
  bd.length = sizeof(k_vertices);
  bd.usage = MR_USAGE_VERTEX;
  bd.shared = true;
  bd.label = "test vertices";
  st = gfx->buffer_create(gfx, &bd, &vbo);
  if (st != MR_OK) {
    snprintf(why, why_len, "buffer_create: %s", mr_metal_last_reason(gfx));
    return st;
  }
  void *contents = gfx->buffer_contents(gfx, vbo);
  if (contents == NULL) {
    snprintf(why, why_len, "buffer_contents returned NULL for a shared buffer");
    return MR_ERR_STATE;
  }
  memcpy(contents, k_vertices, sizeof(k_vertices));

  mr_argument_table_desc atd;
  memset(&atd, 0, sizeof(atd));
  atd.max_buffers = 1;
  atd.max_textures = 0;
  atd.max_samplers = 0;
  atd.label = "test bindings";
  st = gfx->argument_table_create(gfx, &atd, &table);
  if (st != MR_OK) {
    snprintf(why, why_len, "argument_table_create: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->argument_table_set_buffer(gfx, table, 0, vbo, 0);
  if (st != MR_OK) {
    snprintf(why, why_len, "argument_table_set_buffer: %s",
             mr_metal_last_reason(gfx));
    return st;
  }

  mr_render_pipeline_desc pd;
  memset(&pd, 0, sizeof(pd));
  pd.vertex.library = library;
  snprintf(pd.vertex.function, sizeof(pd.vertex.function), "vs_main");
  pd.fragment.library = library;
  snprintf(pd.fragment.function, sizeof(pd.fragment.function), "fs_main");
  pd.color_formats[0] = MR_FMT_R8G8B8A8_UNORM;
  pd.color_count = 1;
  pd.depth_format = MR_FMT_UNKNOWN;
  pd.sample_count = 1;
  pd.raster_sample_count = 1;
  st = gfx->compile_render_pipeline(gfx, &pd, &pso);
  if (st != MR_OK) {
    snprintf(why, why_len, "compile_render_pipeline: %s",
             mr_metal_last_reason(gfx));
    return st;
  }

  mr_render_pass_desc rp;
  memset(&rp, 0, sizeof(rp));
  rp.color[0] = target;
  rp.color_formats[0] = MR_FMT_R8G8B8A8_UNORM;
  rp.do_clear_color[0] = true;
  memcpy(rp.clear_color, k_clear, sizeof(k_clear));
  rp.store_color[0] = true;
  rp.depth = MR_GFX_HANDLE_NONE;
  rp.label = "test pass";
  st = gfx->render_encoder_begin(gfx, cmd, &rp, &encoder);
  if (st != MR_OK) {
    snprintf(why, why_len, "render_encoder_begin: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->bind_argument_table(gfx, encoder, MR_STAGE_VERTEX, table);
  if (st != MR_OK) {
    snprintf(why, why_len, "bind_argument_table: %s", mr_metal_last_reason(gfx));
    return st;
  }

  mr_draw_args args;
  memset(&args, 0, sizeof(args));
  args.pipeline = pso;
  args.vertex_count = 3;
  args.instance_count = 1;
  st = gfx->draw(gfx, encoder, &args);
  if (st != MR_OK) {
    snprintf(why, why_len, "draw: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->render_encoder_end(gfx, encoder);
  if (st != MR_OK) {
    snprintf(why, why_len, "render_encoder_end: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->end_command_buffer(gfx, cmd);
  if (st != MR_OK) {
    snprintf(why, why_len, "end_command_buffer: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = gfx->commit(gfx, &cmd, 1);
  if (st != MR_OK) {
    snprintf(why, why_len, "commit: %s", mr_metal_last_reason(gfx));
    return st;
  }
  st = mr_metal_wait_idle(gfx);
  if (st != MR_OK) {
    snprintf(why, why_len, "the GPU reported an error: %s",
             mr_metal_last_commit_status(gfx));
    return st;
  }

  /* The evidence. */
  unsigned char pixels[MR_TEST_W * MR_TEST_H * 4];
  st = mr_metal_read_texture(gfx, target, pixels, sizeof(pixels),
                             MR_TEST_W * 4u);
  if (st != MR_OK) {
    snprintf(why, why_len, "read-back failed: %s", mr_metal_last_reason(gfx));
    return st;
  }

  const unsigned char *outside = mr_pixel(pixels, 2, 2);
  const unsigned char *inside = mr_pixel(pixels, 32, 22);
  const unsigned char *centroid = mr_pixel(pixels, 32, 38);

  /* Clear colour 0.1/0.15/0.2 in 8 bits. */
  int clear_ok = mr_channel_close(outside[0], 26, 4) &&
                 mr_channel_close(outside[1], 38, 4) &&
                 mr_channel_close(outside[2], 51, 4);

  /*
   * Inside the triangle near the red vertex the barycentric weight of red is
   * about 0.75, so the pixel is red-dominant. Near the centroid all three
   * weights are about a third, so the pixel is grey.
   *
   * These are properties rather than exact numbers on purpose. The exact values
   * follow from rasterisation rules about pixel centres that are not worth
   * pinning a test to; "red dominates here and nothing dominates there" is only
   * true if the triangle was actually rasterised and the interpolator actually
   * ran, which is the claim being tested.
   */
  int red_dominant = inside[0] > (int)inside[1] + 60 &&
                     inside[0] > (int)inside[2] + 60 && inside[0] > 120;
  int grey_centroid =
      mr_channel_close(centroid[0], (int)centroid[1], 16) &&
      mr_channel_close(centroid[1], (int)centroid[2], 16) &&
      centroid[0] > 60 && centroid[0] < 110;

  snprintf(why, why_len,
           "outside (%u,%u,%u) want ~(26,38,51); inside (%u,%u,%u) want red-"
           "dominant; centroid (%u,%u,%u) want grey",
           outside[0], outside[1], outside[2], inside[0], inside[1], inside[2],
           centroid[0], centroid[1], centroid[2]);

  if (!clear_ok || !red_dominant || !grey_centroid) {
    return MR_ERR_STATE;
  }

  *out_target = target;
  return MR_OK;
}
