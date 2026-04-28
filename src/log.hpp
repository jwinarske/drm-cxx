// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// log.hpp — drm-cxx's library-wide diagnostic surface.
//
// Two knobs are exposed:
//
//   1. `set_log_level(LogLevel)` — process-wide threshold. Messages at
//      or below this level are emitted; the rest are silently dropped
//      *before* formatting cost is paid.
//
//   2. `set_log_sink(LogSink)` — process-wide redirection hook. The
//      default sink writes the formatted message to stderr (errors,
//      warnings) or stdout (info, debug) prefixed with "[drm:LEVEL] ".
//      Consumers can install a sink that forwards to spdlog, log4cxx,
//      journald, syslog, an in-memory test buffer — anywhere a
//      `void(LogLevel, std::string_view)` callback can land. Pass
//      nullptr to restore the default. The sink receives the
//      pre-formatted message; it does not need to depend on fmt.
//
// `log_error/warn/info/debug` are templated on the format args so the
// formatting cost is paid only when the active level admits the
// message — calls below the threshold reduce to a single integer
// comparison and a branch.

#pragma once

#include "drm-cxx/detail/format.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace drm {

enum class LogLevel : uint8_t {
  Silent = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
};

/// Sink signature: receives the active level and the fully-formatted
/// message body (no level prefix, no trailing newline). Sinks are
/// invoked synchronously from the calling thread; consumers that need
/// thread-safety or async dispatch arrange it inside their sink.
using LogSink = std::function<void(LogLevel, std::string_view)>;

namespace detail {

inline LogLevel& current_log_level() {
  static LogLevel level =
#ifdef DRM_CXX_LOG_LEVEL
      static_cast<LogLevel>(DRM_CXX_LOG_LEVEL);
#else
      LogLevel::Info;
#endif
  return level;
}

inline void default_sink(LogLevel level, std::string_view message) {
  // Mirrors the pre-hook hardcoded behavior: errors and warnings to
  // stderr, info and debug to stdout, "[drm:LEVEL] " prefix.
  const char* tag = nullptr;
  std::FILE* stream = nullptr;
  switch (level) {
    case LogLevel::Error:
      tag = "error";
      stream = stderr;
      break;
    case LogLevel::Warn:
      tag = "warn";
      stream = stderr;
      break;
    case LogLevel::Info:
      tag = "info";
      stream = stdout;
      break;
    case LogLevel::Debug:
      tag = "debug";
      stream = stdout;
      break;
    case LogLevel::Silent:
      return;  // unreachable in practice — log_* gates on level first
  }
  drm::println(stream, "[drm:{}] {}", tag, message);
}

inline LogSink& current_log_sink() {
  static LogSink sink = &default_sink;
  return sink;
}

inline void emit(LogLevel level, std::string_view message) {
  const auto& sink = current_log_sink();
  if (sink) {
    sink(level, message);
  }
}

}  // namespace detail

inline void set_log_level(LogLevel level) {
  detail::current_log_level() = level;
}

[[nodiscard]] inline LogLevel get_log_level() {
  return detail::current_log_level();
}

/// Install a sink. Pass nullptr (default-constructed `LogSink{}`) to
/// restore the default stderr/stdout sink. Thread-unsafe with respect
/// to concurrent `log_*` calls — install once during application
/// startup, before spinning up worker threads that emit logs.
inline void set_log_sink(LogSink sink) {
  detail::current_log_sink() = sink ? std::move(sink) : LogSink{&detail::default_sink};
}

[[nodiscard]] inline const LogSink& get_log_sink() {
  return detail::current_log_sink();
}

template <typename... Args>
void log_error(drm::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Error) {
    detail::emit(LogLevel::Error, drm::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_warn(drm::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Warn) {
    detail::emit(LogLevel::Warn, drm::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_info(drm::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Info) {
    detail::emit(LogLevel::Info, drm::format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void log_debug(drm::format_string<Args...> fmt, Args&&... args) {
  if (detail::current_log_level() >= LogLevel::Debug) {
    detail::emit(LogLevel::Debug, drm::format(fmt, std::forward<Args>(args)...));
  }
}

}  // namespace drm