/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_virtual_file_system.hh"

#include "MEM_guardedalloc.h"

namespace blender::bke::tests {
using namespace blender::vse;

class VFSTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    gtest_setup();
  }
  static void TearDownTestSuite()
  {
    gtest_teardown();
  }
};

TEST_F(VFSTest, ParseNull)
{
  auto p = VFSPath::parse(nullptr);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "/");
}

TEST_F(VFSTest, ParseEmpty)
{
  auto p = VFSPath::parse("");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "/");
}

TEST_F(VFSTest, ParseWebDAVWithSlash)
{
  auto p = VFSPath::parse("webdav://example.com:8080/project");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::WebDAV);
  EXPECT_EQ(p->endpoint, "example.com:8080");
  EXPECT_EQ(p->path, "/project");
}

TEST_F(VFSTest, ParseWebDAVNoSlash)
{
  auto p = VFSPath::parse("webdav://server/");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::WebDAV);
  EXPECT_EQ(p->endpoint, "server");
  EXPECT_EQ(p->path, "/");
}

TEST_F(VFSTest, ParseLocalSlash)
{
  auto p = VFSPath::parse("file:///home/user/docs");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_TRUE(p->endpoint.empty());
  EXPECT_EQ(p->path, "/home/user/docs");
}

TEST_F(VFSTest, ParsePlainPathDefaultsLocal)
{
  auto p = VFSPath::parse("/tmp/test");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
}

TEST_F(VFSTest, TildeStripped)
{
  auto p = VFSPath::parse("~/some/path");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->path, "/some/path");
}

TEST_F(VFSTest, RoundTripWebDAV)
{
  const char *original = "webdav://myhost:9000/data/models";
  auto parsed = VFSPath::parse(original);
  ASSERT_TRUE(parsed.has_value());
  std::string rebuilt = parsed->to_string();
  EXPECT_EQ(rebuilt, original);
}

TEST_F(VFSTest, RoundTripLocal)
{
  auto parsed = VFSPath::parse("/var/tmp/blend");
  ASSERT_TRUE(parsed.has_value());
  std::string rebuilt = parsed->to_string();
  EXPECT_EQ(rebuilt, "file:///var/tmp/blend");
}

TEST_F(VFSTest, DefaultResultIsSuccess)
{
  VFSResult r;
  EXPECT_TRUE(r.success);
}

TEST_F(VFSTest, ParseWindowsDriveLetter)
{
  auto p = VFSPath::parse("C:/Users/jeroen");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "C:/Users/jeroen");
}

TEST_F(VFSTest, ParseWindowsDriveLetterBackslash)
{
  auto p = VFSPath::parse("C:\\Users\\jeroen");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "C:/Users/jeroen");
}

TEST_F(VFSTest, ParseWindowsDriveLetterRoot)
{
  auto p = VFSPath::parse("D:");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "D:");
}

TEST_F(VFSTest, ParseWindowsDriveLetterNormalize)
{
  VFSPath p = *VFSPath::parse("C:/Users");
  p.normalize();
  EXPECT_EQ(p.path, "C:/Users/");
}

TEST_F(VFSTest, ParseWindowsDriveLetterLocalPrefix)
{
  auto p = VFSPath::parse("file:///C:/Users");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p->path, "C:/Users");
}

TEST_F(VFSTest, RoundTripWindowsLocal)
{
  auto p = VFSPath::parse("C:/Users/jeroen");
  ASSERT_TRUE(p.has_value());
  std::string rebuilt = p->to_string();
  EXPECT_EQ(rebuilt, "file:///C:/Users/jeroen");
}

