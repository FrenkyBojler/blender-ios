/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"

#include "GEO_join_geometries.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_instance_on_points_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Points").description("Points to instance on");
  b.add_input<decl::Bool>("Selection").default_value(true).field_on({0}).hide_value();
  b.add_input<decl::Geometry>("Instance").description("Geometry that is instanced on the points");
  b.add_input<decl::Bool>("Pick Instance")
      .field_on({0})
      .description(
          "Choose instances from the \"Instance\" input at each point instead of instancing the "
          "entire geometry");
  b.add_input<decl::Int>("Instance Index")
      .implicit_field_on(implicit_field_inputs::id_or_index, {0})
      .description(
          "Index of the instance used for each point. This is only used when Pick Instances "
          "is on. By default the point index is used");
  b.add_input<decl::Rotation>("Rotation").field_on({0}).description("Rotation of the instances");
  b.add_input<decl::Vector>("Scale")
      .default_value({1.0f, 1.0f, 1.0f})
      .subtype(PROP_XYZ)
      .field_on({0})
      .description("Scale of the instances");

  b.add_output<decl::Geometry>("Instances").propagate_all();
}

static IndexMask existed_layers_mask(IndexMaskMemory &memory, const GreasePencil &grease_pencil)
{
  using namespace bke::greasepencil;
  return IndexMask::from_predicate(
      grease_pencil.layers().index_range(), GrainSize(4096), memory, [&](const int layer_index) {
        const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
        if (drawing == nullptr) {
          return false;
        }
        const bke::CurvesGeometry &src_curves = drawing->strokes();
        return !src_curves.is_empty();
      });
}

static void wrap_indices(const int total, MutableSpan<int> indices)
{
  threading::parallel_for(indices.index_range(), 4096 * 4, [&](const IndexRange range) {
    for (const int i : range) {
      indices[i] = math::mod_periodic<int>(indices[i], total);
    }
  });
}

static void loc_rot_scale_to_transform(const VArray<float3> positions,
                                       const VArray<math::Quaternion> &rotations,
                                       const VArray<float3> &scales,
                                       const IndexMask &mask,
                                       MutableSpan<float4x4> transformations)
{
  mask.foreach_index(GrainSize(4096), [&](const int src_index, const int dst_pos) {
    transformations[dst_pos] = math::from_loc_rot_scale<float4x4>(
        positions[src_index], rotations[src_index], scales[src_index]);
  });
}

static void apply_transform_masked(const Span<float4x4> parent_transform,
                                   const Span<float4x4> child_transform,
                                   const IndexMask &mask,
                                   const Span<int> indices,
                                   MutableSpan<float4x4> dst_transform)
{
  BLI_assert(mask.size() == parent_transform.size());
  BLI_assert(mask.size() == dst_transform.size());
  BLI_assert(mask.min_array_size() <= indices.size());
  mask.foreach_index(GrainSize(4096), [&](const int index, const int pos) {
    const float4x4 child = child_transform[indices[index]];
    const float4x4 parent = parent_transform[pos];
    dst_transform[pos] = parent * child;
  });
}

static void apply_transform_masked(const Span<float4x4> parent_transform,
                                   const float4x4 child_transform,
                                   const IndexMask &mask,
                                   MutableSpan<float4x4> dst_transform)
{
  BLI_assert(parent_transform.size() == dst_transform.size());
  BLI_assert(parent_transform.size() == mask.size());
  mask.foreach_index(GrainSize(4096), [&](const int /*index*/, const int pos) {
    dst_transform[pos] = parent_transform[pos] * child_transform;
  });
}

static void indices_to_index_of(const Span<int> keys,
                                const VectorSet<int> set,
                                MutableSpan<int> indices)
{
  BLI_assert(keys.size() == indices.size());
  threading::parallel_for(keys.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      indices[i] = set.index_of(keys[i]);
    }
  });
}

