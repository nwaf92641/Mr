/*
 * The test's own shaders: one RGB-gradient triangle, the same geometry Madeira's
 * D3D11 triangle uses, so the two can be compared once the guest path exists.
 *
 * The vertex stage pulls from a buffer by [[vertex_id]] rather than taking a
 * [[stage_in]] vertex descriptor. That is not a shortcut: D3D11 binds vertex
 * buffers at draw time and DXMT's airconv synthesises the fetch from the input
 * layout, so the pull model is what the real path does, and a test that used a
 * vertex descriptor would be exercising something the runtime will not use.
 *
 * packed_float3 is 12 bytes, so the two attributes sit at offsets 0 and 12 and a
 * 24-byte vertex matches D3D11's R32G32B32_FLOAT input element exactly.
 */
#include <metal_stdlib>
using namespace metal;

struct Vertex {
  packed_float3 position;
  packed_float3 color;
};

struct VSOut {
  float4 position [[position]];
  float3 color;
};

vertex VSOut vs_main(uint vid [[vertex_id]],
                     const device Vertex *verts [[buffer(0)]]) {
  VSOut out;
  out.position = float4(float3(verts[vid].position), 1.0);
  out.color = float3(verts[vid].color);
  return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
  return float4(in.color, 1.0);
}
