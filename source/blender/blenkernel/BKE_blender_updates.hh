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
struct Layout;

struct BlenderVersion {
  int version;
  int patch;
  friend auto operator<=>(const BlenderVersion &a, const BlenderVersion &b) = default;
};
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

  std::string date() const;
};

bool check_for_available_updates(bContext &C,
                                 bool use_cache = true,
                                 bool ignore_skipped_versions = false);
Vector<const VersionUpdate *> available_updates();

void ignore_update(const VersionUpdate *update_info);
void ignore_all_updates();

}  // namespace blender::bke
