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
 * @file json_builder.h
 * @brief Minimal JSON builder for the simple JSON responses used by the
 *        CometVisu protocol.
 *
 * This is deliberately minimal — the CometVisu protocol only uses flat
 * JSON objects with string, number and boolean values, so a library with
 * its own DOM and serializer would be pure overhead on an embedded system.
 *
 * Every string is escaped: quotes, backslashes, the C escapes and all
 * remaining control characters (`\u00XX`).  Values are hex strings or fixed
 * protocol keys today, but the response must stay parsable whatever a caller
 * passes in.
 */

#pragma once

#include <string>
#include <string_view>

namespace cvknx {

/**
 * @brief Minimal JSON builder.
 *
 * Builds JSON strings incrementally with automatic comma insertion (each
 * `add_*` call writes the separator itself, so callers never track whether
 * they are first).  Balance is not validated: every caller emits a fixed,
 * known shape.
 *
 * Example:
 * @code
 *   JsonBuilder jb;
 *   jb.start_object();
 *   jb.add_string("v", "0.0.2");
 *   jb.add_string("s", session_id);
 *   jb.end_object();
 *   std::string json = jb.take();  // {"v":"0.0.2","s":"abc123"}
 * @endcode
 */
class JsonBuilder {
public:
  JsonBuilder() = default;

  /// Open a nested object and reset the comma state for its first member.
  void start_object();

  /// Close the current object; the enclosing object continues with a member.
  void end_object();

  /// Add a string key-value pair: "key":"value"
  void add_string(std::string_view key, std::string_view value);

  /// Add an integer key-value pair: "key":<value> (unquoted, minimal JSON)
  void add_number(std::string_view key, int64_t value);

  /// Add a boolean key-value pair: "key":true / "key":false
  void add_bool(std::string_view key, bool value);

  /// Add a nested object as a value.
  /// Caller is responsible for calling start/end_object before/after.
  void add_key(std::string_view key);

  /// Append raw JSON (for already-built sub-objects).
  void add_raw(std::string_view raw);

  /// Get the built JSON string without consuming it (for tests and logging).
  [[nodiscard]] const std::string& str() const { return buffer_; }

  /// Move the built JSON string out; the builder is empty afterwards.
  [[nodiscard]] std::string take() { return std::move(buffer_); }

  /// Reset to an empty object state so the instance can be reused.
  void clear() {
    buffer_.clear();
    first_ = true;
  }

private:
  std::string buffer_;
  /// True until the first member of the current object has been written; the
  /// next `add_*` call must not emit a comma.
  bool first_ = true;

  void add_comma();
  void add_quoted(std::string_view str);
};

}  // namespace cvknx
