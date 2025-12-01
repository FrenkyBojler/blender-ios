/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "DNA_collection_types.h"

#include "BKE_collection.hh"
#include "BKE_lib_id.hh"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

#include <algorithm>

namespace blender::nodes::node_geo_collection_children_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Collection>("Collection").optional_label();
  b.add_input<decl::Bool>("Recursive").description("Recursively retrieve collections and objects");
  b.add_output<decl::Object>("Objects").structure_type(StructureType::List);
  b.add_output<decl::Collection>("Collections").structure_type(StructureType::List);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Collection *collection = params.extract_input<Collection *>("Collection");
  const bool recursive = params.extract_input<bool>("Recursive");

  if (collection == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  auto *objects = new ImplicitSharedValue<Vector<Object *>>();
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
    objects->data.append(object);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  std::sort(objects->data.begin(), objects->data.end(), [](const Object *a, const Object *b) {
    return BLI_strcasecmp_natural(BKE_id_name(a->id), BKE_id_name(b->id)) < 0;
  });

  List::ArrayData objects_array_data = {objects->data.data(), ImplicitSharingPtr<>(objects)};

  //  params.set_output("Collections",
  //                   List::create(CPPType::get<Collection *>(),
  //                                std::move(collections_array_data),
  //                                collections->data.size()));
  params.set_output(
      "Objects",
      List::create(CPPType::get<Object *>(), std::move(objects_array_data), objects->data.size()));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCollectionChildren");
  ntype.ui_name = "Collection Children";
  ntype.ui_description = "Retrieve children collection and object lists from a collection";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_collection_children_cc
