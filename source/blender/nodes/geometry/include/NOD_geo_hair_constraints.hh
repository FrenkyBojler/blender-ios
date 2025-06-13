/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_set.hh"

#include "GEO_hair_constraints.hh"

#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes::hair_constraints {

using geometry::hair_constraints::ConstraintType;

/* -------------------------------------------------------------------- */
/** \name Constraint Bundle Access
 * \{ */

const SocketInterfaceKey &constraint_type_to_socket_key(ConstraintType type);
ConstraintType socket_key_to_constraint_type(const SocketInterfaceKey &key);

struct ConstraintBundleItems {
  bke::GeometrySet stretch_constraints;
  bke::GeometrySet bending_constraints;
  bke::GeometrySet position_constraints;
  bke::GeometrySet rotation_constraints;
  bke::GeometrySet contact_constraints;
};

void set_constraints(BundlePtr &bundle_ptr, ConstraintType type, const bke::GeometrySet &geometry);
bke::GeometrySet lookup_constraints(const Bundle &bundle, ConstraintType type);
BundlePtr combine_constraint_bundle(const ConstraintBundleItems &items);
void separate_constraint_bundle(const Bundle &bundle, ConstraintBundleItems &items);

/** \} */

}  // namespace blender::nodes::hair_constraints
