/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BKE_geometry_set.hh"
#include "BKE_report.hh"

#include "BLI_memory_cache.hh"
#include "BLI_memory_counter.hh"

namespace blender::nodes::geometry_import_cache {

enum class FileType : uint8_t { STL, OBJ, PLY };

class GeometryReadValue : public memory_cache::CachedValue {
 public:
  bke::GeometrySet geometry;
  ReportList reports;

  GeometryReadValue(bke::GeometrySet geometry, ReportList *reports);

  ~GeometryReadValue();

  void count_memory(MemoryCounter &memory) const override;
};

/**
 * Completely clears the memory cache
 */
void import_geometry_cache_clear_all();

/**
 * Imports geometry by first searching for it in the memory cache
 * by absolute path and file type. If the cache key is not found
 * the compute_fn is invoked to provide the geometry which is then
 * cached and returned
 */
std::shared_ptr<const GeometryReadValue> import_geometry_cached(
    const FileType file_type,
    const StringRef absolute_file_path,
    FunctionRef<std::unique_ptr<GeometryReadValue>()> compute_fn);

}  // namespace blender::nodes::geometry_import_cache