TEST_F(VFSTest, ParentLocalPath)
{
  VFSPath p = *VFSPath::parse("file:///home/user/docs");
  VFSPath parent = p.parent();
  EXPECT_EQ(parent.protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(parent.path, "/home/user");
}

TEST_F(VFSTest, ParentWebDAV)
{
  VFSPath p = *VFSPath::parse("webdav://example.com:8080/project/subdir");
  VFSPath parent = p.parent();
  EXPECT_EQ(parent.protocol, VFSProtocol::WebDAV);
  EXPECT_EQ(parent.endpoint, "example.com:8080");
  EXPECT_EQ(parent.path, "/project");
}

TEST_F(VFSTest, ParentRoot)
{
  VFSPath p = *VFSPath::parse("file:///");
  VFSPath parent = p.parent();
  EXPECT_TRUE(parent.is_root());
  EXPECT_EQ(parent.path, "/");
}

TEST_F(VFSTest, JoinLocal)
{
  VFSPath p = *VFSPath::parse("file:///home/user");
  VFSPath child = p.join("docs");
  EXPECT_EQ(child.protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(child.path, "/home/user/docs");
}

TEST_F(VFSTest, JoinWebDAV)
{
  VFSPath p = *VFSPath::parse("webdav://host:9090/share");
  VFSPath child = p.join("subfolder");
  EXPECT_EQ(child.protocol, VFSProtocol::WebDAV);
  EXPECT_EQ(child.endpoint, "host:9090");
  EXPECT_EQ(child.path, "/share/subfolder");
}

TEST_F(VFSTest, RoundTripWebDAVSubdir)
{
  auto p = VFSPath::parse("webdav://server/path/to/dir/");
  ASSERT_TRUE(p.has_value());
  std::string s = p->to_string();
  EXPECT_EQ(s, "webdav://server/path/to/dir/");
  auto p2 = VFSPath::parse(s.c_str());
  ASSERT_TRUE(p2.has_value());
  EXPECT_EQ(p2->protocol, VFSProtocol::WebDAV);
  EXPECT_EQ(p2->endpoint, "server");
  EXPECT_EQ(p2->path, "/path/to/dir/");
}

TEST_F(VFSTest, RoundTripLocalWithSubdir)
{
  auto p = VFSPath::parse("/Users/me/work/blender");
  ASSERT_TRUE(p.has_value());
  std::string s = p->to_string();
  EXPECT_EQ(s, "file:///Users/me/work/blender");
  auto p2 = VFSPath::parse(s.c_str());
  ASSERT_TRUE(p2.has_value());
  EXPECT_EQ(p2->protocol, VFSProtocol::FileSystem);
  EXPECT_EQ(p2->path, "/Users/me/work/blender");
}

TEST_F(VFSTest, NormalizePreservesSimplePath)
{
  VFSPath p = *VFSPath::parse("file:///home/user");
  p.normalize();
  EXPECT_EQ(p.path, "/home/user");
}

TEST_F(VFSTest, NormalizeResolvesDots)
{
  VFSPath p = *VFSPath::parse("file:///home/./user/../docs");
  p.normalize();
  EXPECT_EQ(p.path, "/home/docs");
}

TEST_F(VFSTest, NormalizeWebDAV)
{
  VFSPath p = *VFSPath::parse("webdav://host/share/subdir/././more/../docs");
  p.normalize();
  EXPECT_EQ(p.path, "/share/subdir/docs");
}

TEST_F(VFSTest, NormalizeStripsTrailingSlash)
{
  VFSPath p = *VFSPath::parse("file:///home/user/");
  p.normalize();
  EXPECT_EQ(p.path, "/home/user");
}

TEST_F(VFSTest, RoundTripPreservesProtocol)
{
  const char *urls[] = {
      "webdav://127.0.0.1:8080/rs-vulkan/",
      "webdav://server/data/productions/",
      "file:///Users/jeroen/workspace/",
      "/home/user/docs/",
  };
  for (const char *url : urls) {
    auto p = VFSPath::parse(url);
    ASSERT_TRUE(p.has_value()) << "Failed to parse: " << url;
    std::string s = p->to_string();
    auto p2 = VFSPath::parse(s.c_str());
    ASSERT_TRUE(p2.has_value()) << "Failed to re-parse: " << s;
    EXPECT_EQ(p2->protocol, p->protocol) << "Protocol mismatch for: " << url;
    EXPECT_EQ(p2->endpoint, p->endpoint) << "Endpoint mismatch for: " << url;
    EXPECT_EQ(p2->path, p->path) << "Path mismatch for: " << url;
  }
}

}  // namespace blender::bke::tests
