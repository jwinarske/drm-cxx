// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_acquire_fence.cpp
//
// Host unit tests for the acquire-fence close discipline: drm::sync::SyncFence
// + the AcquiredBuffer.acquire_fence field. The real risk in the fence layer is
// fd lifecycle — a per-frame sync_file that leaks or double-closes. These tests
// exercise that deterministically (no GPU), using a pipe fd as a stand-in
// "signaled fence" (SyncFence::wait polls POLLIN; a written-to pipe is ready).

#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <optional>
#include <unistd.h>
#include <utility>

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

// Count entries under /proc/self/fd. Includes ".", ".." and the opendir fd, but
// those are identical across calls, so deltas are exact.
int count_fds() {
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return -1;
  }
  int n = 0;
  while (::readdir(dir) != nullptr) {
    ++n;
  }
  ::closedir(dir);
  return n;
}

// A pipe whose read end is POLLIN-ready (one byte written) — a stand-in for a
// signaled sync_file. Returns the read fd (caller closes) or -1.
int signaled_fd() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return -1;
  }
  const char byte = 'x';
  (void)::write(fds[1], &byte, 1);
  ::close(fds[1]);
  return fds[0];
}

}  // namespace

int main() {
  // import_fd dups the source fd; fd() peeks without transferring ownership;
  // wait() returns success on an already-ready fence.
  {
    const int raw = signaled_fd();
    CHECK(raw >= 0);
    auto fence = drm::sync::SyncFence::import_fd(raw);
    CHECK(fence.has_value());
    if (fence) {
      CHECK(fence->fd() >= 0);
      CHECK(fence->fd() != raw);  // dup'd, distinct fd
      CHECK(fence->wait(std::chrono::milliseconds(200)).has_value());
    }
    ::close(raw);  // import_fd dup'd; we still own the original
  }

  // Close discipline: churning AcquiredBuffers that carry a fence — including a
  // move (what the scene does on every commit) and drop — must not leak fds.
  {
    const int base = count_fds();
    CHECK(base > 0);
    for (int i = 0; i < 500; ++i) {
      const int raw = signaled_fd();
      auto fence = drm::sync::SyncFence::import_fd(raw);
      ::close(raw);
      CHECK(fence.has_value());
      drm::scene::AcquiredBuffer acq;
      acq.fb_id = 1;
      acq.acquire_fence = std::move(*fence);
      // Move through (mirrors the scene moving the buffer into its release ring),
      // then let it drop — the SyncFence dtor must close the fd exactly once.
      drm::scene::AcquiredBuffer moved = std::move(acq);
      CHECK(moved.acquire_fence.has_value());
    }
    const int after = count_fds();
    // Bounded: a per-iteration leak would add ~500 fds (and likely EMFILE first).
    CHECK(after <= base + 2);
  }

  // A buffer without a fence is the common case and must stay nullopt.
  {
    drm::scene::AcquiredBuffer acq;
    CHECK(!acq.acquire_fence.has_value());
  }

  if (g_fail == 0) {
    std::puts("test_acquire_fence: OK");
  }
  return g_fail == 0 ? 0 : 1;
}
