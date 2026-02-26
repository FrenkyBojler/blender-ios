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

/* Rotation FCurves of one entity. The last index is a nullptr for euler. */
struct RotationFCurves {
  FCurve *fcurves[4];
};

/* Rotation FCurves sorted by the channelbag which they are in. */
using ChannelbagFCurveMap = Map<Channelbag *, RotationFCurves>;
/* FCurves sorted by their RNA path. */
using RNAPathFCurveMap = Map<StringRef, ChannelbagFCurveMap>;

/**
 * Convert any keyframe data for the given bone to the given rotation mode.
 *
 * \returns true if any animation data was modified.
 */
bool convert_pose_bone_rotation_keys(Main *bmain,
                                     ID &owner_id,
                                     bPoseChannel &pchan,
                                     const RNAPathFCurveMap &fcurves_by_rna_path,
                                     eRotationModes to_mode);

/**
 *
 */
void build_rotation_fcurve_map(RNAPathFCurveMap &r_pchan_rotations,
                               Action &action,
                               slot_handle_t slot_handle);

}  // namespace animrig
}  // namespace blender
