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
  vec4 needle_color;  // R, G, B in [0, 1]; A unused (SDF drives alpha)
  float angle;
  float r_needle;
  float r_hub;
  float half_thickness;
  float redline_intensity;  // 0..1, amplifies glow + shifts color toward hot red
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
  float dist_to_line = length(pa - ba * h);
  float d_needle = dist_to_line - pc.half_thickness;

  // Distance to the hub disc.
  float d_hub = length(p) - pc.r_hub;

  // Solid needle/hub alpha (AA over roughly one fragment). The
  // hub takes priority over the needle where they overlap (the
  // pivot point) — matches cluster_sim's "stroke needle, then fill
  // hub on top" paint order.
  bool needle_wins = d_needle < d_hub;
  vec3 hub_rgb = vec3(0x1A, 0x1F, 0x2C) / 255.0;  // cluster_sim 0xFF1A1F2C
  vec3 solid_rgb = needle_wins ? pc.needle_color.rgb : hub_rgb;
  float d_solid = min(d_needle, d_hub);
  float w = max(fwidth(d_solid), 1e-6);
  float solid_alpha = 1.0 - smoothstep(-w, w, d_solid);

  // Soft glow halo around the needle line. Falls off from the line
  // center over ~5x the half-thickness; capped at glow_strength to
  // stay subtle. Suppressed wherever the solid already covers
  // (so the glow only contributes outside the solid silhouette and
  // around the hub). At redline, the glow radius widens and the
  // strength roughly doubles — pushed by the runtime when the
  // tach passes ~80% of its sweep.
  float glow_strength = 0.45 + (0.45 * pc.redline_intensity);
  float glow_radius_mul = 5.0 + (4.0 * pc.redline_intensity);
  float glow_falloff = dist_to_line / (pc.half_thickness * glow_radius_mul);
  float glow_alpha = exp(-glow_falloff * glow_falloff) * glow_strength;
  glow_alpha = clamp(glow_alpha * (1.0 - solid_alpha), 0.0, 1.0);

  // Compose solid + glow. The solid keeps its pushed needle color;
  // the glow shifts warmer at redline (toward a hot orange-red) so
  // an amber tach glows red as it approaches the limit.
  vec3 hot_rgb = vec3(1.0, 0.20, 0.10);
  vec3 glow_rgb = mix(pc.needle_color.rgb, hot_rgb, pc.redline_intensity);
  float final_alpha = clamp(solid_alpha + glow_alpha, 0.0, 1.0);
  vec3 final_rgb = (solid_rgb * solid_alpha) + (glow_rgb * glow_alpha);

  // Premultiplied RGBA so the pipeline's
  //   src = ONE, dst = ONE_MINUS_SRC_ALPHA
  // blend matches: result = color + bg*(1-final_alpha).
  out_color = vec4(final_rgb, final_alpha);
}
