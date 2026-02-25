/* SPDX-FileCopyrightText: 2026 Blender Authors, Joshua Leung. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <string>

namespace blender {

struct AnimationBasePathChange {
  std::string src_basepath;
  std::string dst_basepath;
};

}  // namespace blender
