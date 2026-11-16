// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file query_string.h
 * @brief FastCGI query string parser with multi-value support.
 *
 * Parses URL-encoded query strings (application/x-www-form-urlencoded) into
 * a key→values map.  Supports multiple values for the same key (the `a`
 * parameter commonly appears multiple times in /r and /w requests).
 *
 * Enforces limits on unique keys, values per key, and total pairs to
 * prevent memory exhaustion from maliciously crafted query strings.
 *
 * The limits are sized for real CometVisu installations: the client sends
 * ALL addresses of a visualization in a single request, so configs with
 * several thousand group addresses must work.  Exceeding a limit is never
 * silent — see limit_exceeded(); handlers turn that into an HTTP 400.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cvknx {

/**
 * @brief Transparent hash for heterogeneous lookup with string_view in
 *        unordered_map<string, ...> containers (C++20).
 *
 * The `is_transparent` tag enables find() with string_view keys without
 * constructing a temporary std::string.
 */
struct StringHash {
  using is_transparent = void;  // enables heterogeneous lookup

  [[nodiscard]] size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
  [[nodiscard]] size_t operator()(const std::string& s) const {
    return std::hash<std::string>{}(s);
  }
};

struct StringEqual {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

/**
 * @brief Parsed query string from an HTTP/FCGI request.
 *
 * Supports multiple values for the same key (multi-valued parameters).
 * Uses heterogeneous lookup so get("a") works with string_view without
 * allocating a std::string.
 */
class QueryString {
public:
  /// Maximum number of unique parameter keys accepted in a single query string.
  /// CometVisu uses a handful of keys (`i`, `t`, `s`, `a`, `v`), so this is
  /// already far beyond any legitimate request.
  static constexpr size_t kMaxQueryParams = 256;

  /// Maximum number of values per single key.  The `a` parameter carries one
  /// entry per subscribed group address; 4096 addresses produce a ~50 KB query
  /// string, i.e. more than any web server's default request line limit
  /// (Apache `LimitRequestLine` is 8190 by default, 32768 in the CometVisu
  /// container).
  ///
  /// Historical note: this limit used to be 250, which silently truncated the
  /// address list of large CometVisu configs and left the affected tiles
  /// without a value forever (CometVisu issue #1521).
  static constexpr size_t kMaxValuesPerKey = 4096;

  /// Absolute maximum total key=value pairs parsed from a query string.
  /// Bounds the memory a single request can allocate (worst case ~1 MB).
  static constexpr size_t kMaxTotalPairs = 8192;

  /// Parse a raw query string (e.g. "a=1/2/3&t=5&a=4/5/6").
  explicit QueryString(std::string_view raw);

  /// Get the first value for a key, or std::nullopt if not present.
  [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;

  /// Get all values for a key.
  [[nodiscard]] std::vector<std::string_view> get_all(std::string_view key) const;

  /// Check if a key exists.
  [[nodiscard]] bool has(std::string_view key) const;

  /// @brief True if at least one parameter was dropped because a limit was hit.
  ///
  /// The parser never truncates silently: callers (the request handlers) must
  /// check this flag and answer with HTTP 400 so a client with an oversized
  /// request sees the problem instead of receiving incomplete data.
  [[nodiscard]] bool limit_exceeded() const { return limit_exceeded_; }

  /// Get the number of unique keys.
  [[nodiscard]] size_t size() const { return params_.size(); }

private:
  std::unordered_map<std::string, std::vector<std::string>, StringHash, StringEqual> params_;
  bool limit_exceeded_ = false;
};

}  // namespace cvknx
