/* SPDX-FileCopyrightText: 2026 Blender Authors
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

static void collection_children_recursive(Collection *collection,
                                          Vector<Collection *> &collections,
                                          Set<Collection *> &visited)
{
  for (CollectionChild &child : collection->children) {
    Collection *cc = child.collection;
    if (visited.add(cc)) {
      collections.append(cc);
      collection_children_recursive(cc, collections, visited);
    }
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Collection>("Collection").optional_label();
  b.add_input<decl::Bool>("Recursive").description("Recursively retrieve collections and objects");
  b.add_output<decl::Collection>("Collections").structure_type(StructureType::List);
  b.add_output<decl::Object>("Objects").structure_type(StructureType::List);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Collection *collection = params.extract_input<Collection *>("Collection");
  const bool recursive = params.extract_input<bool>("Recursive");

  if (collection == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  auto *collections = new ImplicitSharedValue<Vector<Collection *>>();
  if (recursive) {
    Set<Collection *> visited;
    collection_children_recursive(collection, collections->data, visited);
  }
  else {
    for (CollectionChild &child : collection->children) {
      collections->data.append(child.collection);
    }
  }

  std::sort(collections->data.begin(),
            collections->data.end(),
            [](const Collection *a, const Collection *b) {
              return BLI_strcasecmp_natural(BKE_id_name(a->id), BKE_id_name(b->id)) < 0;
            });
  List::ArrayData collections_array_data = {collections->data.data(),
                                            ImplicitSharingPtr<>(collections)};

  params.set_output("Collections",
                    List::create(CPPType::get<Collection *>(),
                                 std::move(collections_array_data),
                                 collections->data.size()));

  if (!params.output_is_required("Objects")) {
    params.set_default_remaining_outputs();
    return;
  }

  auto *objects = new ImplicitSharedValue<Vector<Object *>>();

  Vector<Collection *> obj_collections;
  obj_collections.append(collection);
  if (recursive) {
    obj_collections.extend(collections->data);
  }

  Set<const Object *> obj_visited;
  for (Collection *col : obj_collections) {
    for (CollectionObject &cob : col->gobject) {
      Object *obj_original = (Object *)DEG_get_original(cob.ob);
      if (obj_visited.add(obj_original)) {
        objects->data.append(obj_original);
      }
    }
  }

  std::sort(objects->data.begin(), objects->data.end(), [](const Object *a, const Object *b) {
    return BLI_strcasecmp_natural(BKE_id_name(a->id), BKE_id_name(b->id)) < 0;
  });

  List::ArrayData objects_array_data = {objects->data.data(), ImplicitSharingPtr<>(objects)};

  params.set_output(
      "Objects",
      List::create(CPPType::get<Object *>(), std::move(objects_array_data), objects->data.size()));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCollectionChildren");
  ntype.ui_name = "Collection Children";
  ntype.ui_description =
      "Retrieve children collection and object lists from a collection with name-base order";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_collection_children_cc
