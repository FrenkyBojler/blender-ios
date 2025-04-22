/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "GEO_curves_detect_corners.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_find_corner_selection_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Angle Min")
      .min(0.0)
      .max(M_PI)
      .default_value(DEG2RADF(40.0f))
      .subtype(PROP_ANGLE)
      .supports_field()
      .description("Detected angles above this value are considered corners");
  b.add_input<decl::Float>("Radius Min")
      .min(0.0)
      .default_value(0.001)
      .supports_field()
      .description("Minimum search radius for corner detection algorithm");
  b.add_input<decl::Float>("Radius Max")
      .min(0.0)
      .default_value(0.2)
      .supports_field()
      .description("Maximum search radius for corner detection algorithm");
  b.add_input<decl::Int>("Samples Max")
      .min(1)
      .max(32)
      .default_value(16)
      .supports_field()
      .description("Maximum amount of points to test for a potential corner");
  b.add_output<decl::Bool>("Corners").field_source_reference_all().description(
      "The selection of detected corners of the poly curves");
}

class FindCurveCornersFieldInput final : public bke::GeometryFieldInput {
  Field<float> angle_min_;
  Field<float> radius_min_;
  Field<float> radius_max_;
  Field<int> samples_max_;

 public:
  FindCurveCornersFieldInput(Field<float> angle_min,
                             Field<float> radius_min,
                             Field<float> radius_max,
                             Field<int> samples_max)
      : bke::GeometryFieldInput(CPPType::get<bool>(), "Find Curve Corners Selection node"),
        angle_min_(angle_min),
        radius_min_(radius_min),
        radius_max_(radius_max),
        samples_max_(samples_max)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    if (context.domain() != AttrDomain::Point) {
      return {};
    }
    const bke::CurvesGeometry *curves_ptr = context.curves_or_strokes();
    if (!curves_ptr) {
      return {};
    }
    const bke::CurvesGeometry &curves = *curves_ptr;
    if (curves.is_empty()) {
      return {};
    }

    const bke::GeometryFieldContext sub_context{context, AttrDomain::Curve};
    fn::FieldEvaluator evaluator{sub_context, curves.curves_num()};
    evaluator.add(angle_min_);
    evaluator.add(radius_min_);
    evaluator.add(radius_max_);
    evaluator.add(samples_max_);
    evaluator.evaluate();
    const VArray<float> angle_min = evaluator.get_evaluated<float>(0);
    const VArray<float> radius_min = evaluator.get_evaluated<float>(1);
    const VArray<float> radius_max = evaluator.get_evaluated<float>(2);
    const VArray<int> samples_max = evaluator.get_evaluated<int>(3);

    Array<bool> selection = geometry::curves_detect_corners(
        curves, angle_min, radius_min, radius_max, samples_max);

    return VArray<bool>::ForContainer(std::move(selection));
  };

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const final
  {
    angle_min_.node().for_each_field_input_recursive(fn);
    radius_min_.node().for_each_field_input_recursive(fn);
    radius_max_.node().for_each_field_input_recursive(fn);
    samples_max_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const final
  {
    return get_default_hash(angle_min_, radius_min_, radius_max_, samples_max_);
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const FindCurveCornersFieldInput *other_endpoint =
            dynamic_cast<const FindCurveCornersFieldInput *>(&other))
    {
      return angle_min_ == other_endpoint->angle_min_ &&
             radius_min_ == other_endpoint->radius_min_ &&
             radius_max_ == other_endpoint->radius_max_ &&
             samples_max_ == other_endpoint->samples_max_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const GeometryComponent & /*component*/) const final
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float> angle_min = params.extract_input<Field<float>>("Angle Min");
  Field<float> radius_min = params.extract_input<Field<float>>("Radius Min");
  Field<float> radius_max = params.extract_input<Field<float>>("Radius Max");
  Field<int> samples_max = params.extract_input<Field<int>>("Samples Max");
  Field<bool> corner_selection_field{std::make_shared<FindCurveCornersFieldInput>(
      angle_min, radius_min, radius_max, samples_max)};
  params.set_output("Corners", std::move(corner_selection_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCurveFindCornerSelection");
  ntype.ui_name = "Find Corner Selection";
  ntype.ui_description = "Find a selection of corners using the search parameters for each curve";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_type_size(ntype, 160, 120, NODE_DEFAULT_MAX_WIDTH);

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_find_corner_selection_cc
