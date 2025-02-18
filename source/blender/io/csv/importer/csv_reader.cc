/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup csv
 */

#include <charconv>
#include <optional>
#include <variant>

#include "BKE_anonymous_attribute_id.hh"
#include "fast_float.h"

#include "BKE_attribute.hh"
#include "BKE_pointcloud.hh"
#include "BKE_report.hh"

#include "BLI_csv_parse.hh"
#include "BLI_fileops.hh"
#include "BLI_vector.hh"

#include "IO_csv.hh"

namespace blender::io::csv {

using ColumnData = std::variant<std::monostate, Vector<float>, Vector<int>>;

struct ChunkResult {
  int rows_num;
  Vector<ColumnData> columns;
};

struct ColumnTypeInfo {
  std::atomic<bool> found_invalid = false;
  std::atomic<bool> found_int = false;
  std::atomic<bool> found_float = false;
};

struct ColumnsInfo {
  Array<StringRef> names;
  Array<ColumnTypeInfo> types;
};

struct ParseFloatColumnResult {
  Vector<float> data;
  bool found_invalid = false;
};

struct ParseIntColumnResult {
  Vector<int> data;
  bool found_invalid = false;
  bool found_float = false;
};

static ParseFloatColumnResult parse_column_as_floats(const csv_parse::CsvRecords &records,
                                                     const int column_i)
{
  ParseFloatColumnResult result;
  result.data.reserve(records.size());
  for (const int row_i : records.index_range()) {
    const Span<char> value_span = records.record(row_i).field(column_i);
    const char *value_begin = value_span.begin();
    const char *value_end = value_span.end();
    /* Skip leading whitespace and plus sign. */
    while (value_begin < value_end && ELEM(*value_begin, ' ', '+')) {
      value_begin++;
    }
    float value;
    fast_float::from_chars_result res = fast_float::from_chars(value_begin, value_end, value);
    if (res.ec != std::errc()) {
      result.found_invalid = true;
      return result;
    }
    if (res.ptr < value_end) {
      /* Allow trailing whitespace in the value. */
      while (res.ptr < value_end && res.ptr[0] == ' ') {
        res.ptr++;
      }
      if (res.ptr < value_end) {
        result.found_invalid = true;
        return result;
      }
    }
    result.data.append(value);
  }
  return result;
}

static ParseIntColumnResult parse_column_as_ints(const csv_parse::CsvRecords &records,
                                                 const int column_i)
{
  ParseIntColumnResult result;
  result.data.reserve(records.size());
  for (const int row_i : records.index_range()) {
    const Span<char> value_span = records.record(row_i).field(column_i);
    const char *value_begin = value_span.begin();
    const char *value_end = value_span.end();
    /* Skip leading whitespace and plus sign. */
    while (value_begin < value_end && ELEM(*value_begin, ' ', '+')) {
      value_begin++;
    }
    int value;
    std::from_chars_result res = std::from_chars(value_begin, value_end, value);
    if (res.ec != std::errc()) {
      result.found_invalid = true;
      return result;
    }
    if (res.ptr < value_end) {
      /* If the next character after the value is a dot, it should be parsed again as float. */
      if (res.ptr[0] == '.') {
        result.found_float = true;
        return result;
      }
      /* Allow trailing whitespace in the value. */
      while (res.ptr < value_end && res.ptr[0] == ' ') {
        res.ptr++;
      }
      if (res.ptr < value_end) {
        result.found_invalid = true;
        return result;
      }
    }
    result.data.append(value);
  }
  return result;
}

static ChunkResult parse_records_chunk(const csv_parse::CsvRecords &records,
                                       ColumnsInfo &columns_info)
{
  const int columns_num = columns_info.names.size();
  ChunkResult chunk_result;
  chunk_result.rows_num = records.size();
  chunk_result.columns.resize(columns_num);
  for (const int column_i : IndexRange(columns_num)) {
    ColumnTypeInfo &type_info = columns_info.types[column_i];
    if (type_info.found_invalid.load(std::memory_order_relaxed)) {
      /* Invalid values have been found in this column already, skip it. */
      continue;
    }
    /* A float was found in this column already, so parse everything as floats. */
    const bool found_float = type_info.found_float.load(std::memory_order_relaxed);
    if (found_float) {
      ParseFloatColumnResult float_column_result = parse_column_as_floats(records, column_i);
      if (float_column_result.found_invalid) {
        type_info.found_invalid.store(true, std::memory_order_relaxed);
        continue;
      }
      chunk_result.columns[column_i] = std::move(float_column_result.data);
      continue;
    }
    /* No float was found so far in this column, so attempt to parse it as integers. */
    ParseIntColumnResult int_column_result = parse_column_as_ints(records, column_i);
    if (int_column_result.found_invalid) {
      type_info.found_invalid.store(true, std::memory_order_relaxed);
      continue;
    }
    if (!int_column_result.found_float) {
      chunk_result.columns[column_i] = std::move(int_column_result.data);
      type_info.found_int.store(true, std::memory_order_relaxed);
      continue;
    }
    /* While parsing it as integers, floats were detected. So parse it as floats again. */
    type_info.found_float.store(true, std::memory_order_relaxed);
    ParseFloatColumnResult float_column_result = parse_column_as_floats(records, column_i);
    if (float_column_result.found_invalid) {
      type_info.found_invalid.store(true, std::memory_order_relaxed);
      continue;
    }
    chunk_result.columns[column_i] = std::move(float_column_result.data);
  }
  return chunk_result;
}

PointCloud *import_csv_as_point_cloud(const CSVImportParams &import_params)
{
  size_t buffer_len;
  void *buffer = BLI_file_read_text_as_mem(import_params.filepath, 0, &buffer_len);
  if (buffer == nullptr) {
    BKE_reportf(import_params.reports,
                RPT_ERROR,
                "CSV Import: Cannot open file '%s'",
                import_params.filepath);
    return nullptr;
  }

  BLI_SCOPED_DEFER([&]() { MEM_freeN(buffer); });

  if (buffer_len == 0) {
    BKE_reportf(
        import_params.reports, RPT_ERROR, "CSV Import: empty file '%s'", import_params.filepath);
    return nullptr;
  }

  ColumnsInfo columns_info;

  const auto parse_header = [&](const csv_parse::CsvRecord &record) {
    columns_info.names.reinitialize(record.size());
    columns_info.types.reinitialize(record.size());
    for (const int i : record.index_range()) {
      columns_info.names[i] = record.field_str(i);
    }
  };
  const auto parse_data_chunk = [&](const csv_parse::CsvRecords &records) {
    return parse_records_chunk(records, columns_info);
  };

  const Span<char> buffer_span{static_cast<char *>(buffer), int64_t(buffer_len)};
  csv_parse::CsvParseOptions parse_options;
  const std::optional<Vector<ChunkResult>> parsed_chunks =
      csv_parse::parse_csv_in_chunks<ChunkResult>(
          buffer_span, parse_options, parse_header, parse_data_chunk);
  for (StringRef name : columns_info.names) {
    printf("name: %s\n", std::string(name).c_str());
  }
  if (!parsed_chunks.has_value()) {
    BKE_reportf(import_params.reports,
                RPT_ERROR,
                "CSV import: failed to parse file '%s'",
                import_params.filepath);
    return nullptr;
  }

  Vector<int> flatten_offsets_vec;
  flatten_offsets_vec.append(0);
  for (const ChunkResult &chunk : *parsed_chunks) {
    flatten_offsets_vec.append(flatten_offsets_vec.last() + chunk.rows_num);
  }
  const OffsetIndices<int> chunk_offsets(flatten_offsets_vec);
  const int points_num = flatten_offsets_vec.last();

  struct FlattenedAttribute {
    eCustomDataType type;
    void *data;
  };
  Array<std::optional<FlattenedAttribute>> flattened_attributes(columns_info.names.size());
  threading::parallel_for(
      columns_info.names.index_range(), 1, [&](const IndexRange columns_range) {
        for (const int column_i : columns_range) {
          const ColumnTypeInfo &type_info = columns_info.types[column_i];
          if (type_info.found_invalid) {
            /* Can't read data from this column. */
            continue;
          }
          if (type_info.found_float) {
            /* Should read column as floats. */
            float *attribute_buffer = static_cast<float *>(MEM_mallocN_aligned(
                sizeof(float) * points_num, alignof(float), "csv float attribute"));
            flattened_attributes[column_i] = FlattenedAttribute{CD_PROP_FLOAT, attribute_buffer};
            threading::parallel_for(
                parsed_chunks->index_range(), 1, [&](const IndexRange chunks_range) {
                  for (const int chunk_i : chunks_range) {
                    const IndexRange dst_range = chunk_offsets[chunk_i];
                    const ChunkResult &chunk = (*parsed_chunks)[chunk_i];
                    const ColumnData &column_data = chunk.columns[column_i];
                    if (const auto *float_vec = std::get_if<Vector<float>>(&column_data)) {
                      BLI_assert(float_vec->size() == dst_range.size());
                      uninitialized_copy_n(float_vec->data(),
                                           dst_range.size(),
                                           attribute_buffer + dst_range.first());
                    }
                    else if (const auto *int_vec = std::get_if<Vector<int>>(&column_data)) {
                      BLI_assert(int_vec->size() == dst_range.size());
                      uninitialized_convert_n(int_vec->data(), dst_range.size(), attribute_buffer);
                    }
                    else {
                      /* Expected data to be available, because the `found_invalid` flag was not
                       * set. */
                      BLI_assert_unreachable();
                    }
                  }
                });
            continue;
          }
          if (type_info.found_int) {
            /* Should read column as ints. */
            int *attribute_buffer = static_cast<int *>(
                MEM_mallocN_aligned(sizeof(int) * points_num, alignof(int), "csv int attribute"));
            flattened_attributes[column_i] = FlattenedAttribute{CD_PROP_INT32, attribute_buffer};
            threading::parallel_for(
                parsed_chunks->index_range(), 1, [&](const IndexRange chunks_range) {
                  for (const int chunk_i : chunks_range) {
                    const IndexRange dst_range = chunk_offsets[chunk_i];
                    const ChunkResult &chunk = (*parsed_chunks)[chunk_i];
                    const ColumnData &column_data = chunk.columns[column_i];
                    if (const auto *int_vec = std::get_if<Vector<int>>(&column_data)) {
                      BLI_assert(int_vec->size() == dst_range.size());
                      uninitialized_copy_n(
                          int_vec->data(), dst_range.size(), attribute_buffer + dst_range.first());
                    }
                    else {
                      /* Expected data to be available, because the `found_invalid` and
                       * `found_float` flags were not set. */
                      BLI_assert_unreachable();
                    }
                  }
                });
            continue;
          }
        }
      });

  PointCloud *pointcloud = BKE_pointcloud_new_nomain(points_num);
  pointcloud->positions_for_write().fill(float3(0));

  bke::MutableAttributeAccessor attributes = pointcloud->attributes_for_write();

  for (const int column_i : columns_info.names.index_range()) {
    const std::optional<FlattenedAttribute> &attribute = flattened_attributes[column_i];
    if (!attribute.has_value()) {
      continue;
    }
    const StringRef name = columns_info.names[column_i];
    if (!bke::allow_procedural_attribute_access(name)) {
      continue;
    }
    if (bke::attribute_name_is_anonymous(name)) {
      continue;
    }
    attributes.add(name,
                   bke::AttrDomain::Point,
                   attribute->type,
                   bke::AttributeInitMoveArray{attribute->data});
  }

  return pointcloud;
}

}  // namespace blender::io::csv
