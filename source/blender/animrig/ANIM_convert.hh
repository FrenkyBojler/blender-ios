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
                                     ID &owner_id,
                                     bPoseChannel &pchan,
                                     const ChannelbagToFCurveMap &fcurves_by_rna_path,
                                     eRotationModes to_mode);

/**
 * Creates a map of RNA paths and the rotation FCurves associated with that rna path.
 * That means `rotation_euler` and `rotation_quaternion` will have different entries in the map.
 */
ChannelbagToFCurveMap build_rotation_fcurve_map(Action &action, slot_handle_t slot_handle);

}  // namespace animrig
}  // namespace blender
