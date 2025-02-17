/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_csv_parse.hh"
#include "BLI_string_ref.hh"

namespace blender::csv_parse::tests {

static std::optional<int64_t> find_end_of_simple_field(const StringRef buffer,
                                                       const int64_t start,
                                                       const char delimiter = ',')
{
  return detail::find_end_of_simple_field(Span<char>(buffer), start, delimiter);
}

static std::optional<int64_t> find_end_of_quoted_field(const StringRef buffer,
                                                       const int64_t start,
                                                       const char quote = '"',
                                                       const Span<char> escape_chars = Span<char>{
                                                           '"', '\\'})
{
  return detail::find_end_of_quoted_field(Span<char>(buffer), start, quote, escape_chars);
}

TEST(csv_parse, FindEndOfSimpleField)
{
  EXPECT_EQ(find_end_of_simple_field("123", 0), 3);
  EXPECT_EQ(find_end_of_simple_field("123", 1), 3);
  EXPECT_EQ(find_end_of_simple_field("123", 2), 3);
  EXPECT_EQ(find_end_of_simple_field("123", 3), 3);
  EXPECT_EQ(find_end_of_simple_field("1'3", 3), 3);
  EXPECT_EQ(find_end_of_simple_field("123,", 0), 3);
  EXPECT_EQ(find_end_of_simple_field("123,456", 0), 3);
  EXPECT_EQ(find_end_of_simple_field("123,456,789", 0), 3);
  EXPECT_EQ(find_end_of_simple_field(" 23", 0), 3);
  EXPECT_EQ(find_end_of_simple_field("", 0), 0);
  EXPECT_EQ(find_end_of_simple_field("\n", 0), 0);
  EXPECT_EQ(find_end_of_simple_field("12\n", 0), 2);
  EXPECT_EQ(find_end_of_simple_field("0,12\n", 0), 1);
  EXPECT_EQ(find_end_of_simple_field("0,12\n", 2), 4);
  EXPECT_EQ(find_end_of_simple_field("\r\n", 0), 0);
  EXPECT_EQ(find_end_of_simple_field("12\r\n", 0), 2);
  EXPECT_EQ(find_end_of_simple_field("0,12\r\n", 0), 1);
  EXPECT_EQ(find_end_of_simple_field("0,12\r\n", 2), 4);
  EXPECT_EQ(find_end_of_simple_field("0,\t12\r\n", 2), 5);
  EXPECT_EQ(find_end_of_simple_field("0,\t12\r\n", 2, '\t'), 2);
}

TEST(csv_parse, FindEndOfQuotedField)
{
  EXPECT_EQ(find_end_of_quoted_field("", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123\n", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123\r\n", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123\"", 0), 3);
  EXPECT_EQ(find_end_of_quoted_field("\"", 0), 0);
  EXPECT_EQ(find_end_of_quoted_field("\"\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("\"\"\"", 0), 2);
  EXPECT_EQ(find_end_of_quoted_field("123\"\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123\"\"\"", 0), 5);
  EXPECT_EQ(find_end_of_quoted_field("123\"\"\"\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("123\"\"\"\"\"", 0), 7);
  EXPECT_EQ(find_end_of_quoted_field("123\"\"0\"\"\"", 0), 8);
  EXPECT_EQ(find_end_of_quoted_field(",", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field(",\"", 0), 1);
  EXPECT_EQ(find_end_of_quoted_field("0,1\"", 0), 3);
  EXPECT_EQ(find_end_of_quoted_field("0,1\n", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("0,1\"\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("0,1\"\"\"", 0), 5);
  EXPECT_EQ(find_end_of_quoted_field("0\n1\n\"", 0), 4);
  EXPECT_EQ(find_end_of_quoted_field("\n\"", 0), 1);
  EXPECT_EQ(find_end_of_quoted_field("\\\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("\\\"\"", 0), 2);
  EXPECT_EQ(find_end_of_quoted_field("\\\"\"\"", 0), std::nullopt);
  EXPECT_EQ(find_end_of_quoted_field("\\\"\"\"\"", 0), 4);
}

TEST(csv_parse, HandlePotentiallyTrailingDelimiter) {}

}  // namespace blender::csv_parse::tests
