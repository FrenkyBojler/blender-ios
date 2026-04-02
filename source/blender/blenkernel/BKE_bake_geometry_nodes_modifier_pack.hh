/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_modifier_types.h"

#include "BKE_bake_items_paths.hh"
#include "BKE_packedFile.hh"

namespace blender {

struct ReportList;
struct Main;

namespace bke::bake {

NodesModifierPackedBake *pack_bake_from_disk(const BakePath &bake_path, ReportList *reports);

[[nodiscard]] bool unpack_bake_to_disk(const NodesModifierPackedBake &packed_bake,
                                       const BakePath &bake_path,
                                       ReportList *reports);

enum class PackGeometryNodesBakeResult {
  NO_DATA_FOUND,
  PACKED_ALREADY,
  SUCCESS,
};

PackGeometryNodesBakeResult pack_geometry_nodes_bake(Main &bmain,
                                                     ReportList *reports,
                                                     Object &object,
                                                     NodesModifierData &nmd,
                                                     NodesModifierBake &bake);

enum class UnpackGeometryNodesBakeResult {
  BLEND_FILE_NOT_SAVED,
  NO_PACKED_DATA,
  ERROR,
  SUCCESS,
};

UnpackGeometryNodesBakeResult unpack_geometry_nodes_bake(Main &bmain,
                                                         ReportList *reports,
                                                         Object &object,
                                                         NodesModifierData &nmd,
                                                         NodesModifierBake &bake,
                                                         ePF_FileStatus how);

}  // namespace bke::bake
}  // namespace blender
