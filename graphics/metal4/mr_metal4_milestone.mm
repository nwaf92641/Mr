/* Mr — Metal 4 milestone harness. See mr_metal4_milestone.h for what it proves.
 *
 * The shader is compiled from source at run time rather than shipped as a
 * metallib. Two reasons: the harness has to be runnable with nothing but the
 * app binary, and a runtime compile failure names the MSL error instead of
 * producing a library that is silently empty.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "mr_metal4.h"
#include "mr_metal4_milestone.h"

#define MR_MTL4_AVAIL API_AVAILABLE(ios(26.0), macos(26.0))

static const char *kShaderSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Vertex { float2 position; float3 color; };\n"
    "struct Varyings { float4 position [[position]]; float3 color; };\n"
    "vertex Varyings vertex_main(uint vid [[vertex_id]],\n"
    "                            const device Vertex *vertices [[buffer(0)]]) {\n"
    "  Varyings out;\n"
    "  out.position = float4(vertices[vid].position, 0.0, 1.0);\n"
    "  out.color = vertices[vid].color;\n"
    "  return out;\n"
    "}\n"
    "fragment float4 fragment_main(Varyings in [[stage_in]]) {\n"
    "  return float4(in.color, 1.0);\n"
    "}\n";

/* A triangle large enough to fill most of a small target, so the count below is
 * a measurement and not a rounding artefact. */
struct mr_vertex {
  float position[2];
  float color[3];
};

