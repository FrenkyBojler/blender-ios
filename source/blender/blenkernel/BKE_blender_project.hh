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

class BlenderProjectData {
  /* The name and root path should never be empty. */
  std::string name_;
  std::string root_path_;

 public:
  bool set_name(StringRef name);
  bool set_root_path(StringRef root_path);

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

class BlenderProject {
 public:
  /* If the project is not initialized, it has no data. */
  std::optional<BlenderProjectData> data;

  /**
   * Initialize a new Blender Project.
   */
  bool init(blender::StringRef name, blender::StringRef root_path);

  /**
   * Clear the current Blender Project.
   */
  void clear();
};

}  // namespace blender::bke

/**
 * Fetch the current Blender Project.
 */
blender::bke::BlenderProject &BKE_blender_project();
