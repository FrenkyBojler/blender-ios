/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_any.hh"
#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

namespace blender::csv {

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

}  // namespace blender::csv
