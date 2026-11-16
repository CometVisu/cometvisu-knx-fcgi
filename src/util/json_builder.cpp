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
 * @file json_builder.cpp
 * @brief Implementation of the minimal JSON builder.
 */

#include "json_builder.h"

namespace cvknx {

void JsonBuilder::start_object() {
  buffer_.push_back('{');
  // The first member of a nested object must not be preceded by a comma.
  first_ = true;
}

void JsonBuilder::end_object() {
  buffer_.push_back('}');
  // The object itself is a value of its parent, so the next member written to
  // the parent needs a separator.
  first_ = false;
}

void JsonBuilder::add_comma() {
  if (!first_) {
    buffer_.push_back(',');
  }
}

void JsonBuilder::add_quoted(std::string_view str) {
  buffer_.push_back('"');
  // Escape every character that would end the string early (`"`, `\\`) or
  // break the line format (CR/LF/TAB), plus the remaining C0 controls and DEL
  // as \u00XX.  Today every value this builder sees is either a fixed
  // protocol key or a hex string, but a single unescaped control character
  // would make the whole response unparsable for the client — escaping is a
  // few instructions, a broken response is a support case.
  for (char c : str) {
    switch (c) {
      case '"':
        buffer_.push_back('\\');
        buffer_.push_back('"');
        break;
      case '\\':
        buffer_.push_back('\\');
        buffer_.push_back('\\');
        break;
      case '\n':
        buffer_.push_back('\\');
        buffer_.push_back('n');
        break;
      case '\r':
        buffer_.push_back('\\');
        buffer_.push_back('r');
        break;
      case '\t':
        buffer_.push_back('\\');
        buffer_.push_back('t');
        break;
      default:
        if (const auto uc = static_cast<unsigned char>(c); uc < 0x20 || uc == 0x7F) {
          static constexpr char kHexDigits[] = "0123456789abcdef";
          buffer_.append("\\u00");
          buffer_.push_back(kHexDigits[uc >> 4]);
          buffer_.push_back(kHexDigits[uc & 0x0F]);
        } else {
          buffer_.push_back(c);
        }
        break;
    }
  }
  buffer_.push_back('"');
}

void JsonBuilder::add_string(std::string_view key, std::string_view value) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  add_quoted(value);
  first_ = false;
}

void JsonBuilder::add_number(std::string_view key, int64_t value) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  buffer_.append(std::to_string(value));
  first_ = false;
}

void JsonBuilder::add_bool(std::string_view key, bool value) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  buffer_.append(value ? "true" : "false");
  first_ = false;
}

void JsonBuilder::add_key(std::string_view key) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  first_ = true;  // next value is first in nested object
}

void JsonBuilder::add_raw(std::string_view raw) {
  add_comma();
  buffer_.append(raw);
  first_ = false;
}

}  // namespace cvknx
