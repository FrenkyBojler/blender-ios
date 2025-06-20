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

/**
 * Status information about an externally loaded asset library listing, stored globally.
 *
 * Remote asset library downloading is handled in Python. This API allows storing status
 * information globally per URL. Asset UIs can then query the status and reflect it accordingly.
 *
 * Another important use is coordinating the Python side downloading with the C++ side loading.
 * The C++ asset library loading might have to wait for Python to be done downloading and
 * validating individual asset listing pages, and load in these new pages as they become ready.
 */
struct RemoteLibraryLoadingStatus {
 public:
  enum Status {
    Loading,
    Finished,
    Failure,
    Cancelled,
  };

 private:
  float timeout_;
  std::chrono::time_point<std::chrono::steady_clock> last_updated_time_point_;

  Status status_;
  std::optional<StringRef> failure_message_;

  /** Update the last update time point, effectively resetting the timout timer. */
  void reset_timeout();

 public:
  static void begin_loading(StringRef url, float timeout);
  /** Let the state know that the loading is still ongoing, resetting the timeout. */
  static void ping_still_loading(StringRef url);
  static void set_finished(StringRef url);
  static void set_failure(StringRef url, std::optional<StringRef> failure_message);

  static std::optional<StringRef> failure_message(StringRef url);
  static std::optional<RemoteLibraryLoadingStatus::Status> status(StringRef url);

  /**
   * \return True if the loading status switched to #Status::Failure due to timing out.
   */
  static bool handle_timeout(StringRef url);
};

}  // namespace blender::asset_system
