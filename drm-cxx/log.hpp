// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <format>
#include <print>
#include <utility>

namespace drm {

enum class LogLevel : uint8_t {
  Silent = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
};

// Runtime log level. Defaults to Info, overridden by DRM_CXX_LOG_LEVEL define.
namespace detail {

inline LogLevel& current_log_level() {
  static LogLevel level =
#if defined(DRM_CXX_LOG_LEVEL)
      static_cast<LogLevel>(DRM_CXX_LOG_LEVEL);
#else
      LogLevel::Info;
#endif
  return level;
}

}  // namespace detail

inline void set_log_level(LogLevel level) {
  detail::current_log_level() = level;
}

[[nodiscard]] inline LogLevel get_log_level() {
  return detail::current_log_level();
}

template <typename... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Error) {
    std::println(stderr, "[drm:error] {}", std::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Warn) {
    std::println(stderr, "[drm:warn] {}", std::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Info) {
    std::println("[drm:info] {}", std::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Debug) {
    std::println("[drm:debug] {}", std::format(fmt, std::forward<Args>(args)...));
  }
}

}  // namespace drm
