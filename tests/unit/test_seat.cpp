// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm::session::Seat against a fake libseat. The functions below are defined
// in this executable, so libdrm-cxx's calls bind to them (ELF interposition)
// instead of the real libseat: the test drives enable/disable events and
// decides which device opens succeed.

#include <drm-cxx/session/seat.hpp>

#include <cerrno>
#include <chrono>
#include <deque>
#include <gtest/gtest.h>
extern "C" {
#include <libseat.h>
}
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct FakeSeat {
  const libseat_seat_listener* listener{nullptr};
  void* userdata{nullptr};
  std::deque<bool> events;  // true = enable, false = disable
  int next_id{1};
  int next_fd{100};
  std::map<std::string, int> open_errno;  // path -> errno for the next open
  std::vector<int> closed_ids;
  int opens{0};
  bool enable_on_open{true};  // false: a backend that never enables the seat
  bool seat_closed{false};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
FakeSeat g_fake;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_handle;  // address stands in for the libseat pointer

libseat* fake_handle() {
  return reinterpret_cast<libseat*>(
      &g_handle);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

}  // namespace

// NOLINTBEGIN(readability-identifier-naming,misc-use-internal-linkage)
extern "C" {

libseat* libseat_open_seat(const libseat_seat_listener* listener, void* userdata) {
  g_fake.listener = listener;
  g_fake.userdata = userdata;
  if (g_fake.enable_on_open) {
    g_fake.events.push_back(true);  // the initial enable_seat
  }
  return fake_handle();
}

int libseat_close_seat(libseat* /*seat*/) {
  g_fake.seat_closed = true;
  return 0;
}

int libseat_disable_seat(libseat* /*seat*/) {
  return 0;
}

int libseat_dispatch(libseat* seat, int /*timeout*/) {
  if (g_fake.events.empty()) {
    return 0;
  }
  const bool enable = g_fake.events.front();
  g_fake.events.pop_front();
  if (enable) {
    g_fake.listener->enable_seat(seat, g_fake.userdata);
  } else {
    g_fake.listener->disable_seat(seat, g_fake.userdata);
  }
  return 1;
}

int libseat_get_fd(libseat* /*seat*/) {
  return -1;
}

int libseat_open_device(libseat* /*seat*/, const char* path, int* fd) {
  ++g_fake.opens;
  if (const auto it = g_fake.open_errno.find(path); it != g_fake.open_errno.end()) {
    errno = it->second;
    g_fake.open_errno.erase(it);
    return -1;
  }
  *fd = g_fake.next_fd++;
  return g_fake.next_id++;
}

int libseat_close_device(libseat* /*seat*/, int device_id) {
  g_fake.closed_ids.push_back(device_id);
  return 0;
}

int libseat_switch_session(libseat* /*seat*/, int /*session*/) {
  return 0;
}

void libseat_set_log_handler(libseat_log_func /*handler*/) {}

void libseat_set_log_level(enum libseat_log_level /*level*/) {}

}  // extern "C"
// NOLINTEND(readability-identifier-naming,misc-use-internal-linkage)

namespace {

class SeatTest : public ::testing::Test {
 protected:
  void SetUp() override { g_fake = FakeSeat{}; }
};

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access) -- every access follows an ASSERT_TRUE

// Taking a held path returns the same device; no second open, nothing orphaned.
TEST_F(SeatTest, TakeHeldPathReturnsExistingHandle) {
  auto seat = drm::session::Seat::open();
  ASSERT_TRUE(seat.has_value());

  const auto a = seat->take_device("/dev/dri/card0");
  ASSERT_TRUE(a.has_value());
  const auto b = seat->take_device("/dev/dri/card0");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->fd, a->fd);
  EXPECT_EQ(b->device_id, a->device_id);
  EXPECT_EQ(g_fake.opens, 1);

  seat->release_device("/dev/dri/card0");
  EXPECT_EQ(g_fake.closed_ids, (std::vector<int>{a->device_id}));
}

// A resume reopen that fails reaches the failure callback, not resume_cb.
TEST_F(SeatTest, FailedResumeReopenIsReported) {
  auto seat = drm::session::Seat::open();
  ASSERT_TRUE(seat.has_value());
  ASSERT_TRUE(seat->take_device("/dev/dri/card0").has_value());
  ASSERT_TRUE(seat->take_device("/dev/input/event3").has_value());

  std::vector<std::string> resumed;
  std::vector<std::pair<std::string, std::error_code>> failed;
  seat->set_resume_callback([&](std::string_view path, int /*fd*/) { resumed.emplace_back(path); });
  seat->set_resume_failed_callback(
      [&](std::string_view path, std::error_code ec) { failed.emplace_back(path, ec); });

  g_fake.open_errno["/dev/dri/card0"] = ENODEV;
  g_fake.events = {false, true};  // pause, then resume
  seat->dispatch();

  ASSERT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed[0].first, "/dev/dri/card0");
  EXPECT_EQ(failed[0].second, std::error_code(ENODEV, std::system_category()));
  EXPECT_EQ(resumed, (std::vector<std::string>{"/dev/input/event3"}));
}

// After a failed reopen the path can be taken again: a fresh open, not the
// dead entry.
TEST_F(SeatTest, DeadEntryIsReopenedByTake) {
  auto seat = drm::session::Seat::open();
  ASSERT_TRUE(seat.has_value());
  ASSERT_TRUE(seat->take_device("/dev/dri/card0").has_value());

  g_fake.open_errno["/dev/dri/card0"] = ENODEV;
  g_fake.events = {false, true};
  seat->dispatch();
  const int opens_before = g_fake.opens;

  const auto again = seat->take_device("/dev/dri/card0");
  ASSERT_TRUE(again.has_value());
  EXPECT_GE(again->fd, 0);
  EXPECT_EQ(g_fake.opens, opens_before + 1);
}

// A backend that connects but never enables the seat (seatd over SSH, no VT)
// gives nullopt within the timeout, and the seat is closed.
TEST_F(SeatTest, OpenTimesOutWhenSeatNeverEnabled) {
  g_fake.enable_on_open = false;
  const auto start = std::chrono::steady_clock::now();
  auto seat = drm::session::Seat::open(std::chrono::milliseconds{50});
  const auto took = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(seat.has_value());
  EXPECT_TRUE(g_fake.seat_closed);
  EXPECT_LT(took, std::chrono::seconds{2});
}

// NOLINTEND(bugprone-unchecked-optional-access)
