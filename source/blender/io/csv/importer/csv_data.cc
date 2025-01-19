/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup csv
 */

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_pointcloud.hh"

#include "csv_data.hh"

namespace blender::io::csv {

CsvData::CsvData(const int64_t rows_num,
                 const Span<std::string> column_names,
                 const Span<eCustomDataType> column_types)
    : data(column_names.size()),
      rows_num(rows_num),
      columns_num(column_names.size()),
      column_names(column_names),
      column_types(column_types)
{
  for (const int i : IndexRange(this->columns_num)) {
    data[i] = GArray(*bke::custom_data_type_to_cpp_type(this->column_types[i]), rows_num);
  }
}

PointCloud *CsvData::to_point_cloud() const
{
  PointCloud *point_cloud = BKE_pointcloud_new_nomain(rows_num);

  /* Set all positions to be zero */
  point_cloud->positions_for_write().fill(float3(0.0f, 0.0f, 0.0f));

  /* Fill the attributes */
  for (const int i : IndexRange(columns_num)) {
    const StringRef column_name = column_names[i];
    const eCustomDataType column_type = column_types[i];
    void *column_data = get_data_of_garray(data[i], column_type);
    CustomData_add_layer_named_with_data(
        &point_cloud->pdata, column_type, column_data, rows_num, column_name, nullptr);
  }

  return point_cloud;
}

void *CsvData::get_data_of_garray(const GSpan span, const eCustomDataType type) const
{
  const CPPType *cpp_type = bke::custom_data_type_to_cpp_type(type);
  void *data = MEM_mallocN_aligned(rows_num * cpp_type->size(), cpp_type->alignment(), __func__);
  std::memcpy(data, span.data(), rows_num * cpp_type->size());
  return data;
}

}  // namespace blender::io::csv
