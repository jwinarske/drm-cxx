#version 450
// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Textured-quad fragment stage. Samples one bound combined image
// sampler and writes the result. Texture is VK_FORMAT_B8G8R8A8_UNORM
// and the color attachment is too, so the four-component vector that
// `texture()` returns maps componentwise to `out_color` and the byte
// order is preserved end-to-end (no channel swap in this shader).
// Alpha is forwarded so the bg shows through transparent regions of
// the template.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

void main() {
  out_color = texture(u_tex, v_uv);
}
