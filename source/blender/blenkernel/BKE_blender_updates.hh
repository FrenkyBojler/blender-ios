/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include <chrono>
#include <string>

namespace blender {
struct bContext;
}

namespace blender::bke {

/** Blender version. */
struct BlenderVersion {
  int version;
  int patch;
  friend auto operator<=>(const BlenderVersion &a, const BlenderVersion &b) = default;
};

/** Version update notification. */
struct VersionUpdate {
  int64_t build_size;
  std::string checksum_hash;
  std::string commit_hash;
  std::string description;
  std::string download_url;
  std::string cycle;
  bool is_lts;
  std::string platform;
  std::string release_notes_url;
  std::string timestamp;
  std::string version_str;
  std::time_t time;

  BlenderVersion version;

  /** UI string representation of #VersionUpdate::time. */
  std::string date() const;
};

/**
 * Check if there is any new available blender update.
 * \param use_cache: When set to false this request new updates from the server.
 * \param ignore_skipped_versions: When set to true this ignores previously ignored versions.
 */
bool check_for_available_updates(bContext &C,
                                 bool use_cache = true,
                                 bool ignore_skipped_versions = false);

/** Return available blender updates. */
Vector<const VersionUpdate *> available_updates();

/**
 * Ignores an specific update, the update and prior updates from the same type will not be listed
 * again.
 */
void ignore_update(const VersionUpdate *update_info);

/**
 * Ignores all available specific updates, see #ignore_update.
 */
void ignore_all_updates();

}  // namespace blender::bke
