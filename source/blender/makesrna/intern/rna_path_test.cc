/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "RNA_path.hh"

#include "testing/testing.h"

namespace blender::tests {

static void expect_member_item(const RNAPathParsed::Item &item, const StringRef expected)
{
  EXPECT_TRUE(std::holds_alternative<RNAPathParsed::Member>(item));
  const auto &member = std::get<RNAPathParsed::Member>(item);
  EXPECT_EQ(member.identifier.ref(), expected);
}

static void expect_lookup_index_item(const RNAPathParsed::Item &item, const int64_t expected)
{
  EXPECT_TRUE(std::holds_alternative<RNAPathParsed::LookupIndex>(item));
  const auto &lookup_index = std::get<RNAPathParsed::LookupIndex>(item);
  EXPECT_EQ(lookup_index.index, expected);
}

static void expect_lookup_key_item(const RNAPathParsed::Item &item, const StringRef expected)
{
  EXPECT_TRUE(std::holds_alternative<RNAPathParsed::LookupKey>(item));
  const auto &lookup_key = std::get<RNAPathParsed::LookupKey>(item);
  EXPECT_EQ(lookup_key.key.ref(), expected);
}

TEST(parse_rna_path, empty)
{
  const std::optional<RNAPathParsed> parsed = RNAPathParsed::from_string("");
  EXPECT_FALSE(parsed.has_value());
}

TEST(parse_rna_path, just_member)
{
  const std::optional<RNAPathParsed> parsed = RNAPathParsed::from_string("foo");
  EXPECT_EQ(parsed->items.size(), 1);
  expect_member_item(parsed->items[0], "foo");
}

TEST(parse_rna_path, just_index)
{
  const std::optional<RNAPathParsed> parsed = RNAPathParsed::from_string("[42]");
  EXPECT_EQ(parsed->items.size(), 1);
  expect_lookup_index_item(parsed->items[0], 42);
}

TEST(parse_rna_path, just_key)
{
  const std::optional<RNAPathParsed> parsed = RNAPathParsed::from_string("[\"foo\"]");
  EXPECT_EQ(parsed->items.size(), 1);
  expect_lookup_key_item(parsed->items[0], "foo");
}

TEST(parse_rna_path, multi)
{
  const std::optional<RNAPathParsed> parsed = RNAPathParsed::from_string(
      "foo[42].bar[\"b\\\"az\"]");
  EXPECT_EQ(parsed->items.size(), 4);
  expect_member_item(parsed->items[0], "foo");
  expect_lookup_index_item(parsed->items[1], 42);
  expect_member_item(parsed->items[2], "bar");
  expect_lookup_key_item(parsed->items[3], "b\"az");
}

}  // namespace blender::tests
