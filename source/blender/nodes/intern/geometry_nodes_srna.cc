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

StructRNA *get_geometry_nodes_inputs_srna(const bNodeTree &tree,
                                          GeneratedTreeSrnaData &r_generated)
{
  StructRNA *srna = RNA_def_struct_ptr(
      &BLENDER_RNA, "GeometryNodesInputs", &RNA_NodesModifierProperties);
  BLI_assert(!RNA_struct_in_public_namespace(srna));
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
