/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_object_types.h"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_id_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::DataBlockID>("ID"_ustr).optional_label();
  b.add_output<decl::String>("Name"_ustr);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const ID *id = params.extract_input<ID *>("ID"_ustr);

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Name"_ustr, std::string(&id->name[2]));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_cmp_node_type_base(&ntype, "GeometryNodeIDInfo"_ustr);
  ntype.ui_name = "ID Info";
  ntype.ui_description = "Retrieve information from an ID";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_id_info_cc
