// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "theme.hpp"

#include "detail/expected.hpp"
#include "detail/span.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace drm::cursor {

struct Theme::Impl {
  struct Record {
    std::string name;
    // Every search-path directory that provides this theme, in
    // path-priority order. Multiple dirs for one theme is common when
    // a user ships an override under ~/.icons that points at the same
    // theme name as /usr/share/icons ships.
    std::vector<std::filesystem::path> dirs;
    std::vector<std::string> inherits;
  };

  // Memoization key for resolve(). The cursor name and preferred
  // theme together fully determine the resolve() outcome — the BFS
  // root and alias expansion depend on nothing else (XCURSOR_THEME
  // and "default" fallbacks only kick in when preferred_theme is
  // empty, and those env lookups are captured once via the empty-
  // string key). Keyed as a struct so both strings can live
  // side-by-side without a separator-smuggling hack.
  struct CacheKey {
    std::string cursor_name;
    std::string preferred_theme;
    bool operator==(const CacheKey& other) const noexcept {
      return cursor_name == other.cursor_name && preferred_theme == other.preferred_theme;
    }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
      // Boost-style hash_combine. Both components are caller-bounded
      // strings so collision resistance matters less than speed.
      const auto h1 = std::hash<std::string>{}(k.cursor_name);
      const auto h2 = std::hash<std::string>{}(k.preferred_theme);
      return h1 ^ (h2 + 0x9e3779b9U + (h1 << 6U) + (h1 >> 2U));
    }
  };

  // Kept as a vector (not map) so discovery order — which is the
  // fallback iteration order when no inherits chain matches — is
  // deterministic and reflects search-path priority.
  std::vector<Record> themes;
  std::unordered_map<std::string, std::size_t> by_name;
  std::vector<std::filesystem::path> search_paths;

  // Populated lazily by resolve(); mutable because resolve() is
  // logically const from the caller's perspective (pure function of
  // on-disk state captured at discover time). Theme is not documented
  // as thread-safe, so no synchronization.
  mutable std::unordered_map<CacheKey, drm::expected<ThemeResolution, std::error_code>,
                             CacheKeyHash>
      resolve_cache;
};