static GeometrySet instances_on_domain(const VArray<bool> &pick_instances,
                                       const VArray<int> &pick_indices,
                                       const VArray<float3> &positions,
                                       const VArray<math::Quaternion> &rotations,
                                       const VArray<float3> &scales,
                                       const IndexMask &selection,
                                       const GeometrySet &geometry_to_instance,
                                       bool &ignore_realized_data)
{
  Array<bool> selected_pick_instances(selection.size());
  array_utils::gather(pick_instances, selection, selected_pick_instances.as_mutable_span());

  /* Input target domain to fill by indices have two kind of selection: user-provided selection
  which is define part of data to copy. Second one is split of selection into picked and non-picked
  parts. In order to reduce change of fragmentation and even be able trivially copy attributes or
  even share them, order have to be maintained. */

  IndexMaskMemory memory;
  const IndexMask selected_picked_mask = IndexMask::from_bools(selected_pick_instances, memory);
  const IndexMask selected_non_picked_mask = selected_picked_mask.complement(
      selection.index_range(), memory);

  {
    const bool all_instances_picked = selected_non_picked_mask.is_empty();
    const bool some_geometry_can_not_be_picked = geometry_to_instance.has_realized_data();
    ignore_realized_data |= all_instances_picked && some_geometry_can_not_be_picked;
  }

  bke::Instances *dst_instances = new bke::Instances();
  dst_instances->resize(selection.size());
  MutableSpan<int> dst_handles = dst_instances->reference_handles_for_write();
  MutableSpan<float4x4> dst_transforms = dst_instances->transforms_for_write();

  loc_rot_scale_to_transform(positions, rotations, scales, selection, dst_transforms);

  if (!selected_non_picked_mask.is_empty()) {
    const int single_handler_index = dst_instances->add_reference(geometry_to_instance);
    index_mask::masked_fill<int>(dst_handles, single_handler_index, selected_non_picked_mask);
  }

  if (selected_picked_mask.is_empty()) {
    return GeometrySet::from_instances(dst_instances);
  }

  const bke::Instances *instances = geometry_to_instance.get_instances();
  if (instances == nullptr) {
    const int empty_handler_index = dst_instances->add_reference(GeometrySet());
    index_mask::masked_fill<int>(dst_handles, empty_handler_index, selected_picked_mask);
    return GeometrySet::from_instances(dst_instances);
  }

  const Span<bke::InstanceReference> instance_handlers = instances->references();
  const Span<int> instance_handler_indices = instances->reference_handles();
  const Span<float4x4> instance_transforms = instances->transforms();
  const int instances_num = instances->instances_num();
  if (instance_handlers.is_empty()) {
    const int empty_handler_index = dst_instances->add_reference(GeometrySet());
    index_mask::masked_fill<int>(dst_handles, empty_handler_index, selected_picked_mask);
    return GeometrySet::from_instances(dst_instances);
  }

  const IndexMask picked_mask = IndexMask::from_bools(selection, pick_instances, memory);
  BLI_assert(selected_picked_mask.size() == picked_mask.size());

  Array<int> gathered_pick_indices_data;
  VectorSet<int> unique_pick_instances;
  if (const std::optional<int> pick_index = pick_indices.get_if_single()) {
    const int wrapped_index = math::mod_periodic<int>(*pick_index, instances_num);
    unique_pick_instances.add(wrapped_index);
  }
  else {
    gathered_pick_indices_data.reinitialize(picked_mask.size());
    pick_indices.materialize_compressed(picked_mask, gathered_pick_indices_data.as_mutable_span());
    wrap_indices(instances_num, gathered_pick_indices_data.as_mutable_span());
    unique_pick_instances = VectorSet<int>(gathered_pick_indices_data.as_span());
    indices_to_index_of(gathered_pick_indices_data.as_span(),
                        unique_pick_instances,
                        gathered_pick_indices_data.as_mutable_span());
  }

  Array<float4x4> unique_instance_transform(unique_pick_instances.size());
  array_utils::gather(instance_transforms,
                      unique_pick_instances.as_span(),
                      unique_instance_transform.as_mutable_span());

  BLI_assert(!unique_pick_instances.is_empty());
  if (unique_pick_instances.size() == 1) {
    const float4x4 &instance_transform = unique_instance_transform.first();
    apply_transform_masked(
        dst_transforms.as_span(), instance_transform, selected_picked_mask, dst_transforms);
  }
  else {
    apply_transform_masked(dst_transforms.as_span(),
                           unique_instance_transform.as_span(),
                           selected_picked_mask,
                           gathered_pick_indices_data.as_span(),
                           dst_transforms);
  }

  if (unique_pick_instances.size() == 1) {
    const int single_picked_instance = unique_pick_instances[0];
    const int picked_handler_index = instance_handler_indices[single_picked_instance];
    const bke::InstanceReference &handle_reference = instance_handlers[picked_handler_index];
    const int single_handler_index = dst_instances->add_reference(handle_reference);
    index_mask::masked_fill<int>(dst_handles, single_handler_index, selected_picked_mask);
  }
  else {
    Array<int> handle_unique_picked_instances(unique_pick_instances.size());
    array_utils::copy(unique_pick_instances.as_span(),
                      handle_unique_picked_instances.as_mutable_span());
    array_utils::gather(instance_handler_indices,
                        handle_unique_picked_instances.as_span(),
                        handle_unique_picked_instances.as_mutable_span());

    for (const int i : handle_unique_picked_instances.index_range()) {
      const int unique_handler_index = handle_unique_picked_instances[i];
      const bke::InstanceReference &handle_reference = instance_handlers[unique_handler_index];
      const int handler_index = dst_instances->add_reference(handle_reference);
      handle_unique_picked_instances[i] = handler_index;
    }

    array_utils::gather(handle_unique_picked_instances.as_span(),
                        gathered_pick_indices_data.as_span(),
                        gathered_pick_indices_data.as_mutable_span());
    array_utils::scatter<int>(
        gathered_pick_indices_data.as_span(), selected_picked_mask, dst_handles);
  }

  return GeometrySet::from_instances(dst_instances);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  GeometrySet geometry_to_instance = params.get_input<GeometrySet>("Instance");
  geometry_to_instance.ensure_owns_direct_data();
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Instances");

  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<bool> pick_instances_field = params.extract_input<Field<bool>>("Pick Instance");
  const Field<int> indices_field = params.extract_input<Field<int>>("Instance Index");
  const Field<math::Quaternion> rotations_field = params.extract_input<Field<math::Quaternion>>(
      "Rotation");
  const Field<float3> scales_field = params.extract_input<Field<float3>>("Scale");

  std::atomic<bool> has_skiped_realized_instances = {false};

  const auto instances_on_component =
      [&](const bke::AttributeAccessor src_attributes,
          const fn::FieldContext &field_context,
          const Map<StringRef, AttributeDomainAndType> &attributes_to_propagate) {
        const int domain_size = src_attributes.domain_size(AttrDomain::Point);
        fn::FieldEvaluator evaluator(field_context, domain_size);

        evaluator.set_selection(selection_field);
        evaluator.add(pick_instances_field);
        evaluator.add(indices_field);
        evaluator.add(rotations_field);
        evaluator.add(scales_field);

        evaluator.evaluate();

        const VArray<bool> pick_instance = evaluator.get_evaluated<bool>(0);
        const VArray<int> pick_indices = evaluator.get_evaluated<int>(1);
        const VArray<math::Quaternion> rotations = evaluator.get_evaluated<math::Quaternion>(2);
        const VArray<float3> scales = evaluator.get_evaluated<float3>(3);
        const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

        if (selection.is_empty()) {
          return GeometrySet();
        }

        const VArray<float3> positions = *src_attributes.lookup<float3>("position",
                                                                        AttrDomain::Point);

        bool ignore_realized_data = false;
        GeometrySet instanced_geometry = instances_on_domain(pick_instance,
                                                             pick_indices,
                                                             positions,
                                                             rotations,
                                                             scales,
                                                             selection,
                                                             geometry_to_instance,
                                                             ignore_realized_data);

        bke::Instances &instances_component = *instanced_geometry.get_instances_for_write();

        /* This force attribute propagation from any source domain via point domain to instances.
         */
        bke::MutableAttributeAccessor dst_attributes = instances_component.attributes_for_write();
        for (const auto item : attributes_to_propagate.items()) {
          const StringRef id = item.key;
          const eCustomDataType data_type = item.value.data_type;
          if (data_type == CD_PROP_STRING) {
            continue;
          }
          const bke::GAttributeReader src = src_attributes.lookup(
              id, AttrDomain::Point, data_type);
          if (!src) {
            continue;
          }

          if (selection.size() == domain_size && src.varray.size() == domain_size &&
              src.sharing_info && src.varray.is_span())
          {
            const bke::AttributeInitShared init(src.varray.get_internal_span().data(),
                                                *src.sharing_info);
            dst_attributes.add(id, AttrDomain::Instance, data_type, init);
            continue;
          }

          bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
              id, AttrDomain::Instance, data_type);
          if (!src) {
            continue;
          }

          array_utils::gather(*src, selection, dst.span);
          dst.finish();
        }

        if (ignore_realized_data) {
          has_skiped_realized_instances = true;
        }
        return instanced_geometry;
      };

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    /* It's important not to invalidate the existing #InstancesComponent because it owns references
     * to other geometry sets that are processed by this node. */
    InstancesComponent &instances_component =
        geometry_set.get_component_for_write<InstancesComponent>();

    Vector<GeometrySet, 4> instances_set;
    if (const bke::Instances *dst_instances = instances_component.get()) {
      bke::Instances *copy = new bke::Instances(*dst_instances);
      instances_set.append(
          GeometrySet::from_instances(copy));
    }

    static const Array<GeometryComponent::Type> types{GeometryComponent::Type::Mesh,
                                                      GeometryComponent::Type::PointCloud,
                                                      GeometryComponent::Type::Curve};

    Map<StringRef, AttributeDomainAndType> attributes_to_propagate;
    geometry_set.gather_attributes_for_propagation(types,
                                                   GeometryComponent::Type::Instance,
                                                   false,
                                                   attribute_filter,
                                                   attributes_to_propagate);
    attributes_to_propagate.remove("position");
    attributes_to_propagate.remove(".reference_index");

    for (const GeometryComponent::Type type : types) {
      if (!geometry_set.has(type)) {
        continue;
      }
      const GeometryComponent &component = *geometry_set.get_component(type);
      const bke::AttributeAccessor src_attributes = *component.attributes();
      const bke::GeometryFieldContext field_context(component, AttrDomain::Point);
      instances_set.append(
          instances_on_component(src_attributes, field_context, attributes_to_propagate));
    }

    GeometrySet new_instances = geometry::join_geometries(instances_set.as_span(),
                                                          attribute_filter);
    instances_component.replace(
        new_instances.get_component_for_write<InstancesComponent>().release());

    geometry_set.remove_geometry_during_modify();
  });

  if (has_skiped_realized_instances) {
    params.error_message_add(NodeWarningType::Info,
                             TIP_("Realized geometry is not used when pick instances is true"));
  }

  params.set_output("Instances", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInstanceOnPoints", GEO_NODE_INSTANCE_ON_POINTS);
  ntype.ui_name = "Instance on Points";
  ntype.ui_description =
      "Generate a reference to geometry at each of the input points, without duplicating its "
      "underlying data";
  ntype.enum_name_legacy = "INSTANCE_ON_POINTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_instance_on_points_cc
