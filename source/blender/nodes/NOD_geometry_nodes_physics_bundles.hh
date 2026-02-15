/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

#include "NOD_bundle_type_fwd.hh"

namespace blender::nodes::physics_bundles {

class GravityBundle {
 public:
  static constexpr StringRefNull name = "Blender.Gravity";
  static const FlatBundleTypePtr &get_bundle_type();
};

class ForceBundle {
 public:
  static constexpr StringRefNull name = "Blender.Force";
  static const FlatBundleTypePtr &get_bundle_type();
};

class TorqueBundle {
 public:
  static constexpr StringRefNull name = "Blender.Torque";
  static const FlatBundleTypePtr &get_bundle_type();
};

class ColliderBundle {
 public:
  static constexpr StringRefNull name = "Blender.Collider";
  static const FlatBundleTypePtr &get_bundle_type();
};

class DampingBundle {
 public:
  static constexpr StringRefNull name = "Blender.Damping";
  static const FlatBundleTypePtr &get_bundle_type();
};

class XPBDGeometryBundle {
 public:
  static constexpr StringRefNull name = "Blender.XPBDGeometry";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinnedPositionXPBDConstraintBundle {
 public:
  static constexpr StringRefNull name = "Blender.PinnedPositionXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinnedRotationXPBDConstraintBundle {
 public:
  static constexpr StringRefNull name = "Blender.PinnedRotationXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class InfiniteGroundPlaneBundle {
 public:
  static constexpr StringRefNull name = "Blender.InfiniteGroundPlane";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodStretchAndShearXPBDConstraintBundle {
 public:
  static constexpr StringRefNull name = "Blender.RodStretchAndShearXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodBendAndTwistXPBDConstraintBundle {
 public:
  static constexpr StringRefNull name = "Blender.RodBendAndTwistXPBDConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

}  // namespace blender::nodes::physics_bundles
