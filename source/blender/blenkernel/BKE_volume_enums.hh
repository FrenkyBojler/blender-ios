/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <cstdint>

namespace blender {

enum VolumeGridType : int8_t {
  VolumeGridUnknown = 0,
  VolumeGridBoolean,
  VolumeGridFloat,
  VolumeGridDouble,
  VolumeGridInt,
  VolumeGridInT64,
  VolumeGridMask,
  VolumeGridVectorFloat,
  VolumeGridVectorDouble,
  VolumeGridVectorInt,
  VolumeGridPoints,
};

}  // namespace blender
