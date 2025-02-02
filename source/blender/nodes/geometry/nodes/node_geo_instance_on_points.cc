/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

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
static void wrap_indices(const int total, MutableSpan<int> indices)
{
  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      indices[i] = mod_i(indices[i], total);
    }
  });
}

static void fill_by_single_instance(const GeometrySet &instance,
                                    const float4x4 &transform,
                                    const IndexMask &mask,
                                    bke::Instances &instances)
{
  index_mask::masked_fill<int>(
      instances.reference_handles_for_write(), instances.add_reference(instance), mask);
  index_mask::masked_fill<float4x4>(instances.transforms_for_write(), transform, mask);
}

static IndexMask from_bools_in_local(const IndexMask &universe, const VArray<bool> &bools, IndexMaskMemory &memory)
{
  if (bools.is_single()) {
    if (bools.get_internal_single()) {
      return universe.index_range();
    }
    return {};
  }

  Array<int, 0> i_to_index;
  const std::optional<IndexRange> universe_range = universe.to_range();
  if (!universe_range.has_value()) {
    i_to_index.reinitialize(universe.size());
    universe.to_indices(i_to_index.as_mutable_span());
  }

  if (bools.is_span()) {
    const Span<bool> as_span = bools.get_internal_span();
    if (universe_range.has_value()) {
      const IndexRange range = *universe_range;
      return IndexMask::from_predicate(selection.index_range(), GrainSize(4096), memory, [&](const int64_t index) {
        return as_span[range[index]];
      });
    }
    return IndexMask::from_predicate(selection.index_range(), GrainSize(4096), memory, [&](const int64_t index) {
      return as_span[i_to_index[index]];
    });
  }

  if (universe_range.has_value()) {
    const IndexRange range = *universe_range;
    return IndexMask::from_predicate(selection.index_range(), GrainSize(4096), memory, [&](const int64_t index) {
      return bools[range[index]];
    });
  }

  return IndexMask::from_predicate(selection.index_range(), GrainSize(4096), memory, [&](const int64_t index) {
    return bools[i_to_index[index]];
  });
}

static void set_loc_rot_scale(const VArray<float3> positions,
                             const VArray<math::Quaternion> &rotations,
                             const VArray<float3> &scales,
                             const IndexMask &mask,
                             MutableSpan<float4x4> transformations)
{
  mask.foreach_index(GrainSize(4096), [&](const int src_index, const int dst_pos) {
    transformations[dst_pos] = math::from_loc_rot_scale<float4x4>(positions[src_index], rotations[src_index], scales[src_index]);
  });
}

static void apply_loc_rot_scale(const VArray<float3> positions,
                                const VArray<math::Quaternion> &rotations,
                                const VArray<float3> &scales,
                                const IndexMask &mask,
                                MutableSpan<float4x4> transformations)
{
  mask.foreach_index(GrainSize(4096), [&](const int src_index, const int dst_pos) {
    const float4x4 to_apply = math::from_loc_rot_scale<float4x4>(positions[src_index], rotations[src_index], scales[src_index]);
    transformations[dst_pos] = to_apply * transformations[dst_pos];
  });
}

