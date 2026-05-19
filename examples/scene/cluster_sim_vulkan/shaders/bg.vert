#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Full-screen triangle. No vertex buffer: gl_VertexIndex picks one of
// three positions that cover [-1, 3]^2, so the rasterizer clips to the
// [-1, 1]^2 viewport. UV is passed through in [0, 1] for the fragment
// stage to use as a normalized coordinate.

layout(location = 0) out vec2 v_uv;

void main() {
  vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
  );
  vec2 p = positions[gl_VertexIndex];
  v_uv = (p + 1.0) * 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
