/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"

#include "GEO_join_geometries.hh"
#include "GEO_realize_instances.hh"

#include "BKE_instances.hh"

namespace blender::geometry {

using bke::AttributeDomainAndType;
using bke::GeometryComponent;
using bke::GeometrySet;

void join_attributes(const Span<std::optional<bke::AttributeAccessor>> attribute_accessors,
                     const Map<StringRef, eCustomDataType> &attribute_types,
                     const bke::AttrDomain src_domain,
                     const bke::AttrDomain dst_domain,
                     bke::MutableAttributeAccessor dst_attributes)
{
  Array<int> src_offsets_data(attribute_accessors.size() + 1);
  for (const int i : attribute_accessors.index_range()) {
    src_offsets_data[i] = attribute_accessors[i]->domain_size(src_domain);
  }
  const OffsetIndices<int> src_offsets = offset_indices::accumulate_counts_to_offsets(
      src_offsets_data);

  for (const MapItem<StringRef, eCustomDataType> item : attribute_types.items()) {
    const StringRef attribute_id = item.key;
    const eCustomDataType data_type = item.value;

    bke::GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, dst_domain, data_type);
    if (!dst_attribute) {
      continue;
    }

    threading::memory_bandwidth_bound_task(dst_attribute.span.size_in_bytes(), [&]() {
      threading::parallel_for(attribute_accessors.index_range(), 1024 * 16, [&](const IndexRange range) {
        for (const int i : range) {
          const bke::GAttributeReader src_attribute = attribute_accessors[i]->lookup(attribute_id, src_domain, data_type);
          if (!src_attribute) {
            GMutableSpan dst_range = dst_attribute.span.slice(src_offsets[i]);
            const CPPType &type = dst_range.type();
            type.fill_assign_n(type.default_value(), dst_range.data(), dst_range.size());
            continue;
          }

          array_utils::copy(src_attribute.varray, dst_attribute.span.slice(src_offsets[i]));
        }
      }, threading::accumulated_task_sizes([&](const IndexRange range) { return src_offsets[range].size(); }));
    });

    dst_attribute.finish();
  }
}

static void join_instances(const Span<const GeometryComponent *> src_components,
                           const bke::AttributeFilter &attribute_filter,
                           GeometrySet &result)
{
  Array<int> offsets_data(src_components.size() + 1);
  for (const int i : src_components.index_range()) {
    const auto &src_component = static_cast<const bke::InstancesComponent &>(*src_components[i]);
    offsets_data[i] = src_component.get()->instances_num();
  }
  const OffsetIndices offsets = offset_indices::accumulate_counts_to_offsets(offsets_data);

  std::unique_ptr<bke::Instances> dst_instances = std::make_unique<bke::Instances>();
  dst_instances->resize(offsets.total_size());

  MutableSpan<int> all_handles = dst_instances->reference_handles_for_write();

  Map<std::reference_wrapper<const bke::InstanceReference>, int> new_handle_by_src_reference;

  for (const int i : src_components.index_range()) {
    const auto &src_component = static_cast<const bke::InstancesComponent &>(*src_components[i]);
    const bke::Instances &src_instances = *src_component.get();

    const Span<bke::InstanceReference> src_references = src_instances.references();
    Array<int> handle_map(src_references.size());
    for (const int src_handle : src_references.index_range()) {
      const bke::InstanceReference &src_reference = src_references[src_handle];
      handle_map[src_handle] = new_handle_by_src_reference.lookup_or_add_cb(
          src_reference, [&]() { return dst_instances->add_new_reference(src_reference); });
    }

    const IndexRange dst_range = offsets[i];

    const Span<int> src_handles = src_instances.reference_handles();
    array_utils::gather(handle_map.as_span(), src_handles, all_handles.slice(dst_range));
  }

  result.replace_instances(dst_instances.release());
  auto &dst_component = result.get_component_for_write<bke::InstancesComponent>();

  Array<std::optional<bke::AttributeAccessor>> all_attributes(src_components.size());
  for (const int i : src_components.index_range()) {
    all_attributes[i] = *src_components[i]->attributes();
  }
  join_attributes(all_attributes,
                  get_final_attribute_types(
                      all_attributes,
                      bke::attribute_filter_with_skip_ref(attribute_filter, {".reference_index"})),
                  bke::AttrDomain::Instance,
                  bke::AttrDomain::Instance,
                  *dst_component.attributes_for_write());
}

static void join_volumes(const Span<const GeometryComponent *> /*src_components*/,
                         GeometrySet & /*result*/)
{
  /* Not yet supported. Joining volume grids with the same name requires resampling of at least one
   * of the grids. The cell size of the resulting volume has to be determined somehow. */
}

static void join_component_type(const bke::GeometryComponent::Type component_type,
                                const Span<GeometrySet> src_geometry_sets,
                                const bke::AttributeFilter &attribute_filter,
                                GeometrySet &result)
{
  Vector<const GeometryComponent *> components;
  for (const GeometrySet &geometry_set : src_geometry_sets) {
    const GeometryComponent *component = geometry_set.get_component(component_type);
    if (component != nullptr && !component->is_empty()) {
      components.append(component);
    }
  }

  if (components.is_empty()) {
    return;
  }
  if (components.size() == 1) {
    result.add(*components.first());
    return;
  }

  switch (component_type) {
    case bke::GeometryComponent::Type::Instance:
      join_instances(components, attribute_filter, result);
      return;
    case bke::GeometryComponent::Type::Volume:
      join_volumes(components, result);
      return;
    default:
      break;
  }

  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>();
  instances->resize(components.size());
  instances->transforms_for_write().fill(float4x4::identity());
  MutableSpan<int> handles = instances->reference_handles_for_write();
  Map<const GeometryComponent *, int> handle_by_component;
  for (const int i : components.index_range()) {
    const GeometryComponent *component = components[i];
    handles[i] = handle_by_component.lookup_or_add_cb(component, [&]() {
      GeometrySet tmp_geo;
      tmp_geo.add(*components[i]);
      return instances->add_new_reference(bke::InstanceReference{tmp_geo});
    });
  }

  RealizeInstancesOptions options;
  options.keep_original_ids = true;
  options.realize_instance_attributes = false;
  options.attribute_filter = attribute_filter;
  GeometrySet joined_components = realize_instances(
      GeometrySet::from_instances(instances.release()), options);
  result.add(joined_components.get_component_for_write(component_type));
}

GeometrySet join_geometries(
    const Span<GeometrySet> geometries,
    const bke::AttributeFilter &attribute_filter,
    const std::optional<Span<GeometryComponent::Type>> &component_types_to_join)
{
  GeometrySet result;
  result.name = geometries.is_empty() ? "" : geometries[0].name;
  static const Array<GeometryComponent::Type> supported_types(
      {GeometryComponent::Type::Mesh,
       GeometryComponent::Type::PointCloud,
       GeometryComponent::Type::Instance,
       GeometryComponent::Type::Volume,
       GeometryComponent::Type::Curve,
       GeometryComponent::Type::GreasePencil,
       GeometryComponent::Type::Edit});

  const Span<GeometryComponent::Type> types_to_join = component_types_to_join.has_value() ?
                                                          *component_types_to_join :
                                                          Span<GeometryComponent::Type>(
                                                              supported_types);

  for (const GeometryComponent::Type type : types_to_join) {
    join_component_type(type, geometries, attribute_filter, result);
  }

  return result;
}

}  // namespace blender::geometry