static const mr_vertex kTriangle[3] = {
    {{-1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
    {{3.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-1.0f, 3.0f}, {0.0f, 0.0f, 1.0f}},
};

namespace {

struct Report {
  FILE *out;
  int failed = 0;

  void step(const char *name, const char *result, const char *detail = "") {
    fprintf(out, "  %-6s %-28s %s\n", result, name, detail);
  }
  void pass(const char *name, const char *detail = "") { step(name, "PASS", detail); }
  void skip(const char *name, const char *detail = "") { step(name, "SKIP", detail); }
  void fail(const char *name, const char *detail) {
    step(name, "FAIL", detail);
    failed += 1;
  }
};

MR_MTL4_AVAIL int run_impl(FILE *out, void *mtl_drawable) {
  Report report{out};
  const uint32_t width = 64;
  const uint32_t height = 64;

  /* --- device: the gate. nothing below runs without MTLGPUFamilyMetal4. */
  mr_mtl4_device *device = mr_mtl4_device_create();
  if (device == nullptr) {
    report.fail("DEVICE", mr_mtl4_last_error());
    return 1;
  }
  char detail[192];
  snprintf(detail, sizeof(detail), "%s", mr_mtl4_device_name(device));
  report.pass("DEVICE -> MTL4", detail);

  id<MTLDevice> mtl = (__bridge id<MTLDevice>)mr_mtl4_device_metal_device(device);

  /* --- resources. buffers and textures are unchanged in Metal 4; what changed
   * is that they must be resident, and that the table binds them by GPU address
   * and resource ID rather than by object reference. */
  id<MTLBuffer> vertices = [mtl newBufferWithBytes:kTriangle
                                            length:sizeof(kTriangle)
                                           options:MTLResourceStorageModeShared];
  if (vertices == nil) {
    report.fail("RESOURCE buffer", "newBufferWithBytes returned nil");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  MTLTextureDescriptor *texture_desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  /* Shared so the harness can read the result back; a real back buffer is
   * Private and would need a blit to a shared buffer instead. */
  texture_desc.storageMode = MTLResourceStorageModeShared;
  texture_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  id<MTLTexture> target = [mtl newTextureWithDescriptor:texture_desc];
  if (target == nil) {
    report.fail("RESOURCE texture", "newTextureWithDescriptor returned nil");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  report.pass("RESOURCE -> MTL4", "buffer + render target, Shared storage");

  /* --- residency. Metal 4 does not auto-track encoder-bound resources the way
   * Metal 3 did, so anything the draw reads or writes has to be in a set
   * attached to the queue, or the draw is invalid even though every call
   * succeeded. */
  MTLResidencySetDescriptor *res_desc = [[MTLResidencySetDescriptor alloc] init];
  res_desc.initialCapacity = 4;
  NSError *error = nil;
  id<MTLResidencySet> residency = [mtl newResidencySetWithDescriptor:res_desc error:&error];
  if (residency == nil) {
    report.fail("RESIDENCY set", error != nil ? [[error localizedDescription] UTF8String]
                                              : "newResidencySetWithDescriptor returned nil");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  [residency addAllocation:vertices];
  [residency addAllocation:target];
  [residency commit];
  if (!mr_mtl4_device_add_residency_set(device, (__bridge void *)residency)) {
    report.fail("RESIDENCY attach", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  report.pass("RESIDENCY -> QUEUE", "2 allocations committed");

  /* --- pipeline. Built the Metal 3 way on purpose: a Metal 3 pipeline state
   * works on a Metal 4 encoder, so bring-up does not have to wait for the
   * MTL4Compiler path. */
  id<MTLLibrary> library = [mtl newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                              options:nil
                                                error:&error];
  if (library == nil) {
    report.fail("PIPELINE library",
                error != nil ? [[error localizedDescription] UTF8String] : "compile returned nil");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
  pipeline_desc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
  pipeline_desc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
  pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  id<MTLRenderPipelineState> pipeline =
      [mtl newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
  if (pipeline == nil) {
    report.fail("PIPELINE state",
                error != nil ? [[error localizedDescription] UTF8String] : "creation returned nil");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  report.pass("PIPELINE -> MTL4 ENCODER", "MSL compiled at run time");

  /* --- argument table. One table serves both stages, sized for the one buffer
   * the shader declares at [[buffer(0)]]. */
  mr_mtl4_table *table = mr_mtl4_device_new_table(
      device, 1, 1, 1, MR_MTL4_STAGE_VERTEX | MR_MTL4_STAGE_FRAGMENT, false);
  if (table == nullptr) {
    report.fail("TABLE create", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  mr_mtl4_table_set_buffer(table, (uint64_t)[vertices gpuAddress], 0);
  report.pass("TABLE buffer by address", "[[buffer(0)]]");

  /* --- render pass. MTL4RenderPassDescriptor, attachments hold textures and
   * the clear lives on the load action rather than in a clear call. */
  void *render_pass = mr_mtl4_render_pass_create();
  if (render_pass == nullptr) {
    report.fail("RENDER PASS descriptor", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  mr_mtl4_render_pass_set_size(render_pass, width, height);
  mr_mtl4_render_pass_set_color(render_pass, 0, (__bridge void *)target, MR_MTL4_LOAD_CLEAR,
                                MR_MTL4_STORE_STORE, 0.0, 0.0, 0.0, 1.0);

  /* --- command submission: allocator, command buffer, encoder, batched commit,
   * then a wait -- Metal 4 has no addCompletedHandler, so completion comes back
   * through the shared event the queue signals. */
  mr_mtl4_frame *frame = mr_mtl4_device_new_frame(device, "milestone");
  if (frame == nullptr) {
    report.fail("FRAME create", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  if (!mr_mtl4_frame_begin(frame)) {
    report.fail("FRAME begin", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  if (!mr_mtl4_frame_begin_render_pass(frame, render_pass)) {
    report.fail("ENCODER render", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  mr_mtl4_frame_set_render_pipeline(frame, (__bridge void *)pipeline);
  mr_mtl4_frame_bind_table(frame, table);
  mr_mtl4_frame_set_viewport(frame, 0.0, 0.0, (double)width, (double)height, 0.0, 1.0);
  mr_mtl4_frame_draw(frame, MR_MTL4_PRIMITIVE_TRIANGLE, 0, 3, 1, 0);
  mr_mtl4_frame_end_encoder(frame);
  if (!mr_mtl4_frame_commit(frame)) {
    report.fail("SUBMIT commit", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  if (!mr_mtl4_frame_wait(frame, 10000)) {
    report.fail("SUBMIT wait", mr_mtl4_last_error());
    mr_mtl4_device_destroy(device);
    return 1;
  }
  report.pass("COMMAND SUBMISSION", "commit + shared-event wait");

  /* --- presentation. Needs the drawable the app's CAMetalLayer vends, so it is
   * reported separately: a headless run is not evidence about presentation. */
  if (mtl_drawable != nullptr) {
    mr_mtl4_frame *present_frame = mr_mtl4_device_new_frame(device, "present");
    if (present_frame == nullptr) {
      report.fail("PRESENT frame", mr_mtl4_last_error());
      mr_mtl4_device_destroy(device);
      return 1;
    }
    /* A present with no encode is legal -- an empty command buffer commits
     * successfully -- and it is still a real drawable acquire, wait, signal and
     * present, which is what the step is testing. */
    if (!mr_mtl4_frame_begin(present_frame) || !mr_mtl4_frame_present(present_frame, mtl_drawable)) {
      report.fail("PRESENT drawable", mr_mtl4_last_error());
      mr_mtl4_device_destroy(device);
      return 1;
    }
    if (!mr_mtl4_frame_wait(present_frame, 10000)) {
      report.fail("PRESENT wait", mr_mtl4_last_error());
      mr_mtl4_device_destroy(device);
      return 1;
    }
    report.pass("PRESENT -> SCREEN", "waitForDrawable / signalDrawable / present");
    mr_mtl4_frame_destroy(present_frame);
  } else {
    report.skip("PRESENT -> SCREEN", "needs the app's CAMetalLayer");
  }

  /* --- readback: did the GPU actually draw. A clear-only pass would report
   * zero non-black pixels, so this distinguishes "encoded" from "drawn". */
  const NSUInteger bytes_per_row = width * 4;
  uint8_t *pixels = (uint8_t *)calloc((size_t)(bytes_per_row * height), 1);
  if (pixels == nullptr) {
    report.fail("READBACK", "allocation failed");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  [target getBytes:pixels
       bytesPerRow:bytes_per_row
        fromRegion:MTLRegionMake2D(0, 0, width, height)
       mipmapLevel:0];
  uint32_t non_black = 0;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t *pixel = pixels + y * bytes_per_row + x * 4;
      if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
        non_black += 1;
      }
    }
  }
  free(pixels);

  snprintf(detail, sizeof(detail), "%u / %u pixels non-black", non_black, width * height);
  if (non_black == 0) {
    report.fail("FIRST FRAME", "render target is entirely black after a draw");
    mr_mtl4_device_destroy(device);
    return 1;
  }
  report.pass("FIRST FRAME", detail);

  mr_mtl4_frame_destroy(frame);
  mr_mtl4_device_destroy(device);
  return report.failed == 0 ? 0 : 1;
}

} /* namespace */

int mr_mtl4_milestone_run(FILE *out, void *mtl_drawable) {
  if (out == nullptr) {
    out = stderr;
  }
  if (!mr_mtl4_available()) {
    fprintf(out, "  SKIP   Metal 4 unavailable on this device or OS\n");
    return 2;
  }
  if (@available(iOS 26.0, macOS 26.0, *)) {
    fprintf(out, "mr-mtl4 milestone\n");
    return run_impl(out, mtl_drawable);
  }
  fprintf(out, "  SKIP   Metal 4 requires iOS 26 or macOS 26\n");
  return 2;
}
