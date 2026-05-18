#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// SDF-rendered speedo needle + hub.
//
// We work in a local frame [-1, 1] centered on the dial. p.y > 0 is
// downward, matching the y-down screen coordinate convention Blend2D
// uses elsewhere in cluster_sim_vulkan (and which the Vulkan NDC y
// also matches). The needle is a line segment from (0, 0) to
// (cos(angle), sin(angle)) * r_needle with rounded caps; the hub is
// a disc at the origin with radius r_hub, drawn on top of the needle
// at the pivot. Per-fragment AA via fwidth + smoothstep, premultiplied
// output for clean blending over the textured dial face beneath.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
  vec4 dst;
  vec4 uv;
  float angle;
  float r_needle;
  float r_hub;
  float half_thickness;
} pc;

void main() {
  // [0, 1] uv → [-1, 1] centered on the dial.
  vec2 p = (v_uv - vec2(0.5)) * 2.0;

  // Line segment a → b. Endpoint b is the needle's tip.
  vec2 a = vec2(0.0);
  vec2 b = vec2(cos(pc.angle), sin(pc.angle)) * pc.r_needle;

  // Distance to the line segment (rounded-cap convention).
  vec2 pa = p - a;
  vec2 ba = b - a;
  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-9), 0.0, 1.0);
  float d_needle = length(pa - ba * h) - pc.half_thickness;

  // Distance to the hub disc.
  float d_hub = length(p) - pc.r_hub;

  // Pick the feature whose surface is closer. If both are interior
  // (negative distance), the more-interior one (lower) wins, which
  // gives the hub priority over the needle where they overlap (the
  // pivot point). Matches cluster_sim's "stroke needle, then fill
  // hub on top" paint order.
  bool needle_wins = d_needle < d_hub;
  vec3 needle_rgb = vec3(0xFF, 0x3B, 0x30) / 255.0;  // cluster_sim 0xFFFF3B30
  vec3 hub_rgb    = vec3(0x1A, 0x1F, 0x2C) / 255.0;  // cluster_sim 0xFF1A1F2C
  vec3 color = needle_wins ? needle_rgb : hub_rgb;
  float d = min(d_needle, d_hub);

  // AA over roughly one fragment.
  float w = max(fwidth(d), 1e-6);
  float alpha = 1.0 - smoothstep(-w, w, d);

  // Premultiplied RGBA so the pipeline's
  //   src = ONE, dst = ONE_MINUS_SRC_ALPHA
  // blend matches: result = color*alpha + bg*(1-alpha).
  out_color = vec4(color * alpha, alpha);
}
