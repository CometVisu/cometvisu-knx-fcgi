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
 * @file query_string.cpp
 * @brief Implementation of the query string parser.
 *
 * The parser is deliberately lenient about *content* and strict about *size*:
 * a malformed pair is skipped instead of rejecting the whole request (the
 * CometVisu client appends parameters of different generations, and a single
 * stray byte must not break a working installation), while the pair/key/value
 * limits keep the work per request bounded — the query string arrives straight
 * from an untrusted HTTP client.
 */

#include "query_string.h"

namespace cvknx {

namespace {

/// Decode `%XX` escapes and `+` in a key or value.
///
/// A `%` that is not followed by two hex digits is *kept verbatim* rather than
/// dropped: the caller (the handlers) logs and echoes addresses back to the
/// client, so silently deleting bytes would hide what was actually sent.  The
/// same reason keeps the function total — it never fails.
std::string url_decode(std::string_view str) {
  std::string result;
  result.reserve(str.size());
  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
          return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
          return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
          return c - 'A' + 10;
        }
        return -1;
      };
      int hi = from_hex(str[i + 1]);
      int lo = from_hex(str[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    // `+` is the application/x-www-form-urlencoded escape for a space (the
    // format FastCGI uses for QUERY_STRING); decode it here, not in the caller.
    if (str[i] == '+') {
      result.push_back(' ');
    } else {
      result.push_back(str[i]);
    }
  }
  return result;
}

}  // namespace

QueryString::QueryString(std::string_view raw) {
  if (raw.empty()) {
    return;
  }

  size_t pos = 0;
  size_t total_pairs = 0;
  // Split on the raw `&`/`=` delimiters *before* decoding, so an encoded
  // `%26`/`%3D` inside a value can never fabricate a new pair.
  while (pos < raw.size()) {
    // The limits are the only thing between a hostile query string and
    // unbounded memory use — the request itself is already fully in memory.
    if (total_pairs >= kMaxTotalPairs) {
      limit_exceeded_ = true;
      break;
    }
    ++total_pairs;

    const size_t eq = raw.find('=', pos);
    const size_t amp = raw.find('&', pos);

    if (eq == std::string_view::npos || (amp != std::string_view::npos && eq > amp)) {
      // Malformed: no '=' before next '&', skip this pair
      pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
      continue;
    }

    const std::string key{url_decode(raw.substr(pos, eq - pos))};
    const size_t val_end = (amp == std::string_view::npos) ? raw.size() : amp;
    std::string value{url_decode(raw.substr(eq + 1, val_end - eq - 1))};

    // Enforce max unique keys and max values per key
    auto it = params_.find(key);
    if (it != params_.end()) {
      // Existing key — enforce max values per key
      if (it->second.size() >= kMaxValuesPerKey) {
        limit_exceeded_ = true;
        pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
        continue;  // skip this value, key is full
      }
      it->second.push_back(std::move(value));
    } else {
      // New key — enforce max unique keys
      if (params_.size() >= kMaxQueryParams) {
        limit_exceeded_ = true;
        pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
        continue;  // skip this key, map is full
      }
      params_[key].push_back(std::move(value));
    }
    pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
  }
}

std::optional<std::string_view> QueryString::get(std::string_view key) const {
  auto it = params_.find(key);
  // A key only exists while it has at least one value, so no empty() check.
  if (it != params_.end()) {
    return std::string_view{it->second.front()};
  }
  return std::nullopt;
}

std::vector<std::string_view> QueryString::get_all(std::string_view key) const {
  auto it = params_.find(key);
  if (it != params_.end()) {
    std::vector<std::string_view> result;
    result.reserve(it->second.size());
    for (const auto& v : it->second) {
      result.push_back(std::string_view{v});
    }
    return result;
  }
  return {};
}

bool QueryString::has(std::string_view key) const {
  return params_.find(key) != params_.end();
}

}  // namespace cvknx
