/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_any.hh"
#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

namespace blender::csv_parse {

class CsvRecord {
 private:
  Span<Span<char>> fields_;

 public:
  CsvRecord(Span<Span<char>> fields) : fields_(fields) {}

  Span<char> field(const int64_t index) const
  {
    BLI_assert(index >= 0);
    if (index >= fields_.size()) {
      return {};
    }
    return fields_[index];
  }
};

class CsvRecords {
 private:
  Vector<int64_t> offsets_;
  Vector<Span<char>> fields_;

 public:
  CsvRecords(Vector<int64_t> offsets, Vector<Span<char>> fields)
      : offsets_(offsets), fields_(fields)
  {
  }

  OffsetIndices<int64_t> offsets() const
  {
    return OffsetIndices<int64_t>(offsets_, offset_indices::NoSortCheck{});
  }

  int64_t size() const
  {
    return this->offsets().size();
  }

  IndexRange index_range() const
  {
    return this->offsets().index_range();
  }

  CsvRecord record(const int64_t index) const
  {
    OffsetIndices offsets = this->offsets();
    return CsvRecord(fields_.as_span().slice(offsets[index]));
  }
};

std::optional<Vector<Any<>>> parse_csv_in_chunks(
    const Span<char> buffer,
    FunctionRef<void(Span<Span<char>>)> process_header,
    FunctionRef<Any<>(const CsvRecords &records)> process_records);

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

}  // namespace detail

/** \} */

}  // namespace blender::csv_parse
