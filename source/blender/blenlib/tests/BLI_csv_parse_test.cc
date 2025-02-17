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

static std::optional<Vector<std::string>> parse_record_fields(
    const StringRef buffer,
    const int64_t start = 0,
    const char delimiter = ',',
    const char quote = '"',
    const Span<char> quote_escape_chars = Span<char>{'"', '\\'})
{
  Vector<Span<char>> fields;
  const std::optional<int64_t> end_of_record = detail::parse_record_fields(
      Span<char>(buffer), start, delimiter, quote, quote_escape_chars, fields);
  if (!end_of_record.has_value()) {
    return std::nullopt;
  }
  Vector<std::string> result;
  for (const Span<char> field : fields) {
    result.append(std::string(field.begin(), field.end()));
  }
  return result;
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

TEST(csv_parse, ParseRecordFields)
{
  using StrVec = Vector<std::string>;
  EXPECT_EQ(parse_record_fields(""), StrVec());
  EXPECT_EQ(parse_record_fields("1"), StrVec{"1"});
  EXPECT_EQ(parse_record_fields("1,2"), StrVec({"1", "2"}));
  EXPECT_EQ(parse_record_fields("1,2,3"), StrVec({"1", "2", "3"}));
  EXPECT_EQ(parse_record_fields("1\n,2,3"), StrVec({"1"}));
  EXPECT_EQ(parse_record_fields("1, 2\n,3"), StrVec({"1", " 2"}));
  EXPECT_EQ(parse_record_fields("1, 2\r\n,3"), StrVec({"1", " 2"}));
  EXPECT_EQ(parse_record_fields("\"1,2,3\""), StrVec({"1,2,3"}));
  EXPECT_EQ(parse_record_fields("\"1,2,3"), std::nullopt);
  EXPECT_EQ(parse_record_fields("\"1,\n2\t\r\n,3\""), StrVec({"1,\n2\t\r\n,3"}));
  EXPECT_EQ(parse_record_fields("\"1,2,3\",\"4,5\""), StrVec({"1,2,3", "4,5"}));
  EXPECT_EQ(parse_record_fields(","), StrVec({"", ""}));
  EXPECT_EQ(parse_record_fields(",,"), StrVec({"", "", ""}));
  EXPECT_EQ(parse_record_fields(",,\n"), StrVec({"", "", ""}));
  EXPECT_EQ(parse_record_fields("\r\n,,"), StrVec());
  EXPECT_EQ(parse_record_fields("\"a\"\"b\""), StrVec({"a\"\"b"}));
  EXPECT_EQ(parse_record_fields("\"a\\\"b\""), StrVec({"a\\\"b"}));
  EXPECT_EQ(parse_record_fields("\"a\"\nb"), StrVec({"a"}));
  EXPECT_EQ(parse_record_fields("\"a\"  \nb"), StrVec({"a"}));
}

}  // namespace blender::csv_parse::tests
