/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief C++ functions to deal with Armatures.
 */

#pragma once

struct bArmature;
struct Bone;
struct EditBone;
struct bPoseChannel;

namespace blender::animrig {

inline bool bone_is_visible(const bArmature *armature, const Bone *bone);

inline bool bone_is_visible_pchan(const bArmature *armature, const bPoseChannel *pchan);

inline bool bone_is_visible_editbone(const bArmature *armature, const EditBone *ebone);
       
}