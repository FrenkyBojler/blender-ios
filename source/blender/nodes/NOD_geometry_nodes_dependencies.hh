/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "BLI_struct_equality_utils.hh"

struct ID;
struct Object;
struct bNodeTree;

namespace blender::nodes {

struct GeometryNodesEvalDependencies {
  /** Maps `session_uid` to the corresponding data-block. */
  Map<uint32_t, ID *> ids;

  struct ObjectDeps {
    bool transform = false;
    bool geometry = false;

    BLI_STRUCT_EQUALITY_OPERATORS_2(ObjectDeps, transform, geometry);
  };
  static constexpr ObjectDeps all_object_deps{true, true};

  /** Additional information for object dependencies. */
  Map<uint32_t, ObjectDeps> objects_info;

  bool needs_own_transform = false;
  bool needs_active_camera = false;

  void add_generic_id(ID *id);

  void add_generic_id_full(ID *id);

  void add_object(Object *object, const ObjectDeps &object_deps = all_object_deps);

  void merge(const GeometryNodesEvalDependencies &other);

  BLI_STRUCT_EQUALITY_OPERATORS_2(GeometryNodesEvalDependencies, ids, objects_info);
};

void gather_geometry_nodes_eval_dependencies(bNodeTree &ntree,
                                             GeometryNodesEvalDependencies &deps);

}  // namespace blender::nodes
