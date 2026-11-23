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

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "handlers/login_handler.h"
#include "state/session_store.h"
#include "util/json_builder.h"
#include "version.h"

using namespace cvknx;

namespace {

/// Helper: set an environment variable for the duration of a test case.
class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnvVar() { unsetenv(name_); }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
  const char* name_;
};

// ================================================================
// Config block builder — every combination of known/unknown build info
// ================================================================

TEST(LoginConfigBlockTest, EmptyInfoProducesNoBlock) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{});
  json.end_object();
  EXPECT_EQ(json.take(), "{}");
}

TEST(LoginConfigBlockTest, UnknownValuesAreNotReported) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "",
                         LoginBuildInfo{.fcgi_version = "unknown", .fcgi_hash = "unknown"});
  json.end_object();
  EXPECT_EQ(json.take(), "{}");
}

TEST(LoginConfigBlockTest, OnlyFcgiVersion) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{.fcgi_version = "2.0.0"});
  json.end_object();
  const std::string out = json.take();
  EXPECT_NE(out.find(R"("fcgiVersion":"2.0.0")"), std::string::npos) << out;
  EXPECT_EQ(out.find("fcgiGitHash"), std::string::npos) << out;
}

TEST(LoginConfigBlockTest, OnlyFcgiHash) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{.fcgi_hash = "abc1234"});
  json.end_object();
  const std::string out = json.take();
  EXPECT_NE(out.find(R"("fcgiGitHash":"abc1234")"), std::string::npos) << out;
  EXPECT_EQ(out.find("fcgiVersion"), std::string::npos) << out;
}

TEST(LoginConfigBlockTest, OnlyKnxdVersion) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{.knxd_version = "0.14.57"});
  json.end_object();
  const std::string out = json.take();
  EXPECT_NE(out.find(R"("knxdBuildVersion":"0.14.57")"), std::string::npos) << out;
  EXPECT_EQ(out.find("knxdBuildGitHash"), std::string::npos) << out;
}

TEST(LoginConfigBlockTest, OnlyKnxdHash) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{.knxd_hash = "deadbee"});
  json.end_object();
  const std::string out = json.take();
  EXPECT_NE(out.find(R"("knxdBuildGitHash":"deadbee")"), std::string::npos) << out;
  EXPECT_EQ(out.find("knxdBuildVersion"), std::string::npos) << out;
}

TEST(LoginConfigBlockTest, OnlyExtraConfig) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "", LoginBuildInfo{.extra_config = R"("foo":"bar")"});
  json.end_object();
  EXPECT_NE(json.take().find(R"("foo":"bar")"), std::string::npos);
}

TEST(LoginConfigBlockTest, EverythingKnownIsReportedTogether) {
  JsonBuilder json;
  json.start_object();
  add_login_config_block(json, "/proxy/visu",
                         LoginBuildInfo{.fcgi_version = "2.0.0",
                                        .fcgi_hash = "abc1234",
                                        .knxd_version = "0.14.57",
                                        .knxd_hash = "deadbee",
                                        .extra_config = R"("x":1)"});
  json.end_object();
  const std::string out = json.take();
  for (const char* fragment :
       {R"("baseURL":"/proxy/visu")", R"("fcgiVersion":"2.0.0")", R"("fcgiGitHash":"abc1234")",
        R"("knxdBuildVersion":"0.14.57")", R"("knxdBuildGitHash":"deadbee")", R"("x":1)"}) {
    EXPECT_NE(out.find(fragment), std::string::npos) << fragment << " missing in " << out;
  }
}

TEST(LoginHandlerTest, UserWithoutPasswordIsAuthenticated) {
  SessionStore sessions;
  LoginHandler handler(sessions);
  const std::string body = handler.handle("u=admin");
  // Only "u" was given, so this is not the anonymous path (id != "0").
  EXPECT_EQ(body.find(R"("s":"0")"), std::string::npos) << body;
}

TEST(LoginHandlerTest, PasswordWithoutUserIsAuthenticated) {
  SessionStore sessions;
  LoginHandler handler(sessions);
  const std::string body = handler.handle("p=secret");
  EXPECT_EQ(body.find(R"("s":"0")"), std::string::npos) << body;
}
}  // namespace

TEST(LoginHandlerTest, AnonymousLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, AuthenticatedLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=admin&p=secret");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  // Session ID must NOT be "0"
  EXPECT_EQ(response.find("\"s\":\"0\""), std::string::npos);
  // Session ID should be a non-empty hex string
  auto spos = response.find("\"s\":\"");
  ASSERT_NE(spos, std::string::npos);
  auto epos = response.find('"', spos + 5);
  ASSERT_NE(epos, std::string::npos);
  std::string sid = response.substr(spos + 5, epos - spos - 5);
  EXPECT_FALSE(sid.empty());
  EXPECT_NE(sid, "0");
}

TEST(LoginHandlerTest, AnonymousWhenNoUserOrPassword) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  // No u or p params → anonymous
  std::string response = handler.handle("d=mydevice");
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, ValidJsonStructure) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=test&p=test");
  EXPECT_EQ(response.front(), '{');
  EXPECT_EQ(response.back(), '}');
  // Must have exactly v and s keys
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":"), std::string::npos);
}

