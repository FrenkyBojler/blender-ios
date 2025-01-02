/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_path_utils.hh"

#include "node_geometry_import_cache.hh"

namespace blender::nodes::geometry_import_cache {

class GeometryReadKey : public GenericKey {
 public:
  std::string absolute_file_path;
  FileType file_type;

  uint64_t hash() const override
  {
    return get_default_hash(this->absolute_file_path, this->file_type);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(GeometryReadKey, absolute_file_path, file_type)

  bool equal_to(const GenericKey &other) const override
  {
    if (const auto *other_typed = dynamic_cast<const GeometryReadKey *>(&other)) {
      return *this == *other_typed;
    }
    return false;
  }

  std::unique_ptr<GenericKey> to_storable() const override
  {
    return std::make_unique<GeometryReadKey>(*this);
  }
};

void import_geometry_cache_clear_all()
{
  memory_cache::clear();
}

std::shared_ptr<const GeometryReadValue> import_geometry_cached(
    const FileType file_type,
    const StringRef absolute_file_path,
    FunctionRef<std::unique_ptr<GeometryReadValue>()> compute_fn)
{
  BLI_assert_msg(!BLI_path_is_rel(absolute_file_path.data()),
                 "File path is not absolute, try saving the blend file");

  GeometryReadKey cache_key;
  cache_key.file_type = file_type;
  cache_key.absolute_file_path = absolute_file_path;

  return memory_cache::get<GeometryReadValue>(std::move(cache_key), compute_fn);
}

}  // namespace blender::nodes::geometry_import_cache
