#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Needle vertex stage. Same 6-vertex quad as textured.vert, but the
// push-constant block is larger because the fragment stage carries
// the needle's angle / lengths / thickness alongside dst + uv.

layout(location = 0) out vec2 v_uv;

layout(push_constant) uniform PC {
  vec4 dst;             // NDC: x0, y0, x1, y1
  vec4 uv;              // u0, v0, u1, v1
  float angle;          // radians (Blend2D math convention: y-down sin)
  float r_needle;       // tip distance, normalized (1.0 = half-dial)
  float r_hub;          // hub radius, normalized
  float half_thickness; // line half-width, normalized
} pc;

void main() {
  vec2 positions[6] = vec2[](
    vec2(pc.dst.x, pc.dst.y),
    vec2(pc.dst.x, pc.dst.w),
    vec2(pc.dst.z, pc.dst.w),
    vec2(pc.dst.x, pc.dst.y),
    vec2(pc.dst.z, pc.dst.w),
    vec2(pc.dst.z, pc.dst.y)
  );
  vec2 uvs[6] = vec2[](
    vec2(pc.uv.x, pc.uv.y),
    vec2(pc.uv.x, pc.uv.w),
    vec2(pc.uv.z, pc.uv.w),
    vec2(pc.uv.x, pc.uv.y),
    vec2(pc.uv.z, pc.uv.w),
    vec2(pc.uv.z, pc.uv.y)
  );
  v_uv = uvs[gl_VertexIndex];
  gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
