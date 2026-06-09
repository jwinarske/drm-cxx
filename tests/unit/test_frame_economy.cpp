// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_frame_economy.cpp — pure decision tests for FrameEconomy.
// Contained CHECK harness; non-zero exit on failure.

#include <drm-cxx/present/frame_economy.hpp>

#include <cstdio>

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

using drm::present::FrameAction;
using drm::present::FrameEconomy;

// First frame always commits full, regardless of inputs (scanout undefined).
void test_first_frame_full() {
  FrameEconomy econ;
  auto d = econ.decide(/*content_changed=*/false, /*damage_available=*/true);
  CHECK(d.action == FrameAction::Commit);
  CHECK(d.full);
  CHECK(econ.committed() == 1);
  CHECK(econ.skipped() == 0);
}

// After the first commit, an unchanged frame is skipped — no commit issued.
void test_idle_skips() {
  FrameEconomy econ;
  (void)econ.decide(true, false);  // first -> full commit
  for (int i = 0; i < 5; ++i) {
    auto d = econ.decide(/*content_changed=*/false, /*damage_available=*/true);
    CHECK(d.action == FrameAction::Skip);
  }
  CHECK(econ.committed() == 1);
  CHECK(econ.skipped() == 5);
}

// Changed content with damage -> damaged commit; without damage -> full commit.
void test_changed_commits() {
  FrameEconomy econ;
  (void)econ.decide(true, false);  // first -> full

  auto damaged = econ.decide(/*content_changed=*/true, /*damage_available=*/true);
  CHECK(damaged.action == FrameAction::Commit);
  CHECK(!damaged.full);  // damaged commit

  auto full = econ.decide(/*content_changed=*/true, /*damage_available=*/false);
  CHECK(full.action == FrameAction::Commit);
  CHECK(full.full);  // no damage available -> full commit

  CHECK(econ.committed() == 3);
  CHECK(econ.skipped() == 0);
}

// force_full() makes the next commit full even when damage is available.
void test_force_full() {
  FrameEconomy econ;
  (void)econ.decide(true, false);  // first -> full
  econ.force_full();
  auto d = econ.decide(/*content_changed=*/true, /*damage_available=*/true);
  CHECK(d.action == FrameAction::Commit);
  CHECK(d.full);  // forced full despite damage being available

  // force_full is one-shot: the following changed+damage frame is damaged again.
  auto d2 = econ.decide(true, true);
  CHECK(d2.action == FrameAction::Commit);
  CHECK(!d2.full);
}

// force_full() also overrides an idle frame into a full commit (e.g. post-resume).
void test_force_full_overrides_idle() {
  FrameEconomy econ;
  (void)econ.decide(true, false);
  econ.force_full();
  auto d = econ.decide(/*content_changed=*/false, /*damage_available=*/true);
  CHECK(d.action == FrameAction::Commit);
  CHECK(d.full);
}

}  // namespace

int main() {
  test_first_frame_full();
  test_idle_skips();
  test_changed_commits();
  test_force_full();
  test_force_full_overrides_idle();

  if (g_fail != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all frame_economy tests passed");
  return 0;
}
