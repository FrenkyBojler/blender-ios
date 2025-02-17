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

TEST(csv_parse, ParseCsvInChunks)
{
  struct Chunk {
    Vector<Vector<std::string>> fields;
  };

  const std::string buffer = "a,b,c\n1,2,3,4\n4\n77,88,99\n";

  CsvParseOptions options;
  options.chunk_size_bytes = 1;

  Vector<std::string> column_names;
  const std::optional<Vector<Chunk>> result_opt = parse_csv_in_chunks<Chunk>(
      Span<char>(buffer.data(), buffer.size()),
      options,
      [&](const Span<Span<char>> headers) {
        for (const Span<char> header : headers) {
          column_names.append(std::string(header.begin(), header.end()));
        }
      },
      [&](const CsvRecords &records) {
        Chunk result;
        for (const int64_t record_i : records.index_range()) {
          const CsvRecord record = records.record(record_i);
          Vector<std::string> fields;
          for (const int64_t column_i : column_names.index_range()) {
            const Span<char> value = record.field(column_i);
            fields.append(std::string(value.begin(), value.end()));
          }
          result.fields.append(std::move(fields));
        }
        return result;
      });
  EXPECT_TRUE(result_opt.has_value());
  Vector<Vector<std::string>> combined;
  for (const Chunk &chunk : *result_opt) {
    combined.extend(std::move(chunk.fields));
  }

  EXPECT_EQ(column_names.size(), 3);
  EXPECT_EQ(column_names[0], "a");
  EXPECT_EQ(column_names[1], "b");
  EXPECT_EQ(column_names[2], "c");

  EXPECT_EQ(combined.size(), 3);
  EXPECT_EQ(combined[0].size(), 3);
  EXPECT_EQ(combined[1].size(), 3);
  EXPECT_EQ(combined[2].size(), 3);

  EXPECT_EQ(combined[0][0], "1");
  EXPECT_EQ(combined[0][1], "2");
  EXPECT_EQ(combined[0][2], "3");

  EXPECT_EQ(combined[1][0], "4");
  EXPECT_EQ(combined[1][1], "");
  EXPECT_EQ(combined[1][2], "");

  EXPECT_EQ(combined[2][0], "77");
  EXPECT_EQ(combined[2][1], "88");
  EXPECT_EQ(combined[2][2], "99");
}

}  // namespace blender::csv_parse::tests
