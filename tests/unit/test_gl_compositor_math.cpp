// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for GlCompositor's pure quad/texcoord math (no GL context).

#include "scene/gl_compositor_geometry.hpp"

#include <gtest/gtest.h>

namespace {

using drm::scene::gl_geom::quad;
using drm::scene::gl_geom::QuadVertex;

// Vertex order is TL, BL, TR, BR (GL_TRIANGLE_STRIP).
constexpr int k_tl = 0;
constexpr int k_bl = 1;
constexpr int k_tr = 2;
constexpr int k_br = 3;

TEST(GlCompositorMath, FullCanvasMapsToFullNdcYFlipped) {
  // dst = whole 200x100 canvas, src = whole 200x100 texture.
  const auto q = quad(0, 0, 200, 100, 200, 100, 0, 0, 200, 100, 200, 100);
  // X spans the full [-1, +1].
  EXPECT_FLOAT_EQ(q[k_tl].x, -1.0F);
  EXPECT_FLOAT_EQ(q[k_tr].x, 1.0F);
  // Y is flipped: canvas top (y=0) -> NDC +1, canvas bottom -> NDC -1.
  EXPECT_FLOAT_EQ(q[k_tl].y, 1.0F);
  EXPECT_FLOAT_EQ(q[k_bl].y, -1.0F);
  // Texcoords cover the whole texture; v not flipped (top row = v 0).
  EXPECT_FLOAT_EQ(q[k_tl].u, 0.0F);
  EXPECT_FLOAT_EQ(q[k_tl].v, 0.0F);
  EXPECT_FLOAT_EQ(q[k_br].u, 1.0F);
  EXPECT_FLOAT_EQ(q[k_br].v, 1.0F);
}

TEST(GlCompositorMath, SubRectCentersCorrectly) {
  // dst at (50,25) size 100x50 on a 200x100 canvas → centered, half size.
  const auto q = quad(50, 25, 100, 50, 200, 100, 0, 0, 64, 64, 64, 64);
  // x: 50/200 -> -0.5 ; 150/200 -> +0.5
  EXPECT_FLOAT_EQ(q[k_tl].x, -0.5F);
  EXPECT_FLOAT_EQ(q[k_tr].x, 0.5F);
  // y (flipped): 25/100 top -> 1 - 0.5 = 0.5 ; 75/100 bottom -> 1 - 1.5 = -0.5
  EXPECT_FLOAT_EQ(q[k_tl].y, 0.5F);
  EXPECT_FLOAT_EQ(q[k_bl].y, -0.5F);
}

TEST(GlCompositorMath, SourceRectSubsamplesTexture) {
  // Sample the bottom-right 32x32 quadrant of a 64x64 texture.
  const auto q = quad(0, 0, 64, 64, 64, 64, 32, 32, 32, 32, 64, 64);
  EXPECT_FLOAT_EQ(q[k_tl].u, 0.5F);  // 32/64
  EXPECT_FLOAT_EQ(q[k_tl].v, 0.5F);
  EXPECT_FLOAT_EQ(q[k_br].u, 1.0F);  // (32+32)/64
  EXPECT_FLOAT_EQ(q[k_br].v, 1.0F);
}

TEST(GlCompositorMath, ZeroDimensionsDoNotDivideByZero) {
  // Degenerate canvas/texture must not produce NaN/inf (guarded to 1.0).
  const auto q = quad(0, 0, 10, 10, 0, 0, 0, 0, 10, 10, 0, 0);
  for (const QuadVertex& vtx : q) {
    EXPECT_FALSE(vtx.x != vtx.x);  // NaN check
    EXPECT_FALSE(vtx.y != vtx.y);
    EXPECT_FALSE(vtx.u != vtx.u);
    EXPECT_FALSE(vtx.v != vtx.v);
  }
}

}  // namespace
