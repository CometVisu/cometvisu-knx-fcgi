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

#include <string>

#include "fcgi/fcgi_request.h"

namespace cvknx {
namespace {

FcgiRequest make_request(std::string request_uri, std::string script_name = "") {
  FcgiRequest request;
  request.request_method = "GET";
  request.request_uri = std::move(request_uri);
  request.script_name = std::move(script_name);
  return request;
}

TEST(FcgiRequestTest, PrefersPathInfo) {
  FcgiRequest request = make_request("/cgi-bin/visu/l?x=1", "/cgi-bin/visu");
  request.path_info = "/l";
  EXPECT_EQ(request.path(), "/l");
}

TEST(FcgiRequestTest, StripsQueryStringFromRequestUri) {
  EXPECT_EQ(make_request("/l?a=1/2/3").path(), "/l");
}

TEST(FcgiRequestTest, StripsScriptNamePrefix) {
  EXPECT_EQ(make_request("/cgi-bin/visu/r", "/cgi-bin/visu").path(), "/r");
}

TEST(FcgiRequestTest, UsesScriptNameTailWhenItConsumesTheWholeUri) {
  // SCRIPT_NAME == request path: the endpoint is the last component.
  EXPECT_EQ(make_request("/cgi-bin/l", "/cgi-bin/l").path(), "/l");
}

TEST(FcgiRequestTest, KeepsUriPathWhenScriptNameIsNotAPrefix) {
  EXPECT_EQ(make_request("/l", "/cgi-bin/visu").path(), "/l");
}

TEST(FcgiRequestTest, KeepsUriPathWhenItIsShorterThanScriptName) {
  EXPECT_EQ(make_request("/l", "/cgi-bin/visu/longer").path(), "/l");
}

TEST(FcgiRequestTest, FallsBackToScriptNameWithoutRequestUri) {
  EXPECT_EQ(make_request("", "/cgi-bin/l").path(), "/l");
}

TEST(FcgiRequestTest, FallsBackToScriptNameWithoutSlash) {
  // No '/' in SCRIPT_NAME — nothing to return.
  EXPECT_EQ(make_request("", "l").path(), "");
}

TEST(FcgiRequestTest, IgnoresScriptNameThatIsOnlyASlash) {
  EXPECT_EQ(make_request("", "/").path(), "");
}

TEST(FcgiRequestTest, ReturnsEmptyWithoutUriAndScriptName) {
  EXPECT_EQ(make_request("").path(), "");
}

}  // namespace

TEST(FcgiRequestTest, FallsBackWhenScriptNameIsNotAnExactPrefix) {
  // Same length as SCRIPT_NAME but a different character at position 5.
  EXPECT_EQ(make_request("/cgi-bin/r", "/cgi-bin/l").path(), "/cgi-bin/r");
}

TEST(FcgiRequestTest, UsesUriPathWhenScriptNameHasNoSlash) {
  // path == SCRIPT_NAME, and SCRIPT_NAME contains no '/' to fall back to.
  EXPECT_EQ(make_request("l", "l").path(), "l");
}
}  // namespace cvknx
