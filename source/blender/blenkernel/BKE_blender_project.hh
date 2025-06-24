/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

namespace blender::bke {

class BlenderProject {
  /* The name and root path should either both be empty (indicating no active
   * project) or both be set (indicating an active project). */
  std::string name_;
  std::string root_path_;

 public:
  void init(StringRef name, StringRef root_path);
  void clear();

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

/**
 * Fetches the current Blender Project.
 */
BlenderProject &BKE_blender_project();

}  // namespace blender::bke
