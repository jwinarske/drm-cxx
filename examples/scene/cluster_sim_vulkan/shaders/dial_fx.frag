#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// "Dial polish" fragment: a thin metallic highlight band on the
// rim/face boundary plus a top-half gloss reflection on the face.
// Drawn over the textured dial template (rim + face + ticks) and
// under the SDF needle, on the same dial quad geometry as the
// textured pipeline — so we can reuse textured.vert and only have
// push constants for dst + uv.
//
// All effects are computed in the dial's normalized [-1, 1] frame.
// Radii match the textured template's: r_inner = 0.84 (face outer /
// rim inner), r_outer = 0.96 (rim outer). Output is premultiplied;
// the existing premultiplied-alpha blend pipeline handles the
// composite over the dial template beneath.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 p = (v_uv - vec2(0.5)) * 2.0;
  float r = length(p);

  const float r_outer = 0.96;
  const float r_inner = 0.84;

  // Anti-aliased masks bounding the rim (r < r_outer) and the face
  // (r < r_inner). fwidth keeps the edges tight at any quad size.
  float w = max(fwidth(r), 1e-6);
  float dial_mask = 1.0 - smoothstep(r_outer - w, r_outer + w, r);
  float face_mask = 1.0 - smoothstep(r_inner - w, r_inner + w, r);

  // Metallic highlight band: a Gaussian peaking at the rim/face
  // boundary, fading into both sides. Width tuned so the band reads
  // as a "polished bevel" rather than a stroke.
  float rim_d = (r - r_inner) / 0.035;
  float rim_alpha = exp(-rim_d * rim_d) * 0.30;
  rim_alpha *= dial_mask;

  // Top-half gloss reflection on the face. p.y < 0 is the upper
  // half of the dial in Vulkan y-down NDC; we map y in [-r_inner, 0]
  // to alpha in [max, 0]. smoothstep gives a soft falloff toward
  // the dial center.
  float gloss_t = clamp(-p.y / r_inner, 0.0, 1.0);
  float gloss_alpha = smoothstep(0.0, 1.0, gloss_t) * 0.12;
  gloss_alpha *= face_mask;

  // Cool blue-white highlight color for both effects — reads as a
  // glass + brushed-metal pairing without committing to a specific
  // material temperature.
  vec3 rim_rgb = vec3(0.85, 0.88, 0.95);
  vec3 gloss_rgb = vec3(0.80, 0.85, 0.95);

  float alpha = clamp(rim_alpha + gloss_alpha, 0.0, 1.0);
  vec3 rgb = (rim_rgb * rim_alpha) + (gloss_rgb * gloss_alpha);
  out_color = vec4(rgb, alpha);
}
