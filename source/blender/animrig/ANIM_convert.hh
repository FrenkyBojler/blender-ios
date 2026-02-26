/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Functions to deal with transforming animation data.
 */

#include "BLI_map.hh"

namespace blender {
struct FCurve;
struct Object;
struct bPoseChannel;

namespace animrig {
struct Channelbag;
struct RotationFCurves {
  FCurve *fcurves[4];
};

/* Rotation FCurves sorted by the channelbag which they are in. */
using ChannelbagFCurveMap = Map<Channelbag *, RotationFCurves>;
/* FCurves sorted by RNA path. */
using RNAPathFCurveMap = Map<StringRef, ChannelbagFCurveMap>;

/**
 *
 */
void convert_pose_bone_rotation_keys(Main *bmain,
                                     Object &ob,
                                     bPoseChannel &pchan,
                                     RNAPathFCurveMap &fcurves_by_rna_path,
                                     eRotationModes to_mode);
}  // namespace animrig
}  // namespace blender
