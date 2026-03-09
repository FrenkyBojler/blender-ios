/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"

#include "BKE_instances.hh"
#include "BKE_pointcloud.hh"

#include "GEO_join_geometries.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_points_to_instances {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Points")
      .supported_type(GeometryComponent::Type::PointCloud)
      .description("Points that are converted to empty instances");
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_output<decl::Geometry>("Instances").propagate_all();
}

static std::unique_ptr<bke::Instances> points_to_instances(
    GeometrySet &geometry_set,
    Field<bool> &selection_field,
    const NodeAttributeFilter &attribute_filter)
{
  const PointCloud *points = geometry_set.get_pointcloud();
  if (points == nullptr || points->totpoint == 0) {
    return {};
  }

  const bke::PointCloudFieldContext field_context{*points};
  fn::FieldEvaluator selection_evaluator{field_context, points->totpoint};
  selection_evaluator.add(selection_field);
  selection_evaluator.evaluate();
  const IndexMask selection = selection_evaluator.get_evaluated_as_mask(0);
  if (selection.is_empty()) {
    return {};
  }

  auto dst_instances = std::make_unique<bke::Instances>(selection.size());
  const int empty_reference_handle = dst_instances->add_new_reference(bke::InstanceReference());
  dst_instances->reference_handles_for_write().fill(empty_reference_handle);

  const bke::AttributeAccessor src_attributes = points->attributes();
  const VArraySpan positions = *src_attributes.lookup<float3>("position");
  MutableSpan<float4x4> dst_transforms = dst_instances->transforms_for_write();
  selection.foreach_index(
      [&](const int64_t i, const int64_t pos) {
        dst_transforms[pos] = math::from_location<float4x4>(positions[i]);
      },
      exec_mode::grain_size(4096));

  bke::MutableAttributeAccessor dst_attributes = dst_instances->attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, "instance_transform", ".reference_index")) {
      return;
    }
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    if (iter.is_builtin) {
      if (!dst_attributes.is_builtin(iter.name)) {
        return;
      }
    }
    const bke::GAttributeReader src = iter.get(bke::AttrDomain::Point);
    if (!src) {
      /* Domain interpolation can fail if the source domain is empty. */
      return;
    }
    const CommonVArrayInfo info = src.varray.common_info();
    if (info.type == CommonVArrayInfo::Type::Single) {
      const bke::AttributeInitValue init(GPointer(src.varray.type(), info.data));
      dst_attributes.add(iter.name, bke::AttrDomain::Instance, iter.data_type, init);
      return;
    }
    if (info.type == CommonVArrayInfo::Type::Span) {
      if (src.sharing_info && selection.size() == points->totpoint) {
        const bke::AttributeInitShared init(info.data, *src.sharing_info);
        dst_attributes.add(iter.name, AttrDomain::Instance, iter.data_type, init);
        return;
      }
    }
    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        iter.name, bke::AttrDomain::Instance, iter.data_type);
    array_utils::gather(src.varray, selection, dst.span);
    dst.finish();
  });

  return dst_instances;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Instances");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &sub_geometry) {
    Vector<bke::GeometrySet> component_instances;
    if (std::unique_ptr<bke::Instances> instances = points_to_instances(
            sub_geometry, selection_field, attribute_filter))
    {
      component_instances.append(GeometrySet::from_instances(std::move(instances)));
    }
    GeometrySet dst_instances = geometry::join_geometries(component_instances, attribute_filter);

    sub_geometry.keep_only({GeometryComponent::Type::Edit});
    sub_geometry.replace_instances(
        dst_instances.get_component_for_write<InstancesComponent>().release());
  });

  params.set_output("Instances", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodePointsToInstances");
  ntype.ui_name = "Points to Instances";
  ntype.ui_description = "Generate an empty instance for each point";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_points_to_instances
