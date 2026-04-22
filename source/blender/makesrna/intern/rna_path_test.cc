/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "RNA_path.hh"

#include "testing/testing.h"

namespace blender::tests {

static void expect_member_item(const rna_path::Item &item, const StringRef expected)
{
  EXPECT_TRUE(std::holds_alternative<rna_path::Member>(item));
  const auto &member = std::get<rna_path::Member>(item);
  EXPECT_EQ(member.identifier.ref(), expected);
}

static void expect_lookup_index_item(const rna_path::Item &item, const int64_t expected)
{
  EXPECT_TRUE(std::holds_alternative<rna_path::LookupIndex>(item));
  const auto &lookup_index = std::get<rna_path::LookupIndex>(item);
  EXPECT_EQ(lookup_index.index, expected);
}

static void expect_lookup_key_item(const rna_path::Item &item, const StringRef expected)
{
  EXPECT_TRUE(std::holds_alternative<rna_path::LookupKey>(item));
  const auto &lookup_key = std::get<rna_path::LookupKey>(item);
  EXPECT_EQ(lookup_key.key.ref(), expected);
}

TEST(parse_rna_path, empty)
{
  const std::optional<ParsedRNAPath<>> path = ParsedRNAPath<>::from_string("");
  EXPECT_FALSE(path.has_value());
}

TEST(parse_rna_path, just_member)
{
  const std::optional<ParsedRNAPath<>> path = ParsedRNAPath<>::from_string("foo");
  EXPECT_EQ(path->to_string(), "foo");
  EXPECT_EQ(path->items.size(), 1);
  expect_member_item(path->items[0], "foo");
}

TEST(parse_rna_path, just_index)
{
  const std::optional<ParsedRNAPath<>> path = ParsedRNAPath<>::from_string("[42]");
  EXPECT_EQ(path->to_string(), "[42]");
  EXPECT_EQ(path->items.size(), 1);
  expect_lookup_index_item(path->items[0], 42);
}

TEST(parse_rna_path, just_key)
{
  const std::optional<ParsedRNAPath<>> path = ParsedRNAPath<>::from_string("[\"foo\"]");
  EXPECT_EQ(path->to_string(), "[\"foo\"]");
  EXPECT_EQ(path->items.size(), 1);
  expect_lookup_key_item(path->items[0], "foo");
}

TEST(parse_rna_path, multi)
{
  const std::optional<ParsedRNAPath<>> path = ParsedRNAPath<>::from_string(
      "foo[42].bar.bar2[\"b\\\"az\"]");
  EXPECT_EQ(path->to_string(), "foo[42].bar.bar2[\"b\\\"az\"]");
  EXPECT_EQ(path->items.size(), 5);
  expect_member_item(path->items[0], "foo");
  expect_lookup_index_item(path->items[1], 42);
  expect_member_item(path->items[2], "bar");
  expect_member_item(path->items[3], "bar2");
  expect_lookup_key_item(path->items[4], "b\"az");
}

}  // namespace blender::tests
