/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bit_bool_conversion.hh"
#include "BLI_bit_iterator.hh"
#include "BLI_bit_vector.hh"
#include "BLI_csv_parse.hh"
#include "BLI_enumerable_thread_specific.hh"
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

/**
 * Split the buffer into chunks of approximately the given size. The function attempts to align the
 * chunks so that records are not split. This works in the majority of cases, but can fail with
 * multi-line fields. This has to be detected at a higher level.
 */
static Vector<Span<char>> split_into_aligned_chunks(const Span<char> buffer,
                                                    int64_t approximate_chunk_size)
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

/**
 * \param start: Index of the first character of the record (e.g. 0 for the start of the file, or
 *   the index after a newline character).
 * \param set_bits_it: Iterator the points to the next special character in that record (or end if
 *   there is none). After the function, it points to the newline character of end.
 */
[[nodiscard]] static bool parse_record_fields2(const Span<char> buffer,
                                               const int64_t start,
                                               bits::SetBitIterator &set_bits_it,
                                               const bits::SetBitIterator &set_bits_end,
                                               const char delimiter,
                                               const char quote,
                                               const Span<char> quote_escape_chars,
                                               Vector<Span<char>> &r_fields)
{
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
    /* Invariants that should always be true at the beginning of this loop:
     * - `i` points either to:
     *   - the start of the next field
     *   - a quote character that starts the next field
     *   - a delimiter that ends the next field in case that field is empty
     *   - a \r or \n character
     * - `set_bits_it` points to the next special character starting at and including `i`.
     */

    const char c = buffer[i];
    if (c == '\n') {
      BLI_assert(i == *set_bits_it);
      return true;
    }
    if (c == '\r') {
      BLI_assert(i == *set_bits_it);
      /* Ignore this character.*/
      i++;
      continue;
    }
    if (c == delimiter) {
      BLI_assert(i == *set_bits_it);
      r_fields.append({});
      i++;
      handle_potentially_trailing_delimiter(i);
      continue;
    }
    if (c == quote) {
      BLI_assert(i == *set_bits_it);
      const int64_t field_start = i + 1;
      while (true) {
        ++set_bits_it;
        if (set_bits_it == set_bits_end) {
          /* Missing closing quote. */
          return false;
        }
        i = *set_bits_it;
        const char inner_c = buffer[i];
        if (quote_escape_chars.contains(inner_c)) {
          if (i + 1 < buffer.size() && buffer[i + 1] == quote) {
            /* Ignore this escape character. */
            ++set_bits_it;
            /* Ignore the next quote character. */
            BLI_assert(buffer[*set_bits_it] == '"');
            ++set_bits_it;
            continue;
          }
        }
        if (inner_c == quote) {
          /* Found the closing quote. */
          r_fields.append(buffer.slice(IndexRange::from_begin_end(field_start, i)));
          ++set_bits_it;
          break;
        }
      }
      /* Go to start of next field or end of record. */
      while (true) {
        if (set_bits_it == set_bits_end) {
          return true;
        }
        i = *set_bits_it;
        const char inner_c = buffer[i];
        if (inner_c == delimiter) {
          ++set_bits_it;
          i++;
          handle_potentially_trailing_delimiter(i);
          break;
        }
        if (ELEM(inner_c, '\n', '\r')) {
          break;
        }
        ++set_bits_it;
      }
      continue;
    }
    const int64_t field_start = i;
    while (true) {
      if (set_bits_it == set_bits_end) {
        r_fields.append(buffer.slice(IndexRange::from_begin_end(field_start, buffer.size())));
        return true;
      }
      i = *set_bits_it;
      const char inner_c = buffer[i];
      if (inner_c == delimiter) {
        r_fields.append(buffer.slice(IndexRange::from_begin_end(field_start, i)));
        ++set_bits_it;
        i++;
        handle_potentially_trailing_delimiter(i);
        break;
      }
      if (ELEM(inner_c, '\n', '\r')) {
        r_fields.append(buffer.slice(IndexRange::from_begin_end(field_start, i)));
        break;
      }
    }
  }
  return false;
}

struct LocalParseMemoryCache {
  Vector<int64_t> data_offsets;
  Vector<Span<char>> data_fields;
  BitVector<> special_char_bits;
};

static std::optional<CsvRecords> parse_records2(const Span<char> buffer,
                                                const CsvParseOptions &options,
                                                LocalParseMemoryCache &memory_cache)
{
  Vector<char, 32> special_chars;
  special_chars.append_non_duplicates(options.quote);
  special_chars.append_non_duplicates(options.delimiter);
  special_chars.append_non_duplicates('\r');
  special_chars.append_non_duplicates('\n');
  special_chars.extend_non_duplicates(options.quote_escape_chars);

  memory_cache.special_char_bits.resize(buffer.size());
  memory_cache.special_char_bits.fill(false);
  bits::bytes_to_bits(buffer, special_chars, memory_cache.special_char_bits);

  /* Clear the data that may still be in there, but do not free the memory. */
  memory_cache.data_offsets.clear();
  memory_cache.data_fields.clear();

  memory_cache.data_offsets.append(0);

  bits::SetBitIterable set_bits(memory_cache.special_char_bits);
  bits::SetBitIterator set_bits_it = set_bits.begin();
  bits::SetBitIterator set_bits_end = set_bits.end();

  int64_t next_record_start = 0;
  while (next_record_start < buffer.size()) {
    const bool success = parse_record_fields2(buffer,
                                              next_record_start,
                                              set_bits_it,
                                              set_bits_end,
                                              options.delimiter,
                                              options.quote,
                                              options.quote_escape_chars,
                                              memory_cache.data_fields);
    if (!success) {
      return std::nullopt;
    }
    memory_cache.data_offsets.append(memory_cache.data_fields.size());
    if (set_bits_it == set_bits_end) {
      break;
    }
    const int64_t newline_index = *set_bits_it;
    BLI_assert(buffer[newline_index] == '\n');
    next_record_start = newline_index + 1;
    ++set_bits_it;
  }

  return CsvRecords(OffsetIndices<int64_t>(memory_cache.data_offsets), memory_cache.data_fields);
}

