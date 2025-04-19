/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"

#include "BKE_geometry_set.hh"

namespace blender::bke {
class AttributeAccessor;
class Instances;
}  // namespace blender::bke

namespace blender::geometry {

bke::GeometrySet join_geometries(Span<bke::GeometrySet> geometries,
                                 const bke::AttributeFilter &attribute_filter,
                                 const std::optional<Span<bke::GeometryComponent::Type>>
                                     &component_types_to_join = std::nullopt);

/* In order to transfer ownership over geometries inside instances without copy this one will
 * perform joining with modification of some instance. This will ensure #target instance-references
 * will not be invalidated, but other #begin/end can be deleted if they will not unique. */
void join_instances_into(const bke::AttributeFilter &attribute_filter,
                         Span<const bke::Instances *> other_instances,
                         bke::Instances &target);

void join_attributes(Span<bke::AttributeAccessor> attribute_accessors,
                     const Map<StringRef, eCustomDataType> &attribute_types,
                     bke::AttrDomain src_domain,
                     bke::AttrDomain dst_domain,
                     bke::MutableAttributeAccessor dst_attributes);

}  // namespace blender::geometry
