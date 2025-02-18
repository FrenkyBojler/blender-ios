/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_any.hh"
#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::csv_parse {

class CsvRecord {
 private:
  Span<Span<char>> fields_;

 public:
  CsvRecord(Span<Span<char>> fields);

  int64_t size() const;
  IndexRange index_range() const;

  Span<char> field(const int64_t index) const;
  StringRef field_str(const int64_t index) const;
};

class CsvRecords {
 private:
  Span<int64_t> offsets_;
  Span<Span<char>> fields_;

 public:
  CsvRecords(Span<int64_t> offsets, Span<Span<char>> fields);

  int64_t size() const;
  IndexRange index_range() const;
  OffsetIndices<int64_t> offsets() const;

  CsvRecord record(const int64_t index) const;
};

struct CsvParseOptions {
  char delimiter = ',';
  char quote = '"';
  Span<char> quote_escape_chars = Span<char>{'"', '\\'};
  int64_t chunk_size_bytes = 32 * 1024;
};

std::optional<Vector<Any<>>> parse_csv_in_chunks(
    const Span<char> buffer,
    const CsvParseOptions &options,
    FunctionRef<void(const CsvRecord &record)> process_header,
    FunctionRef<Any<>(const CsvRecords &records)> process_records);

template<typename ChunkT>
inline std::optional<Vector<ChunkT>> parse_csv_in_chunks(
    const Span<char> buffer,
    const CsvParseOptions &options,
    FunctionRef<void(const CsvRecord &record)> process_header,
    FunctionRef<ChunkT(const CsvRecords &records)> process_records)
{
  std::optional<Vector<Any<>>> result = parse_csv_in_chunks(
      buffer, options, process_header, [&](const CsvRecords &records) {
        return Any<>(process_records(records));
      });
  if (!result.has_value()) {
    return std::nullopt;
  }
  Vector<ChunkT> result_chunks;
  for (Any<> &value : *result) {
    result_chunks.append(std::move(value.get<ChunkT>()));
  }
  return result_chunks;
}

/* -------------------------------------------------------------------- */
/** \name #CsvRecord inline functions.
 * \{ */

inline CsvRecord::CsvRecord(Span<Span<char>> fields) : fields_(fields) {}

inline int64_t CsvRecord::size() const
{
  return fields_.size();
}

inline IndexRange CsvRecord::index_range() const
{
  return fields_.index_range();
}

inline Span<char> CsvRecord::field(const int64_t index) const
{
  BLI_assert(index >= 0);
  if (index >= fields_.size()) {
    return {};
  }
  return fields_[index];
}

inline StringRef CsvRecord::field_str(const int64_t index) const
{
  const Span<char> value = this->field(index);
  return StringRef(value.data(), value.size());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #CsvRecords inline functions.
 * \{ */

inline CsvRecords::CsvRecords(Span<int64_t> offsets, Span<Span<char>> fields)
    : offsets_(offsets), fields_(fields)
{
}

inline OffsetIndices<int64_t> CsvRecords::offsets() const
{
  return OffsetIndices<int64_t>(offsets_, offset_indices::NoSortCheck{});
}

inline int64_t CsvRecords::size() const
{
  return this->offsets().size();
}

inline IndexRange CsvRecords::index_range() const
{
  return this->offsets().index_range();
}

inline CsvRecord CsvRecords::record(const int64_t index) const
{
  OffsetIndices offsets = this->offsets();
  return CsvRecord(fields_.slice(offsets[index]));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal functions exposed for testing.
 * \{ */

namespace detail {

/**
 * Find the index that ends the current field, i.e. the index of the next delimiter of newline.
 * The start index has to be the index of the first character in the field. It may also be the
 * end of the field already if it is empty.
 *
 * \param start: The index of the first character in the field. This may also be the end of the
 *   field already if it is empty.
 * \param delimiter: The character that ends the field.
 * \return Index of the next delimiter, a newline character or the end of the buffer.
 */
int64_t find_end_of_simple_field(Span<char> buffer, int64_t start, char delimiter = ',');

/**
 * Find the index of the quote that ends the current field.
 *
 * \param start: The index after the opening quote.
 * \param quote: The quote character that ends the field.
 * \param escape_chars: The characters that may be used to escape the quote character.
 * \return Index of the quote character that ends the field, or std::nullopt if the field is
 *   malformed and does not have an end.
 */
std::optional<int64_t> find_end_of_quoted_field(Span<char> buffer,
                                                int64_t start,
                                                char quote = '"',
                                                Span<char> escape_chars = Span<char>{'"', '\\'});

/**
 * Finds all fields for the record starting at the given index. Typically, the record ends with a
 * newline, but quoted multiline records are supported as well.
 *
 * \return Index of the the start of the next record or the end of the buffer. Nullopt is returned
 *   if the buffer has a malformed record at the end, i.e. a quoted field that is not closed.
 */
std::optional<int64_t> parse_record_fields(const Span<char> buffer,
                                           const int64_t start,
                                           const char delimiter,
                                           const char quote,
                                           const Span<char> quote_escape_chars,
                                           Vector<Span<char>> &r_fields);

}  // namespace detail

/** \} */

}  // namespace blender::csv_parse
