#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Fragment stage that samples one layer of a 2D texture array. Used
// for the info-text readout (current speed, 0..240) and the warning
// cells. Same VK_FORMAT_B8G8R8A8_UNORM pre-multiplied input the
// textured pipeline uses; the textured-quad pipeline's premultiplied-
// alpha blend takes care of compositing over the bg.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2DArray u_tex;

layout(push_constant) uniform PC {
  vec4 dst;
  vec4 uv;
  ivec4 layer_info;
} pc;

void main() {
  out_color = texture(u_tex, vec3(v_uv, float(pc.layer_info.x)));
}
