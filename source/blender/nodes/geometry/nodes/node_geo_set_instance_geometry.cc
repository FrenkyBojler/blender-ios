/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.h"
#include "BLI_span.hh"
#include "BLI_task.hh"

#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_instance_geometry {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Instances").only_instances();
  b.add_output<decl::Geometry>("Instances").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Geometry>("Geometry").description("TODO");
  b.add_input<decl::Bool>("Pick Instance")
      .field_on({0})
      .description(
          "Choose instanced geometry from the \"Geometry\" input for each instance instead of the "
          "entire geometry");
  b.add_input<decl::Int>("Instance Index")
      .implicit_field_on(NODE_DEFAULT_INPUT_ID_INDEX_FIELD, {0})
      .description("TODO");
}

static void set_instance_geometry_from_instances(GeometrySet &src_geometry,
                                                 Field<bool> &selection_field,
                                                 Field<bool> &pick_instance_field,
                                                 Field<int> &instance_index_field,
                                                 bke::Instances &dst_instances)
{
  VArray<bool> pick_instance;
  VArray<int> indices;
  const bke::InstancesFieldContext context{dst_instances};
  fn::FieldEvaluator evaluator{context, dst_instances.instances_num()};
  evaluator.set_selection(selection_field);
  evaluator.add(pick_instance_field, &pick_instance);
  evaluator.add(instance_index_field, &indices);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  if (selection.is_empty()) {
    return;
  }

  const int full_instance_handle = dst_instances.add_reference(src_geometry);
  const int empty_reference_handle = dst_instances.add_reference(bke::InstanceReference());

  const bke::Instances &src_instances = *src_geometry.get_instances();

  const int src_instances_num = src_instances.instances_num();
  const Span<int> src_handles = src_instances.reference_handles();
  const Span<bke::InstanceReference> src_references = src_instances.references();
  const Span<float4x4> src_transforms = src_instances.transforms();
  Array<int> handle_mapping(src_handles.size());
  if (!pick_instance.is_single() || pick_instance.get_internal_single()) {
    for (const int src_instance_handle : src_references.index_range()) {
      const bke::InstanceReference &reference = src_references[src_instance_handle];
      const int dst_instance_handle = dst_instances.add_reference(reference);
      handle_mapping[src_instance_handle] = dst_instance_handle;
    }
  }

  MutableSpan<int> dst_handles = dst_instances.reference_handles_for_write();
  MutableSpan<float4x4> dst_transforms = dst_instances.transforms_for_write();
  selection.foreach_index(
      [&](const int64_t i) {
        int dst_handle = empty_reference_handle;
        if (pick_instance[i]) {
          const int original_index = indices[i];
          /* Use #mod_i instead of `%` to get the desirable wrap around behavior where -1
           * refers to the last element. */
          const int index = mod_i(original_index, std::max(src_instances_num, 1));
          if (index < src_instances_num) {
            const int src_handle = src_handles[index];
            dst_handle = handle_mapping[src_handle];
            /* Take transforms of the source instance into account. */
            mul_m4_m4_post(dst_transforms[i].ptr(), src_transforms[index].ptr());
          }
        }
        else {
          dst_handle = full_instance_handle;
        }
        dst_handles[i] = dst_handle;
      },
      exec_mode::grain_size(4096));
  dst_instances.remove_unused_references();
}

static void set_instance_geometry_from_geometry_set(GeometrySet &geometry_set,
                                                    Field<bool> &selection_field,
                                                    bke::Instances &dst_instances)
{
  const bke::InstancesFieldContext context{dst_instances};
  fn::FieldEvaluator evaluator{context, dst_instances.instances_num()};
  evaluator.set_selection(selection_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  if (selection.is_empty()) {
    return;
  }

  const int new_handle = dst_instances.add_new_reference(bke::InstanceReference(geometry_set));
  MutableSpan<int> dst_handles = dst_instances.reference_handles_for_write();
  index_mask::masked_fill(dst_handles, new_handle, selection);
  dst_instances.remove_unused_references();
}

static void set_instance_geometry(GeometrySet &geometry_set,
                                  Field<bool> selection_field,
                                  Field<bool> pick_instance_field,
                                  Field<int> instance_index_field,
                                  bke::Instances &dst_instances)
{
  if (geometry_set.has_instances()) {
    set_instance_geometry_from_instances(
        geometry_set, selection_field, pick_instance_field, instance_index_field, dst_instances);
  }
  else {
    set_instance_geometry_from_geometry_set(geometry_set, selection_field, dst_instances);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Instances");
  GeometrySet instance_geometry = params.extract_input<GeometrySet>("Geometry");

  if (bke::Instances *instances = geometry_set.get_instances_for_write()) {
    set_instance_geometry(instance_geometry,
                          params.extract_input<Field<bool>>("Selection"),
                          params.extract_input<Field<bool>>("Pick Instance"),
                          params.extract_input<Field<int>>("Instance Index"),
                          *instances);
  }

  params.set_output("Instances", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetInstanceGeometry");
  ntype.ui_name = "Set Instance Geometry";
  ntype.ui_description = "Replace the geometry referenced by the instances";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_type_size(ntype, 160, 100, 700);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_instance_geometry
