/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "NOD_geometry_nodes_srna.hh"

#include "DNA_node_types.h"

#include "BKE_node_runtime.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

namespace blender::nodes {

const EnumPropertyItem geometry_nodes_input_type_items[] = {
    {int(GeometryNodesInputType::Value), "VALUE", 0, "Value", "Pass a single value"},
    {int(GeometryNodesInputType::Attribute),
     "ATTRIBUTE",
     0,
     "Attribute",
     "Pass an attribute as field"},
    {0, nullptr, 0, nullptr, nullptr}};

static StructRNA *get_input_socket_struct_rna(const bNodeTree &tree,
                                              const bNodeTreeInterfaceSocket &socket,
                                              GeneratedTreeSrnaData &r_generated)
{
  const bke::bNodeSocketType *stype = socket.socket_typeinfo();
  if (!stype) {
    return nullptr;
  }
  const StringRefNull srna_identifier = r_generated.scope.allocator().copy_string(
      fmt::format("{}_{}", stype->idname, socket.identifier));

  StructRNA *srna = RNA_def_struct_ptr(&BLENDER_RNA, srna_identifier.c_str(), &RNA_PropertyGroup);
  BLI_assert(!RNA_struct_in_public_namespace(srna));
  r_generated.structs.append(srna);

  if (stype->make_geometry_nodes_input_srna) {
    stype->make_geometry_nodes_input_srna(tree, *srna, socket, r_generated);
  }

  return srna;
}

static StructRNA *create_inputs_srna(const bNodeTree &tree, GeneratedTreeSrnaData &r_generated)
{
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, "GeometryNodesInterfaceInputs", &RNA_PropertyGroup);
  BLI_assert(!RNA_struct_in_public_namespace(srna));
  r_generated.structs.append(srna);

  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    StructRNA *socket_srna = get_input_socket_struct_rna(tree, *socket, r_generated);
    if (!socket_srna) {
      continue;
    }
    const StringRefNull identifier = r_generated.scope.allocator().copy_string(socket->identifier);
    r_generated.inputs_map.add_as(identifier, socket_srna);
    RNA_def_pointer_runtime(
        srna, identifier.c_str(), socket_srna, socket->name, socket->description);
  }

  return srna;
}

static StructRNA *create_outputs_srna(const bNodeTree & /*tree*/,
                                      GeneratedTreeSrnaData &r_generated)
{
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, "GeometryNodesInterfaceOutputs", &RNA_PropertyGroup);
  BLI_assert(!RNA_struct_in_public_namespace(srna));
  r_generated.structs.append(srna);

  return srna;
}

StructRNA *get_geometry_nodes_interface_srna(const bNodeTree &tree,
                                             GeneratedTreeSrnaData &r_generated)
{
  tree.ensure_interface_cache();
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, "GeometryNodesInterface", &RNA_NodesModifierProperties);
  BLI_assert(!RNA_struct_in_public_namespace(srna));
  r_generated.structs.append(srna);

  StructRNA *inputs_srna = create_inputs_srna(tree, r_generated);
  StructRNA *outputs_srna = create_outputs_srna(tree, r_generated);

  RNA_def_pointer_runtime(srna, "inputs", inputs_srna, "Inputs", "Settings for input sockets");
  RNA_def_pointer_runtime(srna, "outputs", outputs_srna, "Outputs", "Settings for output sockets");

  return srna;
}

}  // namespace blender::nodes
