/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <iterator>

#include "BKE_geometry_set.hh"

namespace blender::bke {
class Instances;
}

namespace blender::geometry {

bke::GeometrySet join_geometries(Span<bke::GeometrySet> geometries,
                                 const bke::AttributeFilter &attribute_filter,
                                 const std::optional<Span<bke::GeometryComponent::Type>>
                                     &component_types_to_join = std::nullopt);

join_attributes(const Span<bke::AttributeAccessor> attribute_accessors,
                     const Map<StringRef, eCustomDataType> &attribute_types,
                     const bke::AttrDomain src_domain,
                     const bke::AttrDomain dst_domain,
                     bke::MutableAttributeAccessor dst_attributes);

}  // namespace blender::geometry
