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
 * @file login_handler.cpp
 * @brief Implementation of the CometVisu /l (login) endpoint handler.
 *
 * Creates a session and returns a JSON response with protocol version,
 * session ID, and optional configuration block.  The configuration block
 * includes compile-time version info for diagnostics.
 */

#include "login_handler.h"

#include <cstdlib>

#include "state/session_store.h"
#include "util/json_builder.h"
#include "util/query_string.h"
#include "version.h"

namespace cvknx {

/// CometVisu protocol version reported in the login response "v" field.
/// This is the wire-protocol version, not the software version.
static constexpr std::string_view kProtocolVersion = "0.0.2";

LoginHandler::LoginHandler(SessionStore& sessions, std::string base_url)
    : sessions_(sessions), base_url_(std::move(base_url)) {}

std::string LoginHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};

  // Authentication is the web server's job: `u`/`p` are read but never
  // validated, they only decide whether this is an anonymous session (no
  // CometVisu login configured).  The anonymous id "0" is a valid session id
  // for every later `/r` and `/w`, so a credential-less front end works.
  const bool anonymous = !params.has("u") && !params.has("p");
  const std::string session_id = sessions_.create_session(anonymous);

  JsonBuilder json;
  json.start_object();
  json.add_string("v", kProtocolVersion);
  json.add_string("s", session_id);

  // LOGIN_EXTRA_CONFIG: optional environment variable with raw JSON key-value
  // pairs to inject into the "c" block (e.g. `"foo":"bar","knxdRuntimeVersion":"1.2.3"`).
  const char* extra_config = std::getenv("LOGIN_EXTRA_CONFIG");  // NOLINT(concurrency-mt-unsafe)

  add_login_config_block(
      json, base_url_,
      LoginBuildInfo{.fcgi_version = version(),
                     .fcgi_hash = git_hash(),
                     .knxd_version = knxd_build_version(),
                     .knxd_hash = knxd_build_git_hash(),
                     .extra_config = extra_config != nullptr ? std::string_view{extra_config}
                                                             : std::string_view{}});

  json.end_object();

  return json.take();
}

void add_login_config_block(JsonBuilder& json, std::string_view base_url,
                            const LoginBuildInfo& info) {
  // "unknown" is what version()/git_hash() return when CMake could not inject
  // the value — it carries no information and must not create a "c" block.
  const auto known = [](std::string_view value) { return !value.empty() && value != "unknown"; };

  const bool need_config = !base_url.empty() || known(info.fcgi_version) || known(info.fcgi_hash) ||
                           !info.knxd_version.empty() || !info.knxd_hash.empty() ||
                           !info.extra_config.empty();
  if (!need_config) {
    return;
  }

  json.add_key("c");
  json.start_object();

  if (!base_url.empty()) {
    json.add_string("baseURL", base_url);
  }
  if (known(info.fcgi_version)) {
    json.add_string("fcgiVersion", info.fcgi_version);
  }
  if (known(info.fcgi_hash)) {
    json.add_string("fcgiGitHash", info.fcgi_hash);
  }
  if (!info.knxd_version.empty()) {
    json.add_string("knxdBuildVersion", info.knxd_version);
  }
  if (!info.knxd_hash.empty()) {
    json.add_string("knxdBuildGitHash", info.knxd_hash);
  }
  if (!info.extra_config.empty()) {
    json.add_raw(info.extra_config);
  }

  json.end_object();
}

}  // namespace cvknx
