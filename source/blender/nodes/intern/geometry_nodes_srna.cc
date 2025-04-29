/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_srna.hh"

#include "BLI_rand.hh"

#include "DNA_node_types.h"

#include "BKE_node_runtime.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

namespace blender::nodes {

static StringRefNull get_unique_struct_name(const bNodeTree &ntree,
                                            GeneratedTreeSrnaData &r_generated)
{
  RandomNumberGenerator rng = RandomNumberGenerator::from_random_seed();
  std::string str = ntree.id.name + 2;
  str += '_';
  for ([[maybe_unused]] const int i : IndexRange(10)) {
    str += char(rng.get_int32(26) + 97);
  }
  return r_generated.scope.allocator().copy_string(str);
}

static StructRNA *get_input_socket_struct_rna(const bNodeTree &tree,
                                              const bNodeTreeInterfaceSocket &socket,
                                              GeneratedTreeSrnaData &r_generated)
{
  const bke::bNodeSocketType *stype = socket.socket_typeinfo();
  if (!stype) {
    return nullptr;
  }

  const StringRefNull struct_identifier = get_unique_struct_name(tree, r_generated);
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, struct_identifier.c_str(), &RNA_PropertyGroup);
  r_generated.structs.append(srna);

  PropertyRNA *prop;
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(stype->type);
  switch (socket_type) {
    case SOCK_FLOAT: {
      const auto *data = static_cast<const bNodeSocketValueFloat *>(socket.socket_data);
      prop = RNA_def_float(srna,
                           "value",
                           data->value,
                           -FLT_MAX,
                           FLT_MAX,
                           socket.name,
                           socket.description,
                           data->min,
                           data->max);
      RNA_def_property_subtype(prop, PropertySubType(data->subtype));

      static const EnumPropertyItem input_type_items[] = {
          {int(GeometryNodesInputTypeFloat::Value), "VALUE", 0, "Value", ""},
          {int(GeometryNodesInputTypeFloat::Attribute), "ATTRIBUTE", 0, "Attribute", ""},
          {0, nullptr, 0, nullptr, nullptr}};
      prop = RNA_def_enum(srna,
                          "input_type",
                          input_type_items,
                          int(GeometryNodesInputTypeFloat::Value),
                          "Input Type",
                          "");
      break;
    }
    case SOCK_INT: {
      const auto *data = static_cast<const bNodeSocketValueInt *>(socket.socket_data);
      prop = RNA_def_int(srna,
                         "value",
                         data->value,
                         INT32_MIN,
                         INT32_MAX,
                         socket.name,
                         socket.description,
                         data->min,
                         data->max);
      RNA_def_property_subtype(prop, PropertySubType(data->subtype));

      static const EnumPropertyItem input_type_items[] = {
          {int(GeometryNodesInputTypeInt::Value), "VALUE", 0, "Value", ""},
          {int(GeometryNodesInputTypeInt::Attribute), "ATTRIBUTE", 0, "Attribute", ""},
          {0, nullptr, 0, nullptr, nullptr}};
      prop = RNA_def_enum(srna,
                          "input_type",
                          input_type_items,
                          int(GeometryNodesInputTypeInt::Value),
                          "Input Type",
                          "");
      break;
    }
    default: {
      break;
    }
  }

  return srna;
}

StructRNA *get_geometry_nodes_inputs_srna(const bNodeTree &tree,
                                          GeneratedTreeSrnaData &r_generated)
{
  const StringRefNull struct_identifier = get_unique_struct_name(tree, r_generated);
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, struct_identifier.c_str(), &RNA_NodesModifierProperties);
  r_generated.structs.append(srna);

  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    StructRNA *socket_srna = get_input_socket_struct_rna(tree, *socket, r_generated);
    if (!socket_srna) {
      continue;
    }
    RNA_def_pointer_runtime(
        srna, socket->identifier, socket_srna, socket->name, socket->description);
  }

  return srna;
}

}  // namespace blender::nodes