static GeometrySet instances_on_domain(const VArray<bool> &pick_instance,
                                       const VArray<int> &indices,
                                       const VArray<float3> &positions,
                                       const VArray<math::Quaternion> &rotations,
                                       const VArray<float3> &scales
                                       const IndexMask &selection,
                                       const GeometrySet &geometry_to_instance,
                                       bool &ignore_realized_data)
{
  IndexMaskMemory memory;
  const IndexMask selected_picked_mask = from_bools_in_local(selection, memory, pick_instance);
  const IndexMask selected_non_picked_mask = IndexMask(selection.index_range()).complement(selected_picked_mask, memory);

  ignore_realized_data |= selected_picked_mask.is_empty() && geometry_to_instance.has_realized_data();

  bke::Instances *dst_instances = new bke::Instances();
  dst_instances->resize(selection.size());
  MutableSpan<int> dst_handles = dst_instances->reference_handles_for_write();
  MutableSpan<float4x4> dst_transforms = dst_instances->transforms_for_write();

  if (!selected_picked_mask.is_empty()) {
    const int single_handler_index = dst_instances->add_reference(geometry_to_instance);
    index_mask::masked_fill<int>(dst_handles, single_handler_index, selected_picked_mask);
    set_loc_rot_scale(positions, rotations, scales, selected_picked_mask, );
  }

  if (mapped_instance_selection.is_empty()) {
    return GeometrySet::from_instances(dst_instances);
  }

  const bke::Instances *src_instances = geometry_to_instance.get_instances();
  if (src_instances == nullptr) {
    const int empty_handler_index = dst_instances->add_reference(GeometrySet());
    index_mask::masked_fill<int>(dst_handles, empty_handler_index, mapped_instance_selection);
    return GeometrySet::from_instances(dst_instances);
  }

  const int instances_num = src_instances->instances_num();
  const Span<bke::InstanceReference> src_instance_handlers = src_instances->references();
  const Span<int> src_handles = src_instances->reference_handles();
  const Span<float4x4> src_transforms = src_instances->transforms();
  if (src_handles.is_empty()) {
    index_mask::masked_fill<int>(geometry_to_instance.reference_handles_for_write(),
                                 geometry_to_instance.add_reference(instance),
                                 mask);
    fill_by_single_instance(
        GeometrySet(), float4x4::identity(), mapped_instance_selection, *dst_instances);
    return GeometrySet::from_instances(dst_instances);
  }

  Array<int> gathered_indices(mapped_instance_selection.size());
  indices.materialize_compressed(mapped_instance_selection, gathered_indices.as_mutable_span());
  wrap_indices(instances_num, gathered_indices.as_mutable_span());

  Array<int> mapped_to_all_mapping(selection.min_array_size(), -1);
  index_mask::build_reverse_map(selection, mapped_to_all_mapping.as_mutable_span());
  mapped_instance_selection.foreach_index_optimized<int>(
      GrainSize(4096), [&](const int i, const int pos) {
        dst_transforms[mapped_to_all_mapping[i]] = src_transforms[gathered_indices[pos]];
      });

  array_utils::gather(src_handles, gathered_indices.as_span(), gathered_indices.as_mutable_span());
  VectorSet<int> unique_handlers(gathered_indices.as_span());
  threading::parallel_for(gathered_indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      gathered_indices[i] = unique_handlers.index_of(gathered_indices[i]);
    }
  });

  Array<int> unique_handlers_indices(unique_handlers.size());
  for (const int unique_handler_i : unique_handlers.index_range()) {
    const int unique_handler_index = unique_handlers[unique_handler_i];
    const bke::InstanceReference &reference = src_instance_handlers[unique_handler_index];
    unique_handlers_indices[unique_handler_i] = dst_instances->add_reference(reference);
  }

  array_utils::gather(unique_handlers_indices.as_span(),
                      gathered_indices.as_span(),
                      gathered_indices.as_mutable_span());
  // array_utils::scatter(gathered_indices.as_span(), mapped_instance_selection, dst_handles);

  mapped_instance_selection.foreach_index_optimized<int>(
      GrainSize(4096), [&](const int i, const int pos) {
        dst_handles[mapped_to_all_mapping[i]] = gathered_indices[pos];
      });

  return GeometrySet::from_instances(dst_instances);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  GeometrySet instance = params.get_input<GeometrySet>("Instance");
  instance.ensure_owns_direct_data();
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Instances");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    /* It's important not to invalidate the existing #InstancesComponent because it owns references
     * to other geometry sets that are processed by this node. */
    InstancesComponent &instances_component =
        geometry_set.get_component_for_write<InstancesComponent>();
    bke::Instances *dst_instances = instances_component.get_for_write();
    if (dst_instances == nullptr) {
      dst_instances = new bke::Instances();
      instances_component.replace(dst_instances);
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
      if (geometry_set.has(type)) {
        const GeometryComponent &component = *geometry_set.get_component(type);
        const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
        add_instances_from_component(*dst_instances,
                                     *component.attributes(),
                                     instance,
                                     field_context,
                                     params,
                                     attributes_to_propagate);
      }
    }
    if (geometry_set.has_grease_pencil()) {
      using namespace bke::greasepencil;
      const GreasePencil &grease_pencil = *geometry_set.get_grease_pencil();
      bke::Instances *instances = new bke::Instances();
      for (const int layer_index : grease_pencil.layers().index_range()) {
        const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
        if (drawing == nullptr) {
          continue;
        }
        const bke::CurvesGeometry &src_curves = drawing->strokes();
        if (src_curves.is_empty()) {
          /* Add an empty reference so the number of layers and instances match.
           * This makes it easy to reconstruct the layers afterwards and keep their attributes.
           * Although in this particular case we don't propagate the attributes. */
          const int handle = instances->add_reference(bke::InstanceReference());
          instances->add_instance(handle, float4x4::identity());
          continue;
        }
        /* TODO: Attributes are not propagating from the curves or the points. */
        bke::Instances *layer_instances = new bke::Instances();
        const bke::GreasePencilLayerFieldContext field_context(
            grease_pencil, AttrDomain::Point, layer_index);
        add_instances_from_component(*layer_instances,
                                     src_curves.attributes(),
                                     instance,
                                     field_context,
                                     params,
                                     attributes_to_propagate);
        GeometrySet temp_set = GeometrySet::from_instances(layer_instances);
        const int handle = instances->add_reference(bke::InstanceReference{temp_set});
        instances->add_instance(handle, float4x4::identity());
      }

      bke::copy_attributes(geometry_set.get_grease_pencil()->attributes(),
                           bke::AttrDomain::Layer,
                           bke::AttrDomain::Instance,
                           attribute_filter,
                           instances->attributes_for_write());
      GeometrySet new_instances = geometry::join_geometries(
          {GeometrySet::from_instances(dst_instances, bke::GeometryOwnershipType::Editable),
           GeometrySet::from_instances(instances)},
          attribute_filter);
      instances_component.replace(
          new_instances.get_component_for_write<InstancesComponent>().release());

      geometry_set.replace_grease_pencil(nullptr);
    }
    geometry_set.remove_geometry_during_modify();
  });

  /* Unused references may have been added above. Remove those now so that other nodes don't
   * process them needlessly.
   * This should eventually be moved into the loop above, but currently this is quite tricky
   * because it might remove references that the loop still wants to iterate over. */
  if (bke::Instances *instances = geometry_set.get_instances_for_write()) {
    instances->remove_unused_references();
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
