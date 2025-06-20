/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "BLT_translation.hh"

#include "BLI_map.hh"
#include "BLI_string_ref.hh"

#include "AS_remote_library.hh"

namespace blender::asset_system {

using UrlToLibraryStatusMap = Map<std::string /*url*/, asset_system::RemoteLibraryLoadingStatus>;

static UrlToLibraryStatusMap &library_to_status_map()
{
  static UrlToLibraryStatusMap map = UrlToLibraryStatusMap{};
  return map;
}

void RemoteLibraryLoadingStatus::reset_timeout()
{
  this->last_updated_time_point = std::chrono::steady_clock::now();
}

void remote_library_status_begin_loading(StringRef url, const float timeout)
{
  BLI_assert(timeout > 0.0f);

  RemoteLibraryLoadingStatus new_status{};
  new_status.timeout = timeout;
  new_status.status = RemoteLibraryLoadingStatus::Loading;
  new_status.reset_timeout();
  library_to_status_map().add_overwrite(url, new_status);
}

void remote_library_status_ping_still_loading(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status == RemoteLibraryLoadingStatus::Loading) {
      status->reset_timeout();
    }
  }
}

std::optional<RemoteLibraryLoadingStatus::Status> remote_library_status_get(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    return status->status;
  }
  return {};
}

void remote_library_status_set_finished(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    status->status = RemoteLibraryLoadingStatus::Finished;
    status->reset_timeout();
  }
}

void remote_library_status_set_failure(StringRef url, std::optional<StringRef> failure_message)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    status->status = RemoteLibraryLoadingStatus::Failure;
    status->failure_message = failure_message;
    status->reset_timeout();
  }
}

std::optional<StringRef> remote_library_status_failure_message(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status == RemoteLibraryLoadingStatus::Failure) {
      return status->failure_message;
    }
  }

  return {};
}

bool remote_library_status_handle_timeout(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status != RemoteLibraryLoadingStatus::Loading) {
      /* Only handle timeouts while loading. */
      return false;
    }

    std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() -
                                           status->last_updated_time_point;
    if (elapsed.count() >= status->timeout) {
      status->status = RemoteLibraryLoadingStatus::Failure;
      status->failure_message = RPT_("Asset system lost connection to downloader (timed out)");
      return true;
    }
  }

  return false;
}

}  // namespace blender::asset_system
