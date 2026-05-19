#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Vertex stage for the sampler2DArray pipeline shared by the info-text
// readout (241 templates, 0..240 km/h) and the warning-cell strip
// (8 templates, 4 cells × lit/dim). Same 6-vertex quad as textured.vert
// but the push-constant block carries an additional ivec4 whose .x is
// the array layer index the fragment stage samples.

layout(location = 0) out vec2 v_uv;

layout(push_constant) uniform PC {
  vec4 dst;
  vec4 uv;
  ivec4 layer_info;  // .x = layer index, .yzw unused
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