Theme::Theme(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Theme::Theme(Theme&&) noexcept = default;
Theme& Theme::operator=(Theme&&) noexcept = default;
Theme::~Theme() = default;

namespace {

// ---------------------------------------------------------------------------
// Alias table — equivalence classes derived from freedesktop's cursor-spec
// Appendix B plus the libxcursor built-in aliases and the hashed names
// (md5-style strings) that some themes still use internally.
//
// Requesting a cursor by any member of a class causes resolve() to try
// every other member as a fallback. Classes overlap deliberately —
// e.g. "col-resize" and "ew-resize" share sb_h_double_arrow, so both
// names reach the same backing file on any theme that ships just one.
// ---------------------------------------------------------------------------
const std::vector<std::vector<std::string_view>>& alias_classes() {
  static const std::vector<std::vector<std::string_view>> k = {
      {"default", "left_ptr", "top_left_arrow", "arrow"},
      {"pointer", "hand", "hand1", "hand2", "pointing_hand", "9d800788f1b08800ae810202380a0822",
       "e29285e634086352946a0e7090d73106"},
      {"text", "xterm", "ibeam"},
      {"crosshair", "cross", "tcross"},
      {"wait", "watch"},
      {"progress", "left_ptr_watch", "half-busy", "3ecb610c1bf2410f44200f48c40d3599",
       "08e8e1c95fe2fc01f976f1e063a24ccd"},
      {"not-allowed", "crossed_circle", "forbidden", "03b6e0fcb3499374a867c041f52298f0"},
      {"grab", "openhand", "9141b49c8149039304290b508d208c40", "f2d0232ec7c4c06e79e2a2a5ebcb6a43"},
      {"grabbing", "closedhand", "208530c400c041818281048008011002",
       "fcf21c00b30f7e3f83fe0dfd12e71cff"},
      {"help", "question_arrow", "whats_this", "left_ptr_help", "d9ce0ab605698f320427677b458ad60b"},
      {"move", "fleur", "all-scroll"},
      {"col-resize", "ew-resize", "sb_h_double_arrow", "h_double_arrow",
       "14fef782d02440884392942c11205230", "028006030e0e7ebffc7f7070c0600140"},
      {"row-resize", "ns-resize", "sb_v_double_arrow", "v_double_arrow",
       "2870a09082c103050810ffdffffe0204", "00008160000006810000408080010102"},
      {"n-resize", "top_side"},
      {"s-resize", "bottom_side"},
      {"e-resize", "right_side"},
      {"w-resize", "left_side"},
      {"ne-resize", "top_right_corner", "ur_angle"},
      {"nw-resize", "top_left_corner", "ul_angle"},
      {"se-resize", "bottom_right_corner", "lr_angle"},
      {"sw-resize", "bottom_left_corner", "ll_angle"},
      {"nesw-resize", "fd_double_arrow"},
      {"nwse-resize", "bd_double_arrow"},
      {"zoom-in", "magnifier"},
      {"zoom-out"},
      {"copy", "dnd-copy", "1081e37283d90000800003c07f3ef6bf"},
      {"alias", "link", "dnd-link", "0876e1c15ff2fc01f5c56f42a6e9db1c"},
      {"no-drop", "circle", "dnd-none"},
      {"cell", "plus"},
      {"vertical-text"},
      {"context-menu"},
  };
  return k;
}

// Invoke `f` on `name` and every one of its alias-class siblings. If
// the requested name isn't in any class, `f` is called once with just
// the name itself. `f` is passed by value so a capture-by-reference
// lambda is copied cheaply; side effects through its captures still
// work because the captures outlive the copy.
template <typename F>
void for_each_alias(std::string_view name, F f) {
  for (const auto& cls : alias_classes()) {
    if (std::find(cls.begin(), cls.end(), name) != cls.end()) {
      for (const auto& alt : cls) {
        f(alt);
      }
      return;
    }
  }
  f(name);
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string_view env_or_empty(const char* env_name) {
  const char* v = std::getenv(env_name);
  return (v != nullptr) ? std::string_view(v) : std::string_view{};
}

// Split a colon-separated PATH-style string into individual entries.
// Empty entries are skipped — a leading or trailing ":" doesn't add "".
std::vector<std::filesystem::path> split_path(std::string_view s) {
  std::vector<std::filesystem::path> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t end = s.find(':', start);
    const std::size_t len = (end == std::string_view::npos ? s.size() : end) - start;
    if (len > 0) {
      out.emplace_back(std::string(s.substr(start, len)));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

// Compute the default XCursor search path. Mirrors libxcursor's
// built-in fallback: XCURSOR_PATH wins outright; otherwise we walk
// the user and system XDG roots with /icons appended, plus the
// /usr/share/pixmaps leftover that themes still drop cursors into.
std::vector<std::filesystem::path> default_search_paths() {
  const auto env_path = env_or_empty("XCURSOR_PATH");
  if (!env_path.empty()) {
    return split_path(env_path);
  }

  std::vector<std::filesystem::path> out;
  const auto home = env_or_empty("HOME");

  if (!home.empty()) {
    out.emplace_back(std::filesystem::path(std::string(home)) / ".icons");
  }

  // $XDG_DATA_HOME/icons with the spec-mandated fallback to
  // ~/.local/share/icons when XDG_DATA_HOME is unset.
  const auto xdg_home = env_or_empty("XDG_DATA_HOME");
  if (!xdg_home.empty()) {
    for (const auto& p : split_path(xdg_home)) {
      out.emplace_back(p / "icons");
    }
  } else if (!home.empty()) {
    out.emplace_back(std::filesystem::path(std::string(home)) / ".local" / "share" / "icons");
  }

  // $XDG_DATA_DIRS/icons with the spec's default of
  // /usr/local/share:/usr/share.
  const auto xdg_dirs = env_or_empty("XDG_DATA_DIRS");
  const auto dirs =
      xdg_dirs.empty() ? split_path("/usr/local/share:/usr/share") : split_path(xdg_dirs);
  for (const auto& p : dirs) {
    out.emplace_back(p / "icons");
  }

  out.emplace_back("/usr/share/pixmaps");
  return out;
}

// ---------------------------------------------------------------------------
// index.theme parser — we only read [Icon Theme] Inherits=, so do the
// minimum ini parsing inline rather than pulling in a dependency.
// ---------------------------------------------------------------------------

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string> parse_inherits(const std::filesystem::path& index_file) {
  std::ifstream in(index_file);
  if (!in) {
    return {};
  }

  bool in_icon_theme = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto t = trim(line);
    if (t.empty() || t.front() == '#') {
      continue;
    }
    if (t.front() == '[') {
      // Next section starts; stop if we were already inside Icon Theme.
      if (in_icon_theme) {
        break;
      }
      in_icon_theme = (t == "[Icon Theme]");
      continue;
    }
    if (!in_icon_theme) {
      continue;
    }
    constexpr std::string_view k_key = "Inherits";
    if (t.size() <= k_key.size() || t.substr(0, k_key.size()) != k_key) {
      continue;
    }
    auto rest = trim(t.substr(k_key.size()));
    if (rest.empty() || rest.front() != '=') {
      continue;
    }
    rest = trim(rest.substr(1));

    std::vector<std::string> parents;
    std::size_t start = 0;
    while (start <= rest.size()) {
      // Accept both ',' (spec) and ';' (some themes in the wild) as
      // separators. No known theme uses both in one list.
      const auto next = std::min(rest.find(',', start), rest.find(';', start));
      const std::size_t len = (next == std::string_view::npos ? rest.size() : next) - start;
      const auto parent = trim(rest.substr(start, len));
      if (!parent.empty()) {
        parents.emplace_back(parent);
      }
      if (next == std::string_view::npos) {
        break;
      }
      start = next + 1;
    }
    return parents;
  }
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

drm::expected<Theme, std::error_code> Theme::discover() {
  const auto paths = default_search_paths();
  return discover_with_paths(paths);
}

drm::expected<Theme, std::error_code> Theme::discover_with_paths(
    drm::span<const std::filesystem::path> search_paths) {
  auto impl = std::make_unique<Impl>();
  impl->search_paths.assign(search_paths.begin(), search_paths.end());

  // Walk each search path; treat each immediate subdirectory as a
  // candidate theme. We register a theme if it has cursors/, an
  // index.theme, or a cursor.theme. This keeps pure icon themes from
  // cluttering the index while still letting inheritance pass through
  // cursor-less intermediate themes.
  for (const auto& root : impl->search_paths) {
    std::error_code ec;
    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
      continue;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        break;
      }
      const auto& entry = *it;
      std::error_code sub_ec;
      if (!entry.is_directory(sub_ec) || sub_ec) {
        continue;
      }

      const auto& theme_dir = entry.path();
      const auto cursors = theme_dir / "cursors";
      const auto index_theme = theme_dir / "index.theme";
      const auto cursor_theme = theme_dir / "cursor.theme";

      const bool has_cursors = std::filesystem::exists(cursors, sub_ec);
      const bool has_index = std::filesystem::exists(index_theme, sub_ec);
      const bool has_cursor_theme = std::filesystem::exists(cursor_theme, sub_ec);
      if (!has_cursors && !has_index && !has_cursor_theme) {
        continue;
      }

      const std::string name = theme_dir.filename().string();
      if (name.empty()) {
        continue;
      }

      const auto [map_it, inserted] = impl->by_name.try_emplace(name, impl->themes.size());
      if (inserted) {
        impl->themes.push_back(Impl::Record{name, {theme_dir}, {}});
      } else {
        impl->themes[map_it->second].dirs.push_back(theme_dir);
      }

      // Parse Inherits= only once per theme name, on the first dir
      // that provides it. If two search-path copies disagree on their
      // Inherits list, the earlier path wins — the same precedence
      // rule used everywhere else.
      auto& record = impl->themes[map_it->second];
      if (record.inherits.empty()) {
        if (has_cursor_theme) {
          record.inherits = parse_inherits(cursor_theme);
        }
        if (record.inherits.empty() && has_index) {
          record.inherits = parse_inherits(index_theme);
        }
      }
    }
  }

  if (impl->themes.empty()) {
    // No theme directories readable. Return an error so callers know
    // cursor loading will fail wholesale — distinguishes "theme not
    // found" (resolve fails on an otherwise-healthy Theme) from "no
    // themes installed at all" (can't even construct).
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  return Theme(std::move(impl));
}

drm::expected<ThemeResolution, std::error_code> Theme::resolve(
    std::string_view cursor_name, std::string_view preferred_theme) const {
  // Defense in depth: cursor_name is caller-controlled, and filesystem
  // operator/ composes "../../etc/passwd"-style names into real
  // absolute paths. Compositors should sanitize upstream, but the cost
  // of rejecting ambiguous names here is one comparison. `..` alone
  // is the classic escape; `/` or `\` anywhere in the name means the
  // caller treated it as a path, not an identifier.
  //
  // Run the guard before any cache touch: a malformed name never
  // reaches the cache, so a caller that spams bogus names can't
  // bloat the map.
  if (cursor_name.empty() || cursor_name == "." || cursor_name == ".." ||
      cursor_name.find('/') != std::string_view::npos ||
      cursor_name.find('\\') != std::string_view::npos) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const Impl::CacheKey key{std::string(cursor_name), std::string(preferred_theme)};
  if (const auto it = impl_->resolve_cache.find(key); it != impl_->resolve_cache.end()) {
    return it->second;
  }

  // Helper: store result in the cache and return it. Done via a lambda
  // so the three exit paths (theme-chain hit, spec-fallback hit,
  // exhausted) share one memoization site.
  auto memoize = [&](drm::expected<ThemeResolution, std::error_code> result)
      -> drm::expected<ThemeResolution, std::error_code> {
    impl_->resolve_cache.emplace(key, result);
    return result;
  };

  // Effective starting theme: the caller's request wins, then
  // $XCURSOR_THEME, then "default" (which nearly every distro provides
  // as a symlink), then the first theme we indexed as a last resort.
  std::string start(preferred_theme);
  if (start.empty()) {
    const auto env_theme = env_or_empty("XCURSOR_THEME");
    if (!env_theme.empty()) {
      start.assign(env_theme);
    }
  }
  if (start.empty()) {
    start = "default";
  }

  // BFS through the Inherits graph so each theme is visited once in
  // spec-defined order. The chain doubles as a diagnostic in the
  // returned ThemeResolution.
  std::vector<std::string> chain;
  std::unordered_set<std::string> visited;
  std::vector<std::string> queue{start};
  while (!queue.empty()) {
    auto cur = std::move(queue.front());
    queue.erase(queue.begin());
    if (!visited.insert(cur).second) {
      continue;
    }
    const auto it = impl_->by_name.find(cur);
    chain.push_back(std::move(cur));
    if (it == impl_->by_name.end()) {
      continue;
    }
    for (const auto& parent : impl_->themes[it->second].inherits) {
      queue.push_back(parent);
    }
  }

  // For every theme in the chain, try every alias sibling of the
  // requested cursor name inside every dir that provides the theme.
  // First on-disk hit wins.
  auto try_cursors_dir = [&](const std::filesystem::path& cursors_dir,
                             std::filesystem::path& found_out) -> bool {
    bool matched = false;
    for_each_alias(cursor_name, [&](std::string_view alias) {
      if (matched) {
        return;
      }
      std::error_code ec;
      auto candidate = cursors_dir / std::string(alias);
      if (std::filesystem::exists(candidate, ec)) {
        found_out = std::move(candidate);
        matched = true;
      }
    });
    return matched;
  };

  for (const auto& theme : chain) {
    const auto map_it = impl_->by_name.find(theme);
    if (map_it == impl_->by_name.end()) {
      continue;
    }
    const auto& record = impl_->themes[map_it->second];
    std::filesystem::path found;
    for (const auto& dir : record.dirs) {
      if (try_cursors_dir(dir / "cursors", found)) {
        return memoize(ThemeResolution{theme, std::move(found), std::move(chain)});
      }
    }
  }

  // Spec fallback: if the inherits chain struck out, try every other
  // indexed theme in discovery order (user dirs first, then system).
  // The chain vector grows as we go so diagnostics reflect every theme
  // we actually looked in.
  for (const auto& record : impl_->themes) {
    if (std::find(chain.begin(), chain.end(), record.name) != chain.end()) {
      continue;
    }
    std::filesystem::path found;
    bool matched = false;
    for (const auto& dir : record.dirs) {
      if (try_cursors_dir(dir / "cursors", found)) {
        matched = true;
        break;
      }
    }
    chain.push_back(record.name);
    if (matched) {
      return memoize(ThemeResolution{record.name, std::move(found), std::move(chain)});
    }
  }

  return memoize(
      drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_file_or_directory)));
}

}  // namespace drm::cursor
