/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_pointcloud.hh"

#include "GEO_hair_constraint_functions.hh"
#include "GEO_hair_constraints.hh"

#include "NOD_geo_hair_constraints.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

namespace blender::nodes {

namespace xpbd_constraints {

using geometry::hair_constraints::ConstraintType;
using geometry::hair_constraints::ConstraintTypeInfo;

/* Constraint attributes. */
constexpr StringRef ATTR_SOLVER_GROUP = "solver_group";
constexpr StringRef ATTR_ALPHA = "compliance";
constexpr StringRef ATTR_BETA = "damping";
constexpr StringRef ATTR_POINT1 = "point1";
constexpr StringRef ATTR_POINT2 = "point2";
constexpr StringRef ATTR_ACTIVE = "active";
constexpr StringRef ATTR_LAST_ACTIVE = "last_active";

namespace position_goal {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Vector>("Goal").field_on_all().description(
      "Target location of the constraint");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<float3> goal_field = params.extract_input<Field<float3>>("Goal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, component.attribute_domain_size(AttrDomain::Point)};
  evaluator.set_selection(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(goal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  VArray<float> compliance = evaluator.get_evaluated<float>(0);
  VArray<float> damping = evaluator.get_evaluated<float>(1);
  VArray<float3> goal_position = evaluator.get_evaluated<float3>(2);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float3> output_goal_position =
      attributes.lookup_or_add_for_write_only_span<float3>("goal_position", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);
  goal_position.materialize_compressed(selection, output_goal_position.span);
  output_solver_group.span.fill(0);

  output_point1.finish();
  output_compliance.finish();
  output_damping.finish();
  output_goal_position.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodePositionGoalConstraints");
  ntype.ui_name = "Position Goal Constraints";
  ntype.ui_description = "Define position goal constraints that move points to a given location";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace position_goal

namespace rotation_goal {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Rotation>("Goal").field_on_all().description(
      "Target rotation of the constraint");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<math::Quaternion> goal_field = params.extract_input<Field<math::Quaternion>>("Goal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, component.attribute_domain_size(AttrDomain::Point)};
  evaluator.set_selection(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(goal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float> compliance = evaluator.get_evaluated<float>(0);
  const VArray<float> damping = evaluator.get_evaluated<float>(1);
  const VArray<math::Quaternion> goal_rotation = evaluator.get_evaluated<math::Quaternion>(2);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<math::Quaternion> output_goal_rotation =
      attributes.lookup_or_add_for_write_only_span<math::Quaternion>("goal_rotation",
                                                                     AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);
  goal_rotation.materialize_compressed(selection, output_goal_rotation.span);
  output_solver_group.span.fill(0);

  output_point1.finish();
  output_compliance.finish();
  output_damping.finish();
  output_goal_rotation.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeRotationGoalConstraints");
  ntype.ui_name = "Rotation Goal Constraints";
  ntype.ui_description =
      "Define rotation goal constraints that align the rotation with a given orientation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace rotation_goal

namespace stretch_shear {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Vector>("Rest Position")
      .implicit_field_on_all(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .description("Rest position defining the edge length of constraints");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<float3> rest_position_field = params.extract_input<Field<float3>>("Rest Position");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  /* Note: selection is not used to limit the evaluation, because attributes from unselected points
   * may be needed to compute constraint properties (edge length). */
  evaluator.add(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(rest_position_field);
  evaluator.evaluate();

  /* Skip end points of curves, these cannot have stretch/shear constraints. */
  Array<bool> point_valid(curves.points_num(), true);
  IndexMask(curves.curves_range()).foreach_index(GrainSize(256), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    if (!points.is_empty()) {
      point_valid[points.last()] = false;
    }
  });

  IndexMaskMemory memory;
  const IndexMask selection = IndexMask::from_bools(
      evaluator.get_evaluated_as_mask(0), point_valid, memory);
  const VArray<float> compliance = evaluator.get_evaluated<float>(1);
  const VArray<float> damping = evaluator.get_evaluated<float>(2);
  const VArraySpan<float3> rest_position = evaluator.get_evaluated<float3>(3);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_point2 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT2, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float> output_edge_length =
      attributes.lookup_or_add_for_write_only_span<float>("edge_length", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    /* Curve end points have been excluded, so index + 1 is safe. */
    output_point1.span[pos] = index;
    output_point2.span[pos] = index + 1;
    /* Use rest position distance as the edge length. */
    output_edge_length.span[pos] = math::distance(rest_position[index], rest_position[index + 1]);
    /* Alternating by odd/even index separates curve constraints into independent groups. */
    output_solver_group.span[pos] = index % 2;
  });
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);

  output_point1.finish();
  output_point2.finish();
  output_compliance.finish();
  output_damping.finish();
  output_edge_length.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeStretchShearConstraints");
  ntype.ui_name = "Stretch/Shear Constraints";
  /* TODO Difficult to describe in a single sentence: The rotation of a hair segment is a generic
   * independent attribute. This constraint ensures that the distance between points matches the
   * expected edge length (zero stretch) and the orientation Z axis aligns with the actual
   * direction of the segment between neighboring points (zero shear). It can either move the
   * points or change the orientation, balanced by the relative position/rotation weights (inverse
   * point masses and moments of inertia). */
  ntype.ui_description =
      "Define stretch/shear constraints that limit edge length and align positions to segment "
      "rotation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace stretch_shear

namespace bend_twist {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Vector>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Rotation>("Rest Rotation")
      .field_on_all()
      .description("Rest rotation defining the relative orientation of segments");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float3> compliance_field = params.extract_input<Field<float3>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<math::Quaternion> rest_rotation_field =
      params.extract_input<Field<math::Quaternion>>("Rest Rotation");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  /* Note: selection is not used to limit the evaluation, because attributes from unselected points
   * may be needed to compute constraint properties (edge length). */
  evaluator.add(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(rest_rotation_field);
  evaluator.evaluate();

  /* Skip end points of curves, these cannot have bend/twist constraints. */
  Array<bool> point_valid(curves.points_num(), true);
  IndexMask(curves.curves_range()).foreach_index(GrainSize(256), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    if (!points.is_empty()) {
      point_valid[points.last()] = false;
    }
  });

  IndexMaskMemory memory;
  const IndexMask selection = IndexMask::from_bools(
      evaluator.get_evaluated_as_mask(0), point_valid, memory);
  const VArray<float3> compliance = evaluator.get_evaluated<float3>(1);
  const VArray<float> damping = evaluator.get_evaluated<float>(2);
  const VArraySpan<math::Quaternion> rest_rotation = evaluator.get_evaluated<math::Quaternion>(3);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_point2 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT2, AttrDomain::Point);
  SpanAttributeWriter<float3> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float3>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float3> output_darboux_vector =
      attributes.lookup_or_add_for_write_only_span<float3>("darboux_vector", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    /* Curve end points have been excluded, so index + 1 is safe. */
    output_point1.span[pos] = index;
    output_point2.span[pos] = index + 1;
    /* Use rest rotation difference to compute a Darboux vector. */
    output_darboux_vector.span[pos] = (math::invert_normalized(rest_rotation[index]) *
                                       rest_rotation[index + 1])
                                          .imaginary_part();
    /* Alternating by odd/even index separates curve constraints into independent groups. */
    output_solver_group.span[pos] = index % 2;
  });
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);

  output_point1.finish();
  output_point2.finish();
  output_compliance.finish();
  output_damping.finish();
  output_darboux_vector.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBendTwistConstraints");
  ntype.ui_name = "Bend/Twist Constraints";
  ntype.ui_description =
      "Define bend/twist constraints that limit relative rotation between two orientations";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace bend_twist

namespace contact {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Int>("Collider").min(0).description("Index of the collider");
  b.add_input<decl::Float>("Friction")
      .min(0.0f)
      .field_on_all()
      .description("Reduction of velocity along the surface during contact");
  b.add_input<decl::Float>("Restitution")
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .field_on_all()
      .description("Amount the point bounces back after colliding");
  b.add_input<decl::Float>("Threshold Normal Velocity")
      .min(0.0f)
      .default_value(0.5f)
      .field_on_all()
      .description("No bouncing occurs when normal velocity is below this threshold");
  b.add_input<decl::Vector>("Local Position")
      .field_on_all()
      .description("Contact position relative to the point");
  b.add_input<decl::Vector>("Collider Position")
      .field_on_all()
      .description("Contact position relative to the collider");
  b.add_input<decl::Vector>("Normal").field_on_all().description("Contact normal");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const int collider_index = params.extract_input<int>("Collider");
  const Field<float> friction_field = params.extract_input<Field<float>>("Friction");
  const Field<float> restitution_field = params.extract_input<Field<float>>("Restitution");
  const Field<float> threshold_normal_velocity_field = params.extract_input<Field<float>>(
      "Threshold Normal Velocity");
  const Field<float3> local_position_field = params.extract_input<Field<float3>>("Local Position");
  const Field<float3> collider_position_field = params.extract_input<Field<float3>>(
      "Collider Position");
  const Field<float3> normal_field = params.extract_input<Field<float3>>("Normal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  evaluator.set_selection(selection_field);
  evaluator.add(friction_field);
  evaluator.add(restitution_field);
  evaluator.add(threshold_normal_velocity_field);
  evaluator.add(local_position_field);
  evaluator.add(collider_position_field);
  evaluator.add(normal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float> friction = evaluator.get_evaluated<float>(0);
  const VArray<float> restitution = evaluator.get_evaluated<float>(1);
  const VArray<float> threshold_normal_velocity = evaluator.get_evaluated<float>(2);
  const VArray<float3> local_position = evaluator.get_evaluated<float3>(3);
  const VArray<float3> collider_position = evaluator.get_evaluated<float3>(4);
  const VArraySpan<float3> normal = evaluator.get_evaluated<float3>(5);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_collider_index =
      attributes.lookup_or_add_for_write_only_span<int>("collider_index", AttrDomain::Point);
  SpanAttributeWriter<float> output_friction = attributes.lookup_or_add_for_write_only_span<float>(
      "friction", AttrDomain::Point);
  SpanAttributeWriter<float> output_restitution =
      attributes.lookup_or_add_for_write_only_span<float>("restitution", AttrDomain::Point);
  SpanAttributeWriter<float> output_threshold_normal_velocity =
      attributes.lookup_or_add_for_write_only_span<float>("threshold_normal_velocity",
                                                          AttrDomain::Point);
  SpanAttributeWriter<float3> output_local_position1 =
      attributes.lookup_or_add_for_write_only_span<float3>("local_position1", AttrDomain::Point);
  SpanAttributeWriter<float3> output_local_position2 =
      attributes.lookup_or_add_for_write_only_span<float3>("local_position2", AttrDomain::Point);
  SpanAttributeWriter<float3> output_normal = attributes.lookup_or_add_for_write_only_span<float3>(
      "normal", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  output_collider_index.span.fill(collider_index);
  friction.materialize_compressed(selection, output_friction.span);
  restitution.materialize_compressed(selection, output_restitution.span);
  threshold_normal_velocity.materialize_compressed(selection,
                                                   output_threshold_normal_velocity.span);
  local_position.materialize_compressed(selection, output_local_position1.span);
  collider_position.materialize_compressed(selection, output_local_position2.span);
  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    output_normal.span[pos] = math::normalize(normal[index]);
  });
  /* There should only be one contact per point/collider pair, so the collider index can be used
   * to separate constraint groups. */
  output_solver_group.span.fill(collider_index);

  output_point1.finish();
  output_collider_index.finish();
  output_friction.finish();
  output_restitution.finish();
  output_threshold_normal_velocity.finish();
  output_local_position1.finish();
  output_local_position2.finish();
  output_normal.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeContactConstraints");
  ntype.ui_name = "Contact Constraints";
  ntype.ui_description = "Define contact constraints to react to collisions";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace contact

namespace constraint_function_nodes {

namespace mf = blender::fn::multi_function;

enum class ConstraintFunctionType {
  PositionGoal,
  RotationGoal,
  VelocityGoal,
  AngularVelocityGoal,
  StretchShear,
  BendTwist,
  ContactPosition,
  ContactVelocity,
};

/* Shortcuts. */
template<typename T> using mf_input = mf::ParamTag<mf::ParamCategory::SingleInput, T>;
template<typename T> using mf_output = mf::ParamTag<mf::ParamCategory::SingleOutput, T>;

template<typename ExecPreset> static auto stretch_shear_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool linearized_rotation,
         float3 lambda,
         float3 position1,
         float3 position2,
         math::Quaternion rotation,
         const float weight_pos1,
         const float weight_pos2,
         const float weight_rot,
         const float edge_length,
         const float alpha,
         float3 &lambda_out,
         float3 &position1_out,
         float3 &position2_out,
         math::Quaternion &rotation_out) -> void {
        if (linearized_rotation) {
          geometry::hair_constraints::apply_position_stretch_shear<true>(weight_pos1,
                                                                         weight_pos2,
                                                                         weight_rot,
                                                                         edge_length,
                                                                         alpha,
                                                                         lambda,
                                                                         position1,
                                                                         position2,
                                                                         rotation);
        }
        else {
          geometry::hair_constraints::apply_position_stretch_shear<false>(weight_pos1,
                                                                          weight_pos2,
                                                                          weight_rot,
                                                                          edge_length,
                                                                          alpha,
                                                                          lambda,
                                                                          position1,
                                                                          position2,
                                                                          rotation);
        }
        lambda_out = lambda;
        position1_out = position1;
        position2_out = position2;
        rotation_out = rotation;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Stretch/Shear Rod Constraint", call_fn, param_tags);
}

template<typename ExecPreset> static auto bend_twist_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool linearized_rotation,
         float3 lambda,
         math::Quaternion rotation1,
         math::Quaternion rotation2,
         const float3 weight_rot1,
         const float3 weight_rot2,
         const float3 &darboux_vector,
         const float3 &alpha,
         float3 &lambda_out,
         math::Quaternion &rotation_out1,
         math::Quaternion &rotation_out2) -> void {
        if (linearized_rotation) {
          geometry::hair_constraints::apply_position_bend_twist<true>(
              weight_rot1, weight_rot2, darboux_vector, alpha, lambda, rotation1, rotation2);
        }
        else {
          geometry::hair_constraints::apply_position_bend_twist<false>(
              weight_rot1, weight_rot2, darboux_vector, alpha, lambda, rotation1, rotation2);
        }
        lambda_out = lambda;
        rotation_out1 = rotation1;
        rotation_out2 = rotation2;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Stretch/Shear Rod Constraint", call_fn, param_tags);
}

template<typename ExecPreset> static auto contact_position_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float>,
                                           mf_output<float>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool /*linearized_rotation*/,
         float lambda,
         float3 position1,
         float3 position2,
         math::Quaternion rotation1,
         math::Quaternion rotation2,
         const float weight_pos1,
         const float weight_pos2,
         const float weight_rot1,
         const float weight_rot2,
         const float3 &local_position1,
         const float3 &local_position2,
         const float3 &normal,
         const float alpha,
         float &lambda_out,
         float3 &position1_out,
         float3 &position2_out,
         math::Quaternion &rotation1_out,
         math::Quaternion &rotation2_out) -> void {
        geometry::hair_constraints::apply_position_contact(weight_pos1,
                                                           weight_pos2,
                                                           weight_rot1,
                                                           weight_rot2,
                                                           local_position1,
                                                           local_position2,
                                                           normal,
                                                           alpha,
                                                           lambda,
                                                           position1,
                                                           position2,
                                                           rotation1,
                                                           rotation2);
        lambda_out = lambda;
        position1_out = position1;
        position2_out = position2;
        rotation1_out = rotation1;
        rotation2_out = rotation2;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Contact Position Constraint", call_fn, param_tags);
}

static const EnumPropertyItem rna_enum_constraint_type_items[] = {
    {int(ConstraintFunctionType::PositionGoal), "POSITION_GOAL", 0, "Position Goal", ""},
    {int(ConstraintFunctionType::RotationGoal), "ROTATION_GOAL", 0, "Rotation Goal", ""},
    {int(ConstraintFunctionType::VelocityGoal), "VELOCITY_GOAL", 0, "Velocity Goal", ""},
    {int(ConstraintFunctionType::AngularVelocityGoal),
     "ANGULAR_VELOCITY_GOAL",
     0,
     "Angular Velocity Goal",
     ""},
    {int(ConstraintFunctionType::StretchShear), "STRETCH_SHEAR", 0, "Stretch/Shear", ""},
    {int(ConstraintFunctionType::BendTwist), "BEND_TWIST", 0, "Bend/Twist", ""},
    {int(ConstraintFunctionType::ContactPosition), "CONTACT_POSITION", 0, "Contact Position", ""},
    {int(ConstraintFunctionType::ContactVelocity), "CONTACT_VELOCITY", 0, "Contact Velocity", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_default_layout();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const ConstraintFunctionType constraint_type = ConstraintFunctionType(node->custom1);

  /* XXX This should not really be a multifunction input but a fixed option. Using a bool input
   * here for convenience for the time being. */
  b.add_input<decl::Bool>("Linearized Rotation")
      .description("Compute rotation offset as linear offsets, only accurate for small angles.");

  switch (constraint_type) {
    case ConstraintFunctionType::PositionGoal:
      break;
    case ConstraintFunctionType::RotationGoal:
      break;
    case ConstraintFunctionType::VelocityGoal:
      break;
    case ConstraintFunctionType::AngularVelocityGoal:
      break;
    case ConstraintFunctionType::StretchShear:
      b.add_input<decl::Vector>("Lambda");
      b.add_output<decl::Vector>("Lambda").align_with_previous();
      b.add_input<decl::Vector>("Position 1").hide_value();
      b.add_output<decl::Vector>("Position 1").align_with_previous();
      b.add_input<decl::Vector>("Position 2").hide_value();
      b.add_output<decl::Vector>("Position 2").align_with_previous();
      b.add_input<decl::Rotation>("Rotation").hide_value();
      b.add_output<decl::Rotation>("Rotation").align_with_previous();
      b.add_separator();
      b.add_input<decl::Float>("Position Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Position Weight 2").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight").default_value(1.0f);
      b.add_separator();
      b.add_input<decl::Float>("Edge Length");
      b.add_input<decl::Float>("Alpha");
      break;
    case ConstraintFunctionType::BendTwist:
      b.add_input<decl::Vector>("Lambda");
      b.add_output<decl::Vector>("Lambda").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 1").hide_value();
      b.add_output<decl::Rotation>("Rotation 1").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 2").hide_value();
      b.add_output<decl::Rotation>("Rotation 2").align_with_previous();
      b.add_separator();
      b.add_input<decl::Vector>("Rotation Weight 1").default_value(float3(1.0f));
      b.add_input<decl::Vector>("Rotation Weight 2").default_value(float3(1.0f));
      b.add_separator();
      b.add_input<decl::Float>("Edge Length");
      b.add_input<decl::Vector>("Darboux Vector");
      b.add_input<decl::Vector>("Alpha");
      break;
    case ConstraintFunctionType::ContactPosition:
      b.add_input<decl::Float>("Lambda");
      b.add_output<decl::Float>("Lambda").align_with_previous();
      b.add_input<decl::Vector>("Position 1").hide_value();
      b.add_output<decl::Vector>("Position 1").align_with_previous();
      b.add_input<decl::Vector>("Position 2").hide_value();
      b.add_output<decl::Vector>("Position 2").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 1").hide_value();
      b.add_output<decl::Rotation>("Rotation 1").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 2").hide_value();
      b.add_output<decl::Rotation>("Rotation 2").align_with_previous();
      b.add_separator();
      b.add_input<decl::Float>("Position Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Position Weight 2").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight 2").default_value(1.0f);
      b.add_separator();
      b.add_input<decl::Vector>("Local Position 1");
      b.add_input<decl::Vector>("Local Position 2");
      b.add_input<decl::Vector>("Normal");
      b.add_input<decl::Float>("Alpha");
      break;
    case ConstraintFunctionType::ContactVelocity:
      break;
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "constraint_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(ConstraintFunctionType::PositionGoal);
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  namespace mf = fn::multi_function;

  const ConstraintFunctionType constraint_type = ConstraintFunctionType(bnode.custom1);
  static auto exec_preset = mf::build::exec_presets::AllSpanOrSingle();

  static auto fn_stretch_shear = stretch_shear_multifunction(exec_preset);
  static auto fn_bend_twist = bend_twist_multifunction(exec_preset);
  static auto fn_contact = contact_position_multifunction(exec_preset);

  switch (constraint_type) {
    case ConstraintFunctionType::PositionGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintFunctionType::RotationGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintFunctionType::VelocityGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintFunctionType::AngularVelocityGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintFunctionType::StretchShear:
      return &fn_stretch_shear;
    case ConstraintFunctionType::BendTwist:
      return &fn_bend_twist;
    case ConstraintFunctionType::ContactPosition:
      return &fn_contact;
    case ConstraintFunctionType::ContactVelocity:
      BLI_assert_unreachable();
      return nullptr;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "constraint_type",
                    "Constraint Type",
                    "",
                    rna_enum_constraint_type_items,
                    NOD_inline_enum_accessors(custom1),
                    int(ConstraintFunctionType::PositionGoal));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeHairConstraint");
  ntype.ui_name = "Evaluate Hair Constraint";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.build_multi_function = node_build_multi_function;
  ntype.draw_buttons = node_layout;
  bke::node_type_size(ntype, 200, 100, 300);
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}

}  // namespace constraint_function_nodes

static void node_register()
{
  position_goal::node_register();
  rotation_goal::node_register();
  stretch_shear::node_register();
  bend_twist::node_register();
  contact::node_register();
  constraint_function_nodes::node_register();
}
NOD_REGISTER_NODE(node_register)

}  // namespace xpbd_constraints

namespace hair_constraints {

/* -------------------------------------------------------------------- */
/** \name Constraint Bundle Access
 * \{ */

static const SocketInterfaceKey stretch_shear_key = SocketInterfaceKey("StretchShear");
static const SocketInterfaceKey bend_twist_key = SocketInterfaceKey("BendTwist");
static const SocketInterfaceKey position_goal_key = SocketInterfaceKey("PositionGoal");
static const SocketInterfaceKey rotation_goal_key = SocketInterfaceKey("RotationGoal");
static const SocketInterfaceKey contact_key = SocketInterfaceKey("Contact");

const SocketInterfaceKey &constraint_type_to_socket_key(const ConstraintType type)
{
  switch (type) {
    case ConstraintType::StretchShear:
      return stretch_shear_key;
    case ConstraintType::BendTwist:
      return bend_twist_key;
    case ConstraintType::PositionGoal:
      return position_goal_key;
    case ConstraintType::RotationGoal:
      return rotation_goal_key;
    case ConstraintType::Contact:
      return contact_key;
  }
  BLI_assert_unreachable();
  return stretch_shear_key;
}

ConstraintType socket_key_to_constraint_type(const SocketInterfaceKey &key)
{
  if (key.matches(stretch_shear_key)) {
    return ConstraintType::StretchShear;
  }
  if (key.matches(bend_twist_key)) {
    return ConstraintType::BendTwist;
  }
  if (key.matches(position_goal_key)) {
    return ConstraintType::PositionGoal;
  }
  if (key.matches(rotation_goal_key)) {
    return ConstraintType::RotationGoal;
  }
  if (key.matches(contact_key)) {
    return ConstraintType::Contact;
  }

  BLI_assert_unreachable();
  return ConstraintType::StretchShear;
}

void set_constraints(BundlePtr &bundle_ptr,
                     const ConstraintType type,
                     const bke::GeometrySet &geometry)
{
  BLI_assert(bundle_ptr->is_mutable());
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  static const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(
      SOCK_GEOMETRY);
  BLI_assert(geometry_type != nullptr);

  const SocketInterfaceKey &key = constraint_type_to_socket_key(type);
  bundle.remove(key);
  bundle.add(key, *geometry_type, &geometry);
}

bke::GeometrySet lookup_constraints(const Bundle &bundle, const ConstraintType type)
{
  static const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(
      SOCK_GEOMETRY);
  BLI_assert(geometry_type != nullptr);

  const SocketInterfaceKey &key = constraint_type_to_socket_key(type);
  const std::optional<Bundle::Item> value = bundle.lookup(key);
  if (!value) {
    return {};
  }
  GeometrySet output_geometry;
  if (!implicitly_convert_socket_value(
          *value->type, value->value, *geometry_type, &output_geometry))
  {
    return {};
  }
  return output_geometry;
};

BundlePtr combine_constraint_bundle(const ConstraintBundleItems &items)
{
  BundlePtr bundle_ptr = Bundle::create();

  set_constraints(bundle_ptr, ConstraintType::StretchShear, items.stretch_constraints);
  set_constraints(bundle_ptr, ConstraintType::BendTwist, items.bending_constraints);
  set_constraints(bundle_ptr, ConstraintType::PositionGoal, items.position_constraints);
  set_constraints(bundle_ptr, ConstraintType::RotationGoal, items.rotation_constraints);
  set_constraints(bundle_ptr, ConstraintType::Contact, items.contact_constraints);

  return bundle_ptr;
}

void separate_constraint_bundle(const Bundle &bundle, ConstraintBundleItems &items)
{
  items.stretch_constraints = lookup_constraints(bundle, ConstraintType::StretchShear);
  items.bending_constraints = lookup_constraints(bundle, ConstraintType::BendTwist);
  items.position_constraints = lookup_constraints(bundle, ConstraintType::PositionGoal);
  items.rotation_constraints = lookup_constraints(bundle, ConstraintType::RotationGoal);
  items.contact_constraints = lookup_constraints(bundle, ConstraintType::Contact);
}

/** \} */

}  // namespace hair_constraints

}  // namespace blender::nodes
