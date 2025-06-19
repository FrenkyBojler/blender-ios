/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include <chrono>
#include <optional>

#include "BLI_string_ref.hh"

namespace blender::asset_system {

struct AssetLibraryLoadingStatus {
  float timeout;
  std::chrono::time_point<std::chrono::steady_clock> last_updated_time_point;

  enum Status {
    Loading,
    Finished,
    Failure,
    Cancelled,
  } status;
  std::optional<StringRef> failure_message;

  /** Update the last update time point, effectively resetting the timout timer. */
  void touch();
};

void asset_library_status_ensure_loading(StringRef url, float timeout);
std::optional<AssetLibraryLoadingStatus::Status> asset_library_status_get(StringRef url);
void asset_library_status_set_finished(StringRef url);
void asset_library_status_set_failure(StringRef url, std::optional<StringRef> failure_message);

void asset_library_status_handle_timeout(StringRef url);

}  // namespace blender::asset_system
