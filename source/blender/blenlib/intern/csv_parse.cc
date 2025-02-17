/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_csv_parse.hh"
#include "BLI_task.hh"

namespace blender::csv_parse {

/**
 * Returns a guess for the start of the next record. Note that this could split up quoted fields.
 * This case needs to be detected at a higher level.
 */
static int64_t guess_next_record_start(const Span<char> buffer, const int64_t start)
{
  int64_t i = start;
  while (i < buffer.size()) {
    const char c = buffer[i];
    if (c == '\n') {
      return i + 1;
    }
    i++;
  }
  return buffer.size();
}

static Vector<Span<char>> split_to_chunks(const Span<char> buffer, int64_t approximate_chunk_size)
{
  approximate_chunk_size = std::max<int64_t>(approximate_chunk_size, 1);
  Vector<Span<char>> chunks;
  int64_t start = 0;
  while (start < buffer.size()) {
    int64_t end = std::min(start + approximate_chunk_size, buffer.size());
    end = guess_next_record_start(buffer, end);
    chunks.append(buffer.slice(IndexRange::from_begin_end(start, end)));
    start = end;
  }
  return chunks;
}

std::optional<Vector<Any<>>> parse_csv_in_chunks(
    const Span<char> buffer,
    const CsvParseOptions &options,
    FunctionRef<void(Span<Span<char>>)> process_header,
    FunctionRef<Any<>(const CsvRecords &records)> process_records)
{
  using namespace detail;

  Vector<Span<char>> header_fields;
  const std::optional<int64_t> first_data_record_start = parse_record_fields(
      buffer, 0, options.delimiter, options.quote, options.quote_escape_chars, header_fields);
  if (!first_data_record_start.has_value()) {
    return std::nullopt;
  }
  process_header(header_fields);

  const Span<char> data_buffer = buffer.drop_front(*first_data_record_start);
  const Vector<Span<char>> data_buffer_chunks = split_to_chunks(data_buffer, 1);
  Vector<std::optional<Any<>>> chunk_results(data_buffer_chunks.size());
  threading::parallel_for(chunk_results.index_range(), 1, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const Span<char> chunk_buffer = data_buffer_chunks[i];
      Vector<int64_t> data_offsets;
      Vector<Span<char>> data_fields;
      data_offsets.append(0);
      int64_t start = 0;
      while (start < chunk_buffer.size()) {
        const std::optional<int64_t> next_record_start = parse_record_fields(
            chunk_buffer,
            start,
            options.delimiter,
            options.quote,
            options.quote_escape_chars,
            data_fields);
        if (!next_record_start.has_value()) {
          return;
        }
        data_offsets.append(data_fields.size());
        start = *next_record_start;
      }
      CsvRecords records(std::move(data_offsets), std::move(data_fields));
      chunk_results[i] = process_records(records);
    }
  });

  Vector<Any<>> results;
  for (const std::optional<Any<>> &result : chunk_results) {
    if (result.has_value()) {
      results.append(result.value());
    }
    else {
      return std::nullopt;
    }
  }

  return results;
}

namespace detail {

std::optional<int64_t> parse_record_fields(const Span<char> buffer,
                                           const int64_t start,
                                           const char delimiter,
                                           const char quote,
                                           const Span<char> quote_escape_chars,
                                           Vector<Span<char>> &r_fields)
{
  using namespace detail;

  const auto handle_potentially_trailing_delimiter = [&](const int64_t i) {
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
  };

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
      handle_potentially_trailing_delimiter(i);
      continue;
    }
    if (c == quote) {
      i++;
      const std::optional<int64_t> end_of_field = find_end_of_quoted_field(
          buffer, i, quote, quote_escape_chars);
      if (!end_of_field.has_value()) {
        return std::nullopt;
      }
      r_fields.append(buffer.slice(IndexRange::from_begin_end(i, *end_of_field)));
      i = *end_of_field;
      while (i < buffer.size()) {
        const char inner_c = buffer[i];
        if (inner_c == quote) {
          i++;
          continue;
        }
        if (inner_c == delimiter) {
          i++;
          handle_potentially_trailing_delimiter(i);
          break;
        }
        if (ELEM(inner_c, '\n', '\r')) {
          break;
        }
        i++;
      }
      continue;
    }
    const int64_t end_of_field = find_end_of_simple_field(buffer, i, delimiter);
    r_fields.append(buffer.slice(IndexRange::from_begin_end(i, end_of_field)));
    i = end_of_field;
    while (i < buffer.size()) {
      const char inner_c = buffer[i];
      if (inner_c == delimiter) {
        i++;
        handle_potentially_trailing_delimiter(i);
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

int64_t find_end_of_simple_field(const Span<char> buffer,
                                 const int64_t start,
                                 const char delimiter)
{
  int64_t i = start;
  while (i < buffer.size()) {
    const char c = buffer[i];
    if (ELEM(c, delimiter, '\n', '\r')) {
      return i;
    }
    i++;
  }
  return buffer.size();
}

std::optional<int64_t> find_end_of_quoted_field(const Span<char> buffer,
                                                const int64_t start,
                                                const char quote,
                                                const Span<char> escape_chars)
{
  int64_t i = start;
  while (i < buffer.size()) {
    const char c = buffer[i];
    if (escape_chars.contains(c)) {
      if (i + 1 < buffer.size() && buffer[i + 1] == quote) {
        i += 2;
        continue;
      }
    }
    if (c == quote) {
      return i;
    }
    i++;
  }
  return std::nullopt;
}

}  // namespace detail

}  // namespace blender::csv_parse