TEST(LoginHandlerTest, DifferentSessionsAreUnique) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string r1 = handler.handle("u=a&p=b");
  std::string r2 = handler.handle("u=c&p=d");

  auto extract_sid = [](const std::string& json) -> std::string {
    auto s = json.find("\"s\":\"");
    if (s == std::string::npos) {
      return "";
    }
    auto e = json.find('"', s + 5);
    if (e == std::string::npos) {
      return "";
    }
    return json.substr(s + 5, e - s - 5);
  };

  std::string sid1 = extract_sid(r1);
  std::string sid2 = extract_sid(r2);
  EXPECT_NE(sid1, "0");
  EXPECT_NE(sid2, "0");
  EXPECT_NE(sid1, sid2);
}

TEST(LoginHandlerTest, ConfigBlockAbsentWhenNoUrlPath) {
  // The "c" block now always appears (version info), but "baseURL" is absent
  // when no base_url was given.
  SessionStore sessions;
  LoginHandler handler(sessions);  // default base_url = ""

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockPresentWhenUrlPathSet) {
  SessionStore sessions;
  LoginHandler handler(sessions, "/proxy/visu");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_NE(response.find("\"baseURL\""), std::string::npos);
  EXPECT_NE(response.find("\"/proxy/visu\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockWithCustomUrlPath) {
  SessionStore sessions;
  LoginHandler handler(sessions, "/custom/prefix");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/custom/prefix\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockIgnoredWhenUrlPathEmpty) {
  // The "c" block appears (version info), but "baseURL" is absent.
  SessionStore sessions;
  LoginHandler handler(sessions, "");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

// ============================================================================
// Regression test: BASE_URL from process environment must NOT be read.
//
// The original bug: LoginHandler used std::getenv("BASE_URL") at request time.
// In fork-based FCGI mode, FCGX_Accept_r() replaces environ with FCGI_PARAMS,
// which may not include BASE_URL.  Result: the "c" / "baseURL" config block
// silently disappeared from the login response even though the variable was
// correctly set in the process environment.
//
// The fix: inject base_url via the constructor at startup (before any FCGI
// accept), so the handler is decoupled from the run-time environ pointer.
// ============================================================================

TEST(LoginHandlerTest, ConfigBlockNotReadFromEnvironment) {
  // BASE_URL is set in the process environment ...
  ScopedEnvVar set("BASE_URL", "/proxy/visu");

  // ... but NOT injected via constructor (simulating FCGI environ swap).
  SessionStore sessions;
  LoginHandler handler(sessions);  // default base_url = ""

  // The "c" block appears (version info), but "baseURL" must NOT be
  // present — handler must NOT read getenv("BASE_URL").
  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockRequiresExplicitInjectionNotEnvVar) {
  // Even with BASE_URL in the environment ...
  ScopedEnvVar set("BASE_URL", "/secret/prefix");

  // ... only the constructor parameter controls the output.
  SessionStore sessions;
  LoginHandler handler(sessions, "/explicit/prefix");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/explicit/prefix\""), std::string::npos);
  EXPECT_EQ(response.find("/secret/prefix"), std::string::npos);
}

// ============================================================================
// Version info in the "c" block — always included when available
// ============================================================================

TEST(LoginHandlerTest, ConfigBlockIncludesVersionInfo) {
  // Version info is included even without baseURL.
  // The compile-time values come from the build system; runtime from popen.
  SessionStore sessions;
  LoginHandler handler(sessions);  // no base_url

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_NE(response.find("\"fcgiVersion\":"), std::string::npos);
  // The git hash is only advertised when the build could determine one:
  // builds from a source tarball (or a checkout whose ownership git refuses)
  // have no hash, and then the key must be omitted rather than sent empty.
  const bool have_git_hash = !git_hash().empty() && git_hash() != "unknown";
  EXPECT_EQ(response.find("\"fcgiGitHash\":") != std::string::npos, have_git_hash);
}

TEST(LoginHandlerTest, ConfigBlockWithBaseUrlIncludesVersionInfo) {
  // When base_url is set, baseURL and all version keys are present.
  SessionStore sessions;
  LoginHandler handler(sessions, "/proxy/visu");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/proxy/visu\""), std::string::npos);
  EXPECT_NE(response.find("\"fcgiVersion\":"), std::string::npos);
  const bool have_git_hash = !git_hash().empty() && git_hash() != "unknown";
  EXPECT_EQ(response.find("\"fcgiGitHash\":") != std::string::npos, have_git_hash);
}

TEST(LoginHandlerTest, LogExtraConfigEnvVar) {
  // When LOGIN_EXTRA_CONFIG is set, its raw JSON content is injected
  // into the "c" block of the login response.
  SessionStore sessions;
  LoginHandler handler(sessions);

  {
    ScopedEnvVar set("LOGIN_EXTRA_CONFIG", R"("foo":"bar","knxdRuntimeVersion":"1.2.3")");
    std::string response = handler.handle("");
    EXPECT_NE(response.find("\"c\""), std::string::npos);
    EXPECT_NE(response.find("\"foo\":\"bar\""), std::string::npos);
    EXPECT_NE(response.find("\"knxdRuntimeVersion\":\"1.2.3\""), std::string::npos);
  }

  // Without the env var, no extra config is injected
  std::string response_no_env = handler.handle("");
  // Still produces version info, but NOT the extra stuff
  EXPECT_EQ(response_no_env.find("\"foo\":\"bar\""), std::string::npos);
}

TEST(LoginHandlerTest, LogExtraConfigEmptyIgnored) {
  // An empty LOGIN_EXTRA_CONFIG must not inject anything.
  SessionStore sessions;
  LoginHandler handler(sessions);

  ScopedEnvVar set("LOGIN_EXTRA_CONFIG", "");
  std::string response = handler.handle("");
  EXPECT_EQ(response.find("\"knxdRuntimeVersion\""), std::string::npos);
}
