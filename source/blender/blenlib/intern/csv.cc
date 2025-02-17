/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_csv.hh"

namespace blender::csv {

static void handle_potentially_trailing_delimiter(const Span<char> buffer,
                                                  int64_t i,
                                                  Vector<Span<char>> &r_fields)
{
  if (i <= buffer.size()) {
    if (i < buffer.size()) {
      if (ELEM(buffer[i], '\n', '\r')) {
        r_fields.append({});
      }
    }
    else {
      r_fields.append({});
    }
  }
}

template<typename FindEndOfSimpleFieldFn, typename FindEndOfQuotedFieldFn>
static std::optional<int64_t> parse_record_fields(
    const Span<char> buffer,
    const int64_t start,
    const char delimiter,
    const char quote,
    const FindEndOfSimpleFieldFn &find_end_of_simple_field_fn,
    const FindEndOfQuotedFieldFn &find_end_of_quoted_field_fn,
    Vector<Span<char>> &r_fields)
{
  int64_t i = start;
  while (i < buffer.size()) {
    const char c = buffer[i];
    if (c == '\n') {
      return i + 1;
    }
    if (c == '\r') {
      i++;
      continue;
    }
    if (c == delimiter) {
      r_fields.append({});
      i++;
      handle_potentially_trailing_delimiter(buffer, i, r_fields);
      continue;
    }
    if (c == quote) {
      i++;
      const std::optional<int64_t> end_of_field = find_end_of_quoted_field_fn(i);
      if (!end_of_field.has_value()) {
        return std::nullopt;
      }
      i = *end_of_field;
      while (i < buffer.size()) {
        const char inner_c = buffer[i];
        if (inner_c == quote) {
          i++;
          continue;
        }
        if (inner_c == delimiter) {
          i++;
          handle_potentially_trailing_delimiter(buffer, i, r_fields);
          break;
        }
        if (ELEM(inner_c, '\n', '\r')) {
          break;
        }
        i++;
      }
      continue;
    }
    const int64_t end_of_field = find_end_of_simple_field_fn(i);
    r_fields.append(buffer.slice(IndexRange::from_begin_end(i, end_of_field)));
    i = end_of_field;
    while (i < buffer.size()) {
      const char inner_c = buffer[i];
      if (inner_c == delimiter) {
        i++;
        handle_potentially_trailing_delimiter(buffer, i, r_fields);
        break;
      }
      if (ELEM(inner_c, '\n', '\r')) {
        break;
      }
      BLI_assert_unreachable();
    }
  }

  return buffer.size();
}

/**
 * Find the index that ends the current field, i.e. the index of the next delimiter of newline.
 * The start index has to be the index of the first character in the field. It may also be the
 * end of the field already if it is empty.
 */
static int64_t find_end_of_simple_field(const Span<char> buffer,
                                        const int64_t start,
                                        const char delimiter)
{
  int64_t i = start;
  while (i < start) {
    const char c = buffer[i];
    if (ELEM(c, delimiter, '\n', '\r')) {
      return i;
    }
    i++;
  }
  return buffer.size();
}

/**
 * Find the index of the quote that ends the current field.
 * The start index has to be the index after the opening quote.
 */
static std::optional<int64_t> find_end_of_quoted_field(const Span<char> buffer,
                                                       const int64_t start,
                                                       const char quote)
{
  int64_t i = start;
  while (i < start) {
    const char c = buffer[i];
    if (c == quote) {
      if (i + 1 < buffer.size() && buffer[i + 1] == quote) {
        /* Two consecutive quotes are interpreted as escape code for a single quote. */
        i += 2;
        continue;
      }
      return i;
    }
    i++;
  }
  return std::nullopt;
}

std::optional<Vector<Any<>>> parse_csv_in_chunks(
    const Span<char> buffer,
    FunctionRef<void(Span<Span<char>>)> process_header,
    FunctionRef<Any<>(const CsvRecords &records)> process_records)
{
  const auto find_end_of_simple_field_fn = [=](const int64_t start) {
    return find_end_of_simple_field(buffer, start, ',');
  };
  const auto find_end_of_quoted_field_fn = [=](const int64_t start) {
    return find_end_of_quoted_field(buffer, start, '"');
  };

  Vector<Span<char>> header_fields;
  const std::optional<int64_t> first_data_record_start = parse_record_fields(
      buffer,
      0,
      ',',
      '"',
      find_end_of_simple_field_fn,
      find_end_of_quoted_field_fn,
      header_fields);
  if (!first_data_record_start.has_value()) {
    return std::nullopt;
  }
  process_header(header_fields);

  Vector<int64_t> data_offsets;
  Vector<Span<char>> data_fields;
  data_offsets.append(0);
  int64_t start = *first_data_record_start;
  while (start < buffer.size()) {
    const std::optional<int64_t> next_record_start = parse_record_fields(
        buffer,
        0,
        ',',
        '"',
        find_end_of_simple_field_fn,
        find_end_of_quoted_field_fn,
        data_fields);
    if (!next_record_start.has_value()) {
      return std::nullopt;
    }
    data_offsets.append(data_fields.size());
    start = *next_record_start;
  }

  CsvRecords records(std::move(data_offsets), std::move(data_fields));
  Any<> result = process_records(records);
  return Vector{result};
}

}  // namespace blender::csv