/**
 * Parses the given buffer into records and their fields.
 *
 * r_data_offsets and r_data_fields are passed into to be able to reuse their memory.
 */
static std::optional<CsvRecords> parse_records(const Span<char> buffer,
                                               const CsvParseOptions &options,
                                               LocalParseMemoryCache &memory_cache)
{
  return parse_records2(buffer, options, memory_cache);

  using namespace detail;
  /* Clear the data that may still be in there, but do not free the memory. */
  memory_cache.data_offsets.clear();
  memory_cache.data_fields.clear();

  memory_cache.data_offsets.append(0);
  int64_t start = 0;
  while (start < buffer.size()) {
    const std::optional<int64_t> next_record_start = parse_record_fields(
        buffer,
        start,
        options.delimiter,
        options.quote,
        options.quote_escape_chars,
        memory_cache.data_fields);
    if (!next_record_start.has_value()) {
      return std::nullopt;
    }
    memory_cache.data_offsets.append(memory_cache.data_fields.size());
    start = *next_record_start;
  }
  return CsvRecords(OffsetIndices<int64_t>(memory_cache.data_offsets), memory_cache.data_fields);
}

std::optional<Vector<Any<>>> parse_csv_in_chunks(
    const Span<char> buffer,
    const CsvParseOptions &options,
    FunctionRef<void(const CsvRecord &record)> process_header,
    FunctionRef<Any<>(const CsvRecords &records)> process_records)
{
  using namespace detail;

  /* First parse the first row to get the column names. */
  Vector<Span<char>> header_fields;
  const std::optional<int64_t> first_data_record_start = parse_record_fields(
      buffer, 0, options.delimiter, options.quote, options.quote_escape_chars, header_fields);
  if (!first_data_record_start.has_value()) {
    return std::nullopt;
  }
  /* Call this before starting to process the remaining data. This allows the caller to do some
   * preprocessing that is used during chunk parsing. */
  process_header(CsvRecord(header_fields));

  /* This buffer contains only the data records, without the header. */
  const Span<char> data_buffer = buffer.drop_front(*first_data_record_start);
  /* Split the buffer into chunks that can be processed in parallel. */
  const Vector<Span<char>> data_buffer_chunks = split_into_aligned_chunks(
      data_buffer, options.chunk_size_bytes);

  /* It's not common, but it can happen that .csv files contain quoted multi-line values. In the
   * unlucky case that we split the buffer in the middle of such a multi-line field, there will
   * be malformed chunks. In this case we fallback to parsing the whole buffer with a single
   * thread. If this case becomes more common, we could try to avoid splitting into malformed
   * chunks by making the splitting logic a bit smarter. */
  std::atomic<bool> found_malformed_chunk = false;
  Vector<std::optional<Any<>>> chunk_results(data_buffer_chunks.size());
  struct TLS {
    LocalParseMemoryCache memory_cache;
  };
  threading::EnumerableThreadSpecific<TLS> all_tls;
  threading::parallel_for(chunk_results.index_range(), 1, [&](const IndexRange range) {
    TLS &tls = all_tls.local();
    for (const int64_t i : range) {
      if (found_malformed_chunk.load(std::memory_order_relaxed)) {
        /* All work is cancelled when there was a malformed chunk. */
        return;
      }
      const Span<char> chunk_buffer = data_buffer_chunks[i];
      const std::optional<CsvRecords> records = parse_records(
          chunk_buffer, options, tls.memory_cache);
      if (!records.has_value()) {
        found_malformed_chunk.store(true, std::memory_order_relaxed);
        return;
      }
      chunk_results[i] = process_records(*records);
    }
  });

  /* If there was a malformed chunk, process the data again in a single thread without splitting
   * the input into chunks. This should happen quite rarely but is important for overall
   * correctness. */
  if (found_malformed_chunk) {
    chunk_results.clear();
    TLS &tls = all_tls.local();
    const std::optional<CsvRecords> records = parse_records(
        data_buffer, options, tls.memory_cache);
    if (!records.has_value()) {
      return std::nullopt;
    }
    chunk_results.append(process_records(*records));
  }

  /* Prepare the return value. */
  Vector<Any<>> results;
  for (std::optional<Any<>> &result : chunk_results) {
    BLI_assert(result.has_value());
    results.append(std::move(result.value()));
  }
  return results;
}

StringRef unescape_field(const StringRef str,
                         const CsvParseOptions &options,
                         LinearAllocator<> &allocator)
{
  const StringRef escape_chars{options.quote_escape_chars};
  if (str.find_first_of(escape_chars) == StringRef::not_found) {
    return str;
  }
  /* The actual unescaped string may be shorter, but not longer. */
  MutableSpan<char> unescaped_str = allocator.allocate_array<char>(str.size());
  int64_t i = 0;
  int64_t escaped_size = 0;
  while (i < str.size()) {
    const char c = str[i];
    if (options.quote_escape_chars.contains(c)) {
      if (i + 1 < str.size() && str[i + 1] == options.quote) {
        /* Ignore the current escape character. */
        unescaped_str[escaped_size++] = options.quote;
        i += 2;
        continue;
      }
    }
    unescaped_str[escaped_size++] = c;
    i++;
  }
  return StringRef(unescaped_str.take_front(escaped_size));
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
