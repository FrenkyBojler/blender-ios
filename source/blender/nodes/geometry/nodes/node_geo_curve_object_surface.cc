/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_object_surface_cc {

NODE_STORAGE_FUNCS(NodeGeometryObjectInfo)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).optional_label();
  b.add_output<decl::Object>("Surface"_ustr);
  b.add_output<decl::String>("UV Map"_ustr);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object"_ustr);
  if (!object) {
    params.set_default_remaining_outputs();
    return;
  }
  ID *data = object->data;
  if (!data || GS(data->name) != ID_CV) {
    params.error_message_add(NodeWarningType::Warning, TIP_("Not a curves object"));
    params.set_default_remaining_outputs();
    return;
  }
  Curves &curves = *id_cast<Curves *>(data);
  params.set_output("Surface"_ustr, curves.surface);
  params.set_output("UV Map"_ustr, std::string(StringRef(curves.surface_uv_map)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_cmp_node_type_base(&ntype, "GeometryNodeCurveObjectSurface"_ustr);
  ntype.ui_name = "Curve Object Surface";
  ntype.ui_description = "Retrieve surface information from a curve object";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_160;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_object_surface_cc
