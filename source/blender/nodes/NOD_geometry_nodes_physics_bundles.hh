/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_set.hh"

#include "FN_field.hh"

#include "NOD_bundle_type_fwd.hh"
#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

namespace blender::nodes::physics_bundles {

class GravityBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Gravity";
  static const FlatBundleTypePtr &get_bundle_type();
};

class ForceBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Force";
  static const FlatBundleTypePtr &get_bundle_type();
};

class TorqueBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Torque";
  static const FlatBundleTypePtr &get_bundle_type();
};

class ColliderBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Collider";
  static const FlatBundleTypePtr &get_bundle_type();
};

class DampingBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Damping";
  static const FlatBundleTypePtr &get_bundle_type();
};

class XPBDGeometryBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.XPBDGeometry";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinnedPositionXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.PinnedPositionXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinnedRotationXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.PinnedRotationXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class InfiniteGroundPlaneBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.InfiniteGroundPlane";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodStretchAndShearXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.RodStretchAndShearXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodBendAndTwistXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.RodBendAndTwistXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

}  // namespace blender::nodes::physics_bundles
