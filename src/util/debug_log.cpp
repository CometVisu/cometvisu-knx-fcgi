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
 * @file debug_log.cpp
 * @brief Implementation of the debug logging facility.
 */

#include "debug_log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cvknx {

namespace {

/// True when @p pos starts a `u=` or `p=` parameter (credentials).
bool is_credential_parameter(std::string_view text, std::size_t pos) {
  if (pos != 0) {
    const char prev = text[pos - 1];
    if (prev != '&' && prev != '?') {
      return false;
    }
  }
  return (text[pos] == 'u' || text[pos] == 'p') && pos + 1 < text.size() && text[pos + 1] == '=';
}

/// Make an externally supplied string safe for a log line.
///
/// Two problems with logging raw input:
///   - a CR/LF (or any control character) lets a client forge log lines and
///     hide what it did;
///   - the CometVisu login sends `p=<password>`, which must never be written
///     to a log file.  Both credentials are redacted here.
std::string sanitize_for_log(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (is_credential_parameter(text, i)) {
      out.append(std::string_view{text.data() + i, 2});  // keep "u="/"p="
      out.append("***");
      i += 2;
      while (i < text.size() && text[i] != '&') {
        ++i;  // skip the value
      }
      continue;
    }
    const auto uc = static_cast<unsigned char>(text[i]);
    out.push_back((uc < 0x20 || uc == 0x7F) ? '?' : text[i]);
    ++i;
  }
  return out;
}

}  // namespace

bool DebugLog::enabled_ = false;
size_t DebugLog::max_uri_length_ = 500;
size_t DebugLog::max_body_length_ = 1000;

void DebugLog::init_from_env() {
  const char* val = std::getenv("DEBUG_BACKEND");
  if (val == nullptr || *val == '\0') {
    enabled_ = false;
    return;
  }
  // Accept "1", "true", "yes", "on" (exact, case sensitive).
  enabled_ = (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0 ||
              std::strcmp(val, "yes") == 0 || std::strcmp(val, "on") == 0);
}

void DebugLog::write_timestamp(std::ostream& os) {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_now = {};
  localtime_r(&time_t_now, &tm_now);

  os << "[" << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0')
     << std::setw(3) << ms.count() << "] ";
}

bool DebugLog::write_truncated(std::ostream& os, std::string_view text, size_t max_len) {
  if (max_len == 0 || text.size() <= max_len) {
    os << text;
    return false;
  }
  // Truncate: show first max_len chars
  os << text.substr(0, max_len) << "... (truncated, " << text.size() << " chars total)";
  return true;
}

void DebugLog::http_request(std::string_view method, std::string_view uri) {
  if (!enabled_) {
    return;
  }

  // Build the entire log line in a local buffer to write it atomically.
  // Multiple processes share stderr; building then writing in one call
  // minimises (but cannot completely eliminate) interleaving.
  const std::string safe_uri = sanitize_for_log(uri);
  std::ostringstream oss;
  write_timestamp(oss);
  oss << "→ HTTP REQUEST: " << method << " ";
  write_truncated(oss, safe_uri, max_uri_length_);
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::http_response(int status_code, std::string_view body) {
  if (!enabled_) {
    return;
  }

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "← HTTP RESPONSE: " << status_code;
  if (!body.empty()) {
    oss << " body=";
    write_truncated(oss, body, max_body_length_);
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::knxd_send(std::string_view operation, std::string_view address,
                         std::string_view details) {
  if (!enabled_) {
    return;
  }

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "  → KNXD SEND: " << operation << " addr=" << address;
  if (!details.empty()) {
    oss << " " << details;
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::knxd_recv(std::string_view operation, std::string_view address,
                         std::string_view data) {
  if (!enabled_) {
    return;
  }

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "  ← KNXD RECV: " << operation << " addr=" << address;
  if (!data.empty()) {
    oss << " data=" << data;
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::log(Level level, std::string_view message) {
  const char* name = "INFO";
  if (level == Level::Error) {
    name = "ERROR";
  } else if (level == Level::Warn) {
    name = "WARN";
  }

  // Build the complete line first: stderr is shared by every worker process
  // and a single write keeps the fields from interleaving.
  std::ostringstream oss;
  oss << "[" << name << "] ";
  write_timestamp(oss);
  oss << message << "\n";
  std::cerr << oss.str() << std::flush;
}

}  // namespace cvknx
