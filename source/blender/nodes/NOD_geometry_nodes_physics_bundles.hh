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

class InfinitePlaneColliderBundle {
 public:
  static constexpr StringRefNull name = "Blender.InfinitePlaneCollider";
  static const FlatBundleTypePtr &get_bundle_type();
};

class DampingBundle {
 public:
  static constexpr StringRefNull name = "Blender.Damping";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinPositionBundle {
 public:
  static constexpr StringRefNull name = "Blender.PinPositionConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class PinRotationBundle {
 public:
  static constexpr StringRefNull name = "Blender.PinRotationConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodStretchShearBundle {
 public:
  static constexpr StringRefNull name = "Blender.RodStretchShearConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class RodBendTwistBundle {
 public:
  static constexpr StringRefNull name = "Blender.RodBendTwistConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

class EdgeLengthConstraintBundle {
 public:
  static constexpr StringRefNull name = "Blender.EdgeLengthConstraint";
  static const FlatBundleTypePtr &get_bundle_type();
};

}  // namespace blender::nodes::physics_bundles
