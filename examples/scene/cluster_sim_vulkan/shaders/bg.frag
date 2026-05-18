#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Cluster_sim-parity radial bg gradient:
//   center color = 0xFF101626  (R=0x10, G=0x16, B=0x26)
//   edge   color = 0xFF02040A
//   radius (in pixels) = max(width, height) * 0.6
//
// Output framebuffer is VK_FORMAT_B8G8R8A8_UNORM, where component 0 is B,
// component 1 is G, component 2 is R. The DRM consumer reads
// DRM_FORMAT_ARGB8888 (little-endian BGRA bytes), so writing
// out_color = vec4(B, G, R, A) lands the bytes in the right slots.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
  // extent of the framebuffer in pixels — fragment math works in
  // pixels (rather than uv) so the gradient radius matches the CPU
  // path exactly across aspect ratios.
  vec2 extent;
} pc;

void main() {
  vec2 px = v_uv * pc.extent;
  vec2 c = pc.extent * 0.5;
  float radius = max(pc.extent.x, pc.extent.y) * 0.6;
  float t = clamp(length(px - c) / radius, 0.0, 1.0);

  // sRGB-ish hex constants from cluster_sim, normalized to [0, 1].
  vec3 center_rgb = vec3(0x10, 0x16, 0x26) / 255.0;
  vec3 edge_rgb   = vec3(0x02, 0x04, 0x0A) / 255.0;
  vec3 color = mix(center_rgb, edge_rgb, t);

  // BGRA byte order for VK_FORMAT_B8G8R8A8_UNORM → DRM ARGB8888.
  out_color = vec4(color.b, color.g, color.r, 1.0);
}
