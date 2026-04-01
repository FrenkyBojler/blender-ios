/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_length_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curve")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curve to compute the length of");
  b.add_output<decl::Float>("Length")
      .description("Total length (sum of the lengths of the Splines in the Curve)");
  b.add_output<decl::Float>("Spline Lengths")
      .structure_type(StructureType::List)
      .description("List of the lengths of each Spline in the Curve");
}

struct LengthsData {
    float total_length = 0.0f;
    std::vector<float> spline_lengths;
  };

static LengthsData curves_total_length(const bke::CurvesGeometry &curves)
{
  const VArray<bool> cyclic = curves.cyclic();
  curves.ensure_evaluated_lengths();

  LengthsData result;

  for (const int i : curves.curves_range()) {
    float length = curves.evaluated_length_total_for_curve(i, cyclic[i]);
    result.spline_lengths.push_back(length);
    result.total_length += length;
  }
  return result;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  int curve_num = 0;
  float total_length = 0.0f;
  std::vector<float> spline_lengths;
  if (geometry_set.has_curves()) {
    const Curves &curves_id = *geometry_set.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    curve_num = curves.curve_num;
    const Object *obj = params.self_object();
    if (obj->object_to_world() != blender::float4x4::identity()) {
      bke::CurvesGeometry curves_geom = curves;
      curves_geom.transform(obj->object_to_world().view<4, 4>());
      LengthsData lengths_data = curves_total_length(curves_geom);
      total_length += lengths_data.total_length;
      spline_lengths = lengths_data.spline_lengths;
    } else {
      LengthsData lengths_data = curves_total_length(curves);
      total_length += lengths_data.total_length;
      spline_lengths = lengths_data.spline_lengths;
    }
  }
  else if (geometry_set.has_grease_pencil()) {
    using namespace bke::greasepencil;
    const GreasePencil &grease_pencil = *geometry_set.get_grease_pencil();
    for (const int layer_index : grease_pencil.layers().index_range()) {
      const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
      if (drawing == nullptr) {
        continue;
      }
      const bke::CurvesGeometry &curves = drawing->strokes();
      curve_num = curves.curve_num;
      const Object *obj = params.self_object();
      if (obj->object_to_world() != blender::float4x4::identity()) {
        bke::CurvesGeometry curves_geom = curves;
        curves_geom.transform(obj->object_to_world().view<4, 4>());
        LengthsData lengths_data = curves_total_length(curves_geom);
        total_length += lengths_data.total_length;
        spline_lengths = lengths_data.spline_lengths;
      } else {
        LengthsData lengths_data = curves_total_length(curves);
        total_length += lengths_data.total_length;
        spline_lengths = lengths_data.spline_lengths;
      }
    }
  }
  else {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Length", total_length);
  if (params.output_is_required("Spline Lengths")) {
    const CPPType &cpp_type = CPPType::get<float>();
    ListPtr list_ptr = List::create(cpp_type,
                                    List::ArrayData::ForUninitialized(cpp_type, curve_num),
                                    curve_num);
    GMutableSpan values(cpp_type,
                        const_cast<void *>(std::get<List::ArrayData>(list_ptr->data()).data),
                        curve_num);
    for (int i = 0; i < spline_lengths.size(); i++) {
      cpp_type.copy_construct((void *)&spline_lengths[i], values[i]);
    }
    params.set_output("Spline Lengths", std::move(list_ptr));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCurveLength", GEO_NODE_CURVE_LENGTH);
  ntype.ui_name = "Curve Length";
  ntype.ui_description = "Retrieve the length of all splines added together";
  ntype.enum_name_legacy = "CURVE_LENGTH";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_length_cc
