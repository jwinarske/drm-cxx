#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Cluster_sim-parity radial bg gradient:
//   center color = 0xFF101626  (R=0x10, G=0x16, B=0x26)
//   edge   color = 0xFF02040A
//   radius (in pixels) = max(width, height) * 0.6
//
// Fragment-shader outputs are always RGBA-semantic regardless of the
// color attachment's format. The Vulkan implementation handles the
// memory-layout mapping for VK_FORMAT_B8G8R8A8_UNORM — write the
// colors in straight (R, G, B, A) order and the framebuffer encoder
// lands them in the right bytes (which the DRM consumer then reads
// as ARGB8888).

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

  out_color = vec4(color, 1.0);
}
