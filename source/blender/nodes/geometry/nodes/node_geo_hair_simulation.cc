/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BKE_geometry_set.hh"
#include "BKE_type_conversions.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_xpbd_constraints.hh"
#include "NOD_xpbd_solver.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_hair_simulation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Hair").supported_type(bke::GeometryComponent::Type::Curve);

  b.add_output<decl::Geometry>("Hair").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  UNUSED_VARS(ptr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  UNUSED_VARS(node);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet hair_geometry = params.extract_input<GeometrySet>("Hair");

  params.set_output("Hair", std::move(hair_geometry));
}

static void node_rna(StructRNA *srna)
{
  UNUSED_VARS(srna);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeHairSimulation");
  ntype.ui_name = "Hair Simulation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_hair_simulation_cc
