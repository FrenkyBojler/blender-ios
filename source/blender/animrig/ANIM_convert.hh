/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Functions to deal with transforming animation data.
 */

#include "BKE_action.hh"
#include "BLI_map.hh"

namespace blender {
struct FCurve;
struct ID;
struct bPoseChannel;

namespace animrig {

struct Channelbag;

/**
 * Provides a common interface to rotation properties of various structs.
 *
 * \note Christoph: While this could be the start to the Transformable API this is kept narrow in
 * scope and should in time be replaced by the properly designed real thing.
 */
class Rotateable {
  short *rotation_mode_;
  float *quaternion_;
  float *rot_axis_;
  float *rot_angle_;
  float *euler_;
  StringRefNull fcurve_group_name_;
  std::string rna_path_from_id;

 public:
  Rotateable(Object &object);
  Rotateable(bPoseChannel &pose_bone);

  eRotationModes get_rotation_mode() const
  {
    return eRotationModes(*rotation_mode_);
  }

  StringRefNull get_group_name() const
  {
    return fcurve_group_name_;
  }

  /* This returns a copy of the rotation mode. Returning a pointer is more complicated due to axis
   * angle being two properties. */
  float4 get_rotation(const eRotationModes for_mode) const
  {
    switch (for_mode) {
      case ROT_MODE_QUAT:
        return float4(quaternion_);

      case ROT_MODE_AXISANGLE:
        return float4(*rot_angle_, rot_axis_[0], rot_axis_[1], rot_axis_[2]);

      default:
        return float4(euler_[0], euler_[1], euler_[2], 0);
    }
  }

  std::string rna_path_to_property(const StringRef property_name) const;
};

/**
 * A non owning storage buffer for FCurves where they are sorted by `array_index`.
 */
class SortedFCurveBuffer {
  Vector<FCurve *> fcurves_;

 public:
  void insert_fcurve(FCurve &fcurve);
  Span<FCurve *> fcurves() const;
  /**
   * Returns the FCurve with the given array index from the buffer or a nullptr if that index
   * does not exist.
   */
  FCurve *get_fcurve_by_array_index(int array_index) const;
};

/* FCurves grouped by their RNA path. */
using RNAFCurveMap = Map<StringRefNull, SortedFCurveBuffer>;
/* For each Channelbag FCurves grouped by their RNA path. */
using ChannelbagToFCurveMap = Map<Channelbag *, RNAFCurveMap>;

/**
 * Convert any keyframe data for the given bone to the given rotation mode.
 *
 * \returns true if any animation data was modified.
 */
bool convert_pose_bone_rotation_keys(Main *bmain,
                                     const Rotateable &rotateable,
                                     const ChannelbagToFCurveMap &fcurves_by_rna_path,
                                     eRotationModes to_mode);

/**
 * Creates a map of RNA paths and the rotation FCurves associated with that rna path.
 * That means `rotation_euler` and `rotation_quaternion` will have different entries in the map.
 */
ChannelbagToFCurveMap build_rotation_fcurve_map(Action &action, slot_handle_t slot_handle);

/**
 * Bake all existing rotation fcurves that start with the given `base_rna_path`.
 */
void bake_rotation_fcurves(const ChannelbagToFCurveMap &channelbag_fcurve_map,
                           const Rotateable &rotateable);

}  // namespace animrig
}  // namespace blender
