// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::set_log_sink — the consumer-installable
// diagnostic redirection hook. Coverage:
//   - sink receives (level, formatted_message);
//   - LogLevel filter gates entry to the sink;
//   - format args are interpolated before reaching the sink;
//   - set_log_sink(nullptr) restores the default (no-throw).
//
// Each test runs in a fixture that snapshots and restores the global
// log level + sink. Without that, a level-filter test that lowers the
// threshold would bleed into later tests, and the default-sink path
// would write [drm:...] noise to stderr in CI logs.

#include "log.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CapturedEvent {
  drm::LogLevel level;
  std::string message;
};

class LogSinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_level_ = drm::get_log_level();
    saved_sink_ = drm::get_log_sink();
    drm::set_log_level(drm::LogLevel::Debug);  // accept everything by default
    drm::set_log_sink([this](drm::LogLevel level, std::string_view message) {
      events_.push_back({level, std::string(message)});
    });
  }

  void TearDown() override {
    drm::set_log_sink(saved_sink_);
    drm::set_log_level(saved_level_);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  std::vector<CapturedEvent> events_;

 private:
  drm::LogLevel saved_level_{drm::LogLevel::Info};
  drm::LogSink saved_sink_;
};

}  // namespace

TEST_F(LogSinkTest, SinkReceivesLevelAndMessage) {
  drm::log_warn("hello {}", "world");
  drm::log_error("answer is {}", 42);

  ASSERT_EQ(events_.size(), 2U);
  EXPECT_EQ(events_[0].level, drm::LogLevel::Warn);
  EXPECT_EQ(events_[0].message, "hello world");
  EXPECT_EQ(events_[1].level, drm::LogLevel::Error);
  EXPECT_EQ(events_[1].message, "answer is 42");
}

TEST_F(LogSinkTest, AllFourLevelsRouteThroughSink) {
  drm::log_error("e");
  drm::log_warn("w");
  drm::log_info("i");
  drm::log_debug("d");

  ASSERT_EQ(events_.size(), 4U);
  EXPECT_EQ(events_[0].level, drm::LogLevel::Error);
  EXPECT_EQ(events_[1].level, drm::LogLevel::Warn);
  EXPECT_EQ(events_[2].level, drm::LogLevel::Info);
  EXPECT_EQ(events_[3].level, drm::LogLevel::Debug);
}

TEST_F(LogSinkTest, LevelFilterGatesSinkEntry) {
  drm::set_log_level(drm::LogLevel::Warn);

  drm::log_error("err");    // <= Warn, admitted
  drm::log_warn("warn");    // <= Warn, admitted
  drm::log_info("info");    // > Warn, dropped
  drm::log_debug("debug");  // > Warn, dropped

  ASSERT_EQ(events_.size(), 2U);
  EXPECT_EQ(events_[0].message, "err");
  EXPECT_EQ(events_[1].message, "warn");
}

TEST_F(LogSinkTest, SilentLevelDropsEverything) {
  drm::set_log_level(drm::LogLevel::Silent);

  drm::log_error("e");
  drm::log_warn("w");
  drm::log_info("i");
  drm::log_debug("d");

  EXPECT_TRUE(events_.empty());
}

TEST_F(LogSinkTest, NullSinkRestoresDefaultWithoutThrow) {
  // Install the empty function — the implementation is documented to
  // restore the default sink in that case. We can't observe stderr
  // from inside gtest cleanly, but we can verify it doesn't crash and
  // that get_log_sink() reports a callable sink afterwards.
  drm::set_log_sink(drm::LogSink{});
  EXPECT_TRUE(static_cast<bool>(drm::get_log_sink()));

  // Installing nullptr explicitly takes the same path.
  drm::set_log_sink(nullptr);
  EXPECT_TRUE(static_cast<bool>(drm::get_log_sink()));
}
